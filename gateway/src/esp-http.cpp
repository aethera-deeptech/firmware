#include "esp-http.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <string.h>

namespace wattlink {

namespace {

// The time that each step of the exchange may take. The client of the backend gives a request up after 40 seconds, and these keep the usual case below that.
constexpr uint32_t kConnectTimeoutMs = 8000;
constexpr unsigned long kHandshakeTimeoutMs = 10000;
constexpr uint32_t kReadTimeoutMs = 8000;
constexpr uint32_t kSocketTimeoutSeconds = 8;

constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;

// Receives the body of the answer from HTTPClient, which decodes a chunked body before it gives it here. It keeps what fits and counts the rest.
class BodySink : public Stream {
 public:
  typedef bool (*Append)(void *context, const uint8_t *data, size_t length);
  BodySink(Append append, void *context) : append_(append), context_(context) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    // Returning the full length keeps the transfer going, so the connection is read to its end even when the body is longer than the buffer.
    append_(context_, data, length);
    return length;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

 private:
  Append append_;
  void *context_;
};

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
  xSemaphoreTake(lock_, portMAX_DELAY);
  uint8_t expected = Working;
  if (!state_.compare_exchange_strong(expected, Abandoned)) {
    // A request that the task has not taken yet, or one that has ended, is dropped at once.
    expected = Requested;
    if (!state_.compare_exchange_strong(expected, Idle)) {
      expected = Finished;
      state_.compare_exchange_strong(expected, Idle);
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

  WiFiClientSecure client;
  client.setCACertBundle(caBundle_);
  client.setHandshakeTimeout(kHandshakeTimeoutMs);
  client.setTimeout(kSocketTimeoutSeconds);

  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(kConnectTimeoutMs);
  http.setTimeout(kReadTimeoutMs);
  if (!http.begin(client, host_, port_, path_, true)) {
    deliver(HttpFailure::Other, 0, 0);
    return;
  }
  static const char *kHeaders[] = {"Retry-After"};
  http.collectHeaders(kHeaders, 1);
  if (hasBearer_) {
    http.addHeader("Authorization", String("Bearer ") + bearer_);
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
    const bool tls = client.lastError(detail, sizeof(detail)) != 0;
    HttpFailure failure = HttpFailure::Other;
    if (tls) {
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
  BodySink sink(
      [](void *context, const uint8_t *data, size_t length) { return static_cast<EspHttpPort *>(context)->appendBody(data, length); }, this);
  http.writeToStream(&sink);
  http.end();
  deliver(HttpFailure::None, status, retryAfter);
}

}  // namespace wattlink
