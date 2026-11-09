#include "esp-http.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace wattlink {

namespace {

// The time that each step of the exchange may take. The client of the backend gives a request up after 40 seconds, and these keep the usual case below that.
constexpr uint32_t kConnectTimeoutMs = 8000;
// WiFiClientSecure takes this value in seconds, and so does its socket timeout.
constexpr unsigned long kHandshakeTimeoutSeconds = 10;
constexpr uint32_t kSocketTimeoutSeconds = 8;
constexpr uint32_t kReadTimeoutMs = 8000;
// The whole body must arrive in this time, because a connection that stalls in the middle of a body would hold the task for ever.
constexpr uint32_t kBodyDeadlineMs = 8000;
// The least free block that a TLS connection needs (the buffers of mbedTLS and the objects of HTTPClient).
constexpr size_t kMinFreeBlockBytes = 48 * 1024;
constexpr size_t kBearerPrefixLength = 7;  // "Bearer "
constexpr int kMaxTrailerLines = 16;

constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;

uint32_t parseSeconds(const String &text) {
  uint32_t value = 0;
  for (size_t i = 0; i < text.length(); i++) {
    const char c = text[i];
    if (c < '0' || c > '9') {
      return 0;
    }
    value = value * 10 + static_cast<uint32_t>(c - '0');
    if (value > 100000) {
      return 100000;
    }
  }
  return value;
}

}  // namespace

EspHttpPort::EspHttpPort(const char *host, uint16_t port, const uint8_t *caBundle)
    : host_(host),
      port_(port),
      caBundle_(caBundle),
      state_(Idle),
      task_(nullptr),
      lock_(nullptr),
      method_(HttpMethod::Get),
      hasBearer_(false),
      body_(nullptr),
      bodyLength_(0),
      response_(nullptr),
      received_(0) {
  path_[0] = '\0';
  bearer_[0] = '\0';
}

bool EspHttpPort::start() {
  if (task_ != nullptr) {
    return true;
  }
  lock_ = xSemaphoreCreateMutexStatic(&lockBuffer_);
  if (lock_ == nullptr) {
    return false;
  }
  task_ = xTaskCreateStaticPinnedToCore(&EspHttpPort::taskEntry, "backend-http", sizeof(stack_) / sizeof(StackType_t), this, kTaskPriority, stack_,
                                        &taskBuffer_, kTaskCore);
  return task_ != nullptr;
}

bool EspHttpPort::begin(const HttpRequest &request, HttpResponse *response) {
  if (task_ == nullptr || response == nullptr || request.path == nullptr || state_.load() != Idle) {
    return false;
  }
  if (strlen(request.path) >= kPathBytes) {
    return false;
  }
  if (request.bearer != nullptr && strnlen(request.bearer, kApiKeySize) >= kApiKeySize) {
    return false;
  }
  method_ = request.method;
  strcpy(path_, request.path);
  hasBearer_ = request.bearer != nullptr;
  if (hasBearer_) {
    strcpy(bearer_, request.bearer);
  } else {
    bearer_[0] = '\0';
  }
  body_ = request.body;
  bodyLength_ = request.bodyLength;
  response_ = response;
  received_ = 0;
  state_.store(Requested);
  xTaskNotifyGive(task_);
  return true;
}

bool EspHttpPort::poll() {
  uint8_t expected = Finished;
  // The answer is in the response of the caller when the state is Finished, and the port is free again afterwards.
  return state_.compare_exchange_strong(expected, Idle);
}

void EspHttpPort::cancel() {
  if (lock_ == nullptr) {
    return;
  }
  xSemaphoreTake(lock_, portMAX_DELAY);
  // The task takes a request without the lock, so the state may change between the look and the exchange. The loop looks again then, until the exchange
  // works, and it ends with the request given up or dropped.
  while (true) {
    uint8_t seen = state_.load();
    uint8_t next;
    if (seen == Working) {
      next = Abandoned;
    } else if (seen == Requested || seen == Finished) {
      next = Idle;  // a request that the task has not taken yet, or one that has ended, is dropped at once
    } else {
      break;
    }
    if (state_.compare_exchange_strong(seen, next)) {
      break;
    }
  }
  xSemaphoreGive(lock_);
}

void EspHttpPort::taskEntry(void *self) {
  static_cast<EspHttpPort *>(self)->loop();
}

void EspHttpPort::loop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uint8_t expected = Requested;
    if (!state_.compare_exchange_strong(expected, Working)) {
      continue;  // the request was cancelled before the task took it
    }
    execute();
    // The bearer is not needed any more.
    volatile char *key = bearer_;
    for (size_t i = 0; i < sizeof(bearer_); i++) {
      key[i] = 0;
    }
    expected = Working;
    if (!state_.compare_exchange_strong(expected, Finished)) {
      state_.store(Idle);  // given up by cancel(), and nobody reads the answer
    }
  }
}

bool EspHttpPort::deliver(HttpFailure failure, int status, uint32_t retryAfterSeconds) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  const bool wanted = state_.load() == Working;
  if (wanted) {
    response_->failure = failure;
    response_->status = status;
    response_->retryAfterSeconds = retryAfterSeconds;
    response_->bodyLength = received_ < kMaxAnswerBytes ? received_ : kMaxAnswerBytes;
    response_->truncated = received_ > kMaxAnswerBytes;
  }
  xSemaphoreGive(lock_);
  return wanted;
}

bool EspHttpPort::appendBody(const uint8_t *data, size_t length) {
  xSemaphoreTake(lock_, portMAX_DELAY);
  const bool wanted = state_.load() == Working;
  if (wanted) {
    if (received_ < kMaxAnswerBytes) {
      const size_t room = kMaxAnswerBytes - received_;
      memcpy(response_->body + received_, data, length < room ? length : room);
    }
    received_ += length;
  }
  xSemaphoreGive(lock_);
  return wanted;
}

int EspHttpPort::readSome(WiFiClient &stream, uint8_t *out, size_t max, uint32_t startMs, HttpFailure *why) {
  while (true) {
    if (state_.load() != Working) {
      *why = HttpFailure::Other;
      return -1;
    }
    const int available = stream.available();
    if (available > 0) {
      const size_t want = max < static_cast<size_t>(available) ? max : static_cast<size_t>(available);
      const int count = stream.read(out, want);
      if (count > 0) {
        return count;
      }
    } else if (!stream.connected()) {
      return 0;
    }
    if (millis() - startMs > kBodyDeadlineMs) {
      *why = HttpFailure::Timeout;
      return -1;
    }
    delay(2);
  }
}

int EspHttpPort::readLine(WiFiClient &stream, char *line, size_t capacity, uint32_t startMs, HttpFailure *why) {
  size_t length = 0;
  while (true) {
    uint8_t c = 0;
    const int count = readSome(stream, &c, 1, startMs, why);
    if (count < 0) {
      return -1;
    }
    if (count == 0) {
      *why = HttpFailure::Connect;
      return -1;
    }
    if (c == '\n') {
      line[length] = '\0';
      return static_cast<int>(length);
    }
    if (c != '\r' && length + 1 < capacity) {
      line[length++] = static_cast<char>(c);
    }
  }
}

HttpFailure EspHttpPort::readBody(WiFiClient &stream, int32_t contentLength, bool chunked) {
  const uint32_t startMs = millis();
  uint8_t buffer[256];
  HttpFailure why = HttpFailure::Other;

  if (!chunked) {
    int32_t remaining = contentLength;
    while (contentLength < 0 || remaining > 0) {
      const size_t want = (contentLength < 0 || remaining > static_cast<int32_t>(sizeof(buffer))) ? sizeof(buffer) : static_cast<size_t>(remaining);
      const int count = readSome(stream, buffer, want, startMs, &why);
      if (count < 0) {
        return why;
      }
      if (count == 0) {
        // The end of a body without a length is the end of the connection. A body with a length that ends early is cut short.
        return contentLength < 0 ? HttpFailure::None : HttpFailure::Connect;
      }
      if (!appendBody(buffer, static_cast<size_t>(count))) {
        return HttpFailure::Other;
      }
      if (contentLength >= 0) {
        remaining -= count;
      }
    }
    return HttpFailure::None;
  }

  while (true) {
    char line[24];
    if (readLine(stream, line, sizeof(line), startMs, &why) < 0) {
      return why;
    }
    char *end = nullptr;
    unsigned long size = strtoul(line, &end, 16);
    if (end == line) {
      return HttpFailure::Other;
    }
    if (size == 0) {
      // The trailer section ends with an empty line.
      for (int i = 0; i < kMaxTrailerLines; i++) {
        const int length = readLine(stream, line, sizeof(line), startMs, &why);
        if (length < 0) {
          return why;
        }
        if (length == 0) {
          return HttpFailure::None;
        }
      }
      return HttpFailure::Other;
    }
    while (size > 0) {
      const size_t want = size > sizeof(buffer) ? sizeof(buffer) : static_cast<size_t>(size);
      const int count = readSome(stream, buffer, want, startMs, &why);
      if (count < 0) {
        return why;
      }
      if (count == 0) {
        return HttpFailure::Connect;
      }
      if (!appendBody(buffer, static_cast<size_t>(count))) {
        return HttpFailure::Other;
      }
      size -= static_cast<unsigned long>(count);
    }
    // The line end after the data of the chunk.
    if (readLine(stream, line, sizeof(line), startMs, &why) < 0) {
      return why;
    }
  }
}

void EspHttpPort::execute() {
  if (WiFi.status() != WL_CONNECTED) {
    deliver(HttpFailure::NoLink, 0, 0);
    return;
  }
  if (caBundle_ == nullptr) {
    // Without roots to check against there is no connection, and the check is never skipped.
    deliver(HttpFailure::Tls, 0, 0);
    return;
  }
  if (time(nullptr) < static_cast<time_t>(kMinUnixSeconds)) {
    // mbedTLS compares the dates of the certificates with the system time, so the handshake would fail whatever the server sends.
    deliver(HttpFailure::Clock, 0, 0);
    return;
  }
  if (ESP.getMaxAllocHeap() < kMinFreeBlockBytes) {
    deliver(HttpFailure::Memory, 0, 0);
    return;
  }

  WiFiClientSecure client;
  client.setCACertBundle(caBundle_);
  client.setHandshakeTimeout(kHandshakeTimeoutSeconds);
  client.setTimeout(kSocketTimeoutSeconds);

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kReadTimeoutMs);
  if (!http.begin(client, host_, port_, path_, true)) {
    deliver(HttpFailure::Other, 0, 0);
    return;
  }
  static const char *kHeaders[] = {"Retry-After", "Transfer-Encoding"};
  http.collectHeaders(kHeaders, 2);
  if (hasBearer_) {
    String authorization = String("Bearer ") + bearer_;
    // A String that could not grow is shorter than it should be, and a request with a cut key would only earn an answer 401.
    if (authorization.length() != kBearerPrefixLength + strlen(bearer_)) {
      http.end();
      deliver(HttpFailure::Memory, 0, 0);
      return;
    }
    http.addHeader("Authorization", authorization);
  }

  int status;
  if (method_ == HttpMethod::Post) {
    http.addHeader("Content-Type", "application/json");
    status = http.sendRequest("POST", const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(body_)), bodyLength_);
  } else {
    status = http.GET();
  }

  if (status < 0) {
    char detail[64];
    // The plain value -1 is a failure of the TCP connection or a timeout of the handshake, and a code below it is an error of mbedTLS.
    const int tlsError = client.lastError(detail, sizeof(detail));
    HttpFailure failure = HttpFailure::Other;
    if (tlsError < -1) {
      failure = HttpFailure::Tls;
    } else if (status == HTTPC_ERROR_READ_TIMEOUT) {
      failure = HttpFailure::Timeout;
    } else if (status == HTTPC_ERROR_CONNECTION_REFUSED || status == HTTPC_ERROR_CONNECTION_LOST || status == HTTPC_ERROR_NOT_CONNECTED) {
      failure = HttpFailure::Connect;
    }
    http.end();
    deliver(failure, 0, 0);
    return;
  }

  const uint32_t retryAfter = parseSeconds(http.header("Retry-After"));
  const bool chunked = http.header("Transfer-Encoding").indexOf("chunked") >= 0;
  const int32_t contentLength = http.getSize();
  HttpFailure bodyFailure = HttpFailure::None;
  // An answer 204 or 304 has no body, and neither has an informational answer.
  if (status != 204 && status != 304 && status >= 200) {
    bodyFailure = readBody(client, contentLength, chunked);
  }
  http.end();
  if (bodyFailure != HttpFailure::None) {
    deliver(bodyFailure, 0, 0);
    return;
  }
  deliver(HttpFailure::None, status, retryAfter);
}

}  // namespace wattlink
