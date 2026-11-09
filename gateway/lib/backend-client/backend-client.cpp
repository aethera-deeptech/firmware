#include "backend-client.h"

#include <string.h>

namespace wattlink {

namespace {

constexpr size_t kPathPrefixLength = 18;  // "/device/v1/enroll/"
constexpr uint32_t kMaxBackoffShift = 6;  // 5 seconds doubled 6 times is above 300 seconds, which the cap holds

// Writes zeros that the compiler may not drop, for a key that was copied to the stack.
void wipe(char *data, size_t size) {
  volatile char *p = data;
  for (size_t i = 0; i < size; i++) {
    p[i] = 0;
  }
}

bool isLetterOrDigit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isHardwareIdChar(char c) {
  return isLetterOrDigit(c) || c == ':' || c == '_' || c == '-';
}

bool isFirmwareChar(char c) {
  return isLetterOrDigit(c) || c == '.' || c == '_' || c == '+' || c == '-';
}

// Collects text in a buffer and remembers whether it fitted.
class BufferOutput : public BatchOutput {
 public:
  BufferOutput(char *out, size_t capacity) : out_(out), capacity_(capacity), length_(0), ok_(true) {}

  bool write(const char *text, size_t length) override {
    if (!ok_ || length > capacity_ - length_) {
      ok_ = false;
      return false;
    }
    memcpy(out_ + length_, text, length);
    length_ += length;
    return true;
  }
  void text(const char *text) { write(text, strlen(text)); }

  size_t length() const { return length_; }
  bool ok() const { return ok_; }

 private:
  char *out_;
  size_t capacity_;
  size_t length_;
  bool ok_;
};

}  // namespace

BackendClient::BackendClient(HttpPort &http, ClientStore &store, BatchBuilder &builder, CommandRelay &relay, const ClientConfig &config)
    : http_(http),
      store_(store),
      builder_(builder),
      relay_(relay),
      config_(config),
      state_(ClientState::Stopped),
      pending_(Pending::None),
      inFlight_(false),
      startedMs_(0),
      hasDue_(false),
      dueMs_(0),
      failureCount_(0),
      consecutive401_(0),
      consecutive409_(0),
      consecutive422_(0),
      probing_(false),
      confirmed_(false),
      noClockReported_(false),
      codeExpiresMs_(0),
      clockSet_(false),
      clockEpoch_(0),
      clockMs_(0),
      sentResults_(0),
      bodyLength_(0),
      lastStatus_(0),
      batchesStored_(0),
      failures_(0),
      eventHead_(0),
      eventCount_(0) {
  claimCode_[0] = '\0';
  pollId_[0] = '\0';
  path_[0] = '\0';
  memset(events_, 0, sizeof(events_));
  memset(&response_, 0, sizeof(response_));
  memset(&answer_, 0, sizeof(answer_));
}

bool BackendClient::configValid() const {
  if (config_.hardwareId == nullptr || config_.sha256 == nullptr) {
    return false;
  }
  const size_t idLength = strnlen(config_.hardwareId, kHardwareIdMaxLength + 1);
  if (idLength < kHardwareIdMinLength || idLength > kHardwareIdMaxLength) {
    return false;
  }
  for (size_t i = 0; i < idLength; i++) {
    if (!isHardwareIdChar(config_.hardwareId[i])) {
      return false;
    }
  }
  if (config_.firmware != nullptr) {
    const size_t length = strnlen(config_.firmware, kFirmwareMaxLength + 1);
    if (length == 0 || length > kFirmwareMaxLength) {
      return false;
    }
    for (size_t i = 0; i < length; i++) {
      if (!isFirmwareChar(config_.firmware[i])) {
        return false;
      }
    }
  }
  return true;
}

bool BackendClient::begin(uint32_t nowMs) {
  if (!configValid()) {
    state_ = ClientState::Stopped;
    emit(ClientEventKind::ConfigInvalid);
    return false;
  }
  char key[kApiKeySize];
  const bool enrolled = store_.enrolled();
  const bool haveKey = enrolled && store_.loadKey(key);
  wipe(key, sizeof(key));
  inFlight_ = false;
  pending_ = Pending::None;
  confirmed_ = false;
  probing_ = false;
  resetFailures();
  if (enrolled && haveKey) {
    state_ = ClientState::Running;
    setDue(nowMs, 0);
  } else {
    // A flag without a key cannot be used, because the backend knows another key or none.
    if (enrolled) {
      store_.setEnrolled(false);
    }
    state_ = ClientState::NeedEnroll;
    setDue(nowMs, 0);
  }
  return true;
}

bool BackendClient::due(uint32_t nowMs) const {
  return !hasDue_ || static_cast<int32_t>(nowMs - dueMs_) >= 0;
}

void BackendClient::setDue(uint32_t nowMs, uint32_t delayMs) {
  hasDue_ = true;
  dueMs_ = nowMs + delayMs;
}

void BackendClient::setUnixTime(uint32_t nowMs, uint32_t epochSeconds) {
  if (epochSeconds < kMinUnixSeconds) {
    return;
  }
  clockSet_ = true;
  clockEpoch_ = epochSeconds;
  clockMs_ = nowMs;
}

void BackendClient::advanceClock(uint32_t nowMs) {
  if (!clockSet_) {
    return;
  }
  const uint32_t seconds = (nowMs - clockMs_) / 1000;
  clockEpoch_ += seconds;
  clockMs_ += seconds * 1000;
}

uint32_t BackendClient::unixTime(uint32_t nowMs) {
  advanceClock(nowMs);
  return clockSet_ ? clockEpoch_ : 0;
}

uint32_t BackendClient::claimSecondsLeft(uint32_t nowMs) const {
  if (state_ != ClientState::ShowCode) {
    return 0;
  }
  const int32_t left = static_cast<int32_t>(codeExpiresMs_ - nowMs);
  return left <= 0 ? 0 : (static_cast<uint32_t>(left) + 999) / 1000;
}

void BackendClient::backoff(uint32_t nowMs) {
  failureCount_++;
  failures_++;
  const uint32_t shift = failureCount_ - 1 < kMaxBackoffShift ? failureCount_ - 1 : kMaxBackoffShift;
  uint32_t delay = kBackoffFirstMs << shift;
  if (delay > kBackoffMaxMs) {
    delay = kBackoffMaxMs;
  }
  setDue(nowMs, delay);
}

void BackendClient::resetFailures() {
  failureCount_ = 0;
  consecutive401_ = 0;
  consecutive409_ = 0;
  consecutive422_ = 0;
}

void BackendClient::emit(ClientEventKind kind, int status, uint32_t value) {
  if (eventCount_ == kEventSlots) {
    eventHead_ = (eventHead_ + 1) % kEventSlots;
    eventCount_--;
  }
  ClientEvent &slot = events_[(eventHead_ + eventCount_) % kEventSlots];
  slot.kind = kind;
  slot.status = status;
  slot.value = value;
  eventCount_++;
}

bool BackendClient::takeEvent(ClientEvent *out) {
  if (eventCount_ == 0) {
    return false;
  }
  *out = events_[eventHead_];
  eventHead_ = (eventHead_ + 1) % kEventSlots;
  eventCount_--;
  return true;
}

void BackendClient::goNeedEnroll(uint32_t nowMs) {
  state_ = ClientState::NeedEnroll;
  store_.setEnrolled(false);
  probing_ = false;
  confirmed_ = false;
  claimCode_[0] = '\0';
  resetFailures();
  setDue(nowMs, 0);
}

bool BackendClient::requestNewEnroll(uint32_t nowMs) {
  if (state_ == ClientState::Stopped || inFlight_) {
    return false;
  }
  if (!store_.renewKey()) {
    return false;
  }
  goNeedEnroll(nowMs);
  return true;
}

bool BackendClient::startRequest(uint32_t nowMs, Pending kind, const HttpRequest &request) {
  if (!http_.begin(request)) {
    setDue(nowMs, kIdleCheckMs);
    return false;
  }
  pending_ = kind;
  inFlight_ = true;
  startedMs_ = nowMs;
  return true;
}

void BackendClient::update(uint32_t nowMs, bool linkUp) {
  advanceClock(nowMs);
  if (inFlight_) {
    if (http_.poll(&response_)) {
      inFlight_ = false;
      finish(nowMs);
    } else if (nowMs - startedMs_ >= kRequestWatchdogMs) {
      http_.cancel();
      inFlight_ = false;
      memset(&response_, 0, sizeof(response_));
      response_.failure = HttpFailure::Timeout;
      finish(nowMs);
    }
    return;
  }
  if (state_ == ClientState::ShowCode && static_cast<int32_t>(nowMs - codeExpiresMs_) >= 0) {
    emit(ClientEventKind::CodeExpired);
    goNeedEnroll(nowMs);
  }
  if (!linkUp || !due(nowMs)) {
    return;
  }
  switch (state_) {
    case ClientState::NeedEnroll:
      startEnroll(nowMs);
      break;
    case ClientState::ShowCode:
      startPoll(nowMs);
      break;
    case ClientState::Running:
      startBatch(nowMs);
      break;
    case ClientState::Stopped:
    case ClientState::Refused:
      break;
  }
}

void BackendClient::finish(uint32_t nowMs) {
  const Pending kind = pending_;
  pending_ = Pending::None;
  if (response_.bodyLength > kMaxAnswerBytes) {
    response_.bodyLength = kMaxAnswerBytes;
    response_.truncated = true;
  }
  lastStatus_ = response_.failure == HttpFailure::None ? response_.status : 0;
  if (response_.failure != HttpFailure::None) {
    emit(ClientEventKind::RequestFailed, 0, static_cast<uint32_t>(response_.failure));
    backoff(nowMs);
    return;
  }
  // An answer that is not a 401 ends a run of 401 answers, and the same holds for 409 and 422.
  if (response_.status != 401) {
    consecutive401_ = 0;
  }
  if (response_.status != 409) {
    consecutive409_ = 0;
  }
  if (response_.status != 422) {
    consecutive422_ = 0;
  }
  switch (kind) {
    case Pending::Enroll:
      onEnrollAnswer(nowMs);
      break;
    case Pending::Poll:
      onPollAnswer(nowMs);
      break;
    case Pending::Readings:
      onReadingsAnswer(nowMs);
      break;
    case Pending::None:
      break;
  }
}

void BackendClient::waitForRetryAfter(uint32_t nowMs) {
  ErrorAnswer error;
  parseErrorAnswer(response_.body, response_.bodyLength, &error);
  uint32_t seconds = error.retryAfterSeconds > response_.retryAfterSeconds ? error.retryAfterSeconds : response_.retryAfterSeconds;
  if (seconds < 1) {
    seconds = 1;
  }
  if (seconds > kRetryAfterMaxSeconds) {
    seconds = kRetryAfterMaxSeconds;
  }
  emit(ClientEventKind::RateLimited, response_.status, seconds);
  setDue(nowMs, seconds * 1000);
}

// ---- Enroll

void BackendClient::startEnroll(uint32_t nowMs) {
  char key[kApiKeySize];
  if (!store_.ensureKey(key)) {
    wipe(key, sizeof(key));
    emit(ClientEventKind::KeyMissing);
    backoff(nowMs);
    return;
  }
  uint8_t digest[kSha256Size];
  const bool hashed = config_.sha256(config_.sha256Context, reinterpret_cast<const uint8_t *>(key), strlen(key), digest);
  wipe(key, sizeof(key));
  if (!hashed) {
    emit(ClientEventKind::KeyMissing);
    backoff(nowMs);
    return;
  }
  BufferOutput out(body_, sizeof(body_));
  out.text("{\"hardwareId\":\"");
  out.text(config_.hardwareId);
  if (config_.firmware != nullptr) {
    out.text("\",\"firmware\":\"");
    out.text(config_.firmware);
  }
  out.text("\",\"keyHash\":\"");
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < kSha256Size; i++) {
    const char pair[2] = {kHex[digest[i] >> 4], kHex[digest[i] & 0x0F]};
    out.write(pair, 2);
  }
  out.text("\"}");
  if (!out.ok()) {
    emit(ClientEventKind::BodyError);
    backoff(nowMs);
    return;
  }
  bodyLength_ = out.length();
  HttpRequest request = {HttpMethod::Post, "/device/v1/enroll", nullptr, body_, bodyLength_};
  startRequest(nowMs, Pending::Enroll, request);
}

void BackendClient::onEnrollAnswer(uint32_t nowMs) {
  const int status = response_.status;
  if (status == 201) {
    EnrollAnswer answer;
    if (!parseEnrollAnswer(response_.body, response_.bodyLength, &answer)) {
      emit(ClientEventKind::BadAnswer, status);
      backoff(nowMs);
      return;
    }
    memcpy(claimCode_, answer.claimCode, sizeof(claimCode_));
    memcpy(pollId_, answer.pollId, sizeof(pollId_));
    uint32_t seconds = answer.expiresInSeconds;
    if (seconds < 1) {
      seconds = 1;
    }
    if (seconds > kMaxClaimSeconds) {
      seconds = kMaxClaimSeconds;
    }
    codeExpiresMs_ = nowMs + seconds * 1000;
    state_ = ClientState::ShowCode;
    resetFailures();
    setDue(nowMs, kEnrollPollMs);
    emit(ClientEventKind::CodeShown, status, seconds);
    return;
  }
  if (status == 429) {
    waitForRetryAfter(nowMs);
    return;
  }
  emit(ClientEventKind::RequestFailed, status, 0);
  backoff(nowMs);
}

void BackendClient::startPoll(uint32_t nowMs) {
  memcpy(path_, "/device/v1/enroll/", kPathPrefixLength);
  strncpy(path_ + kPathPrefixLength, pollId_, sizeof(path_) - kPathPrefixLength - 1);
  path_[sizeof(path_) - 1] = '\0';
  HttpRequest request = {HttpMethod::Get, path_, nullptr, nullptr, 0};
  startRequest(nowMs, Pending::Poll, request);
}

void BackendClient::onPollAnswer(uint32_t nowMs) {
  const int status = response_.status;
  if (status == 202) {
    failureCount_ = 0;
    setDue(nowMs, kEnrollPollMs);
    return;
  }
  if (status == 200 || status == 404) {
    // The answer 200 says that the owner typed the code. The answer 404 comes when the answer 200 was lost, or when the code expired and was removed, so the
    // first batch tests the key. The backend answers 401 to it when the hub was never claimed, and then the hub asks for a new code.
    if (status == 200) {
      store_.setEnrolled(true);
      confirmed_ = true;
      probing_ = false;
    } else {
      probing_ = true;
    }
    state_ = ClientState::Running;
    claimCode_[0] = '\0';
    resetFailures();
    setDue(nowMs, 0);
    emit(ClientEventKind::Claimed, status);
    return;
  }
  if (status == 410) {
    emit(ClientEventKind::CodeExpired, status);
    goNeedEnroll(nowMs);
    return;
  }
  if (status == 429) {
    waitForRetryAfter(nowMs);
    return;
  }
  emit(ClientEventKind::RequestFailed, status, 0);
  backoff(nowMs);
}

// ---- Readings

void BackendClient::startBatch(uint32_t nowMs) {
  const uint32_t epoch = unixTime(nowMs);
  const size_t results = relay_.resultCount();
  const BatchPrepare prepared = builder_.prepare(nowMs, epoch, relay_.results(), results);
  if (prepared == BatchPrepare::ClockNotSet) {
    if (!noClockReported_) {
      noClockReported_ = true;
      emit(ClientEventKind::NoClock);
    }
    setDue(nowMs, kIdleCheckMs);
    return;
  }
  if (prepared != BatchPrepare::Ready) {
    setDue(nowMs, kIdleCheckMs);
    return;
  }
  noClockReported_ = false;
  const size_t size = builder_.bodySize();
  BufferOutput out(body_, sizeof(body_));
  if (size > sizeof(body_) || !builder_.render(out) || out.length() != size) {
    emit(ClientEventKind::BodyError, 0, static_cast<uint32_t>(size));
    backoff(nowMs);
    return;
  }
  bodyLength_ = size;
  char key[kApiKeySize];
  if (!store_.loadKey(key)) {
    wipe(key, sizeof(key));
    emit(ClientEventKind::KeyMissing);
    backoff(nowMs);
    return;
  }
  HttpRequest request = {HttpMethod::Post, "/device/v1/readings", key, body_, bodyLength_};
  const bool started = startRequest(nowMs, Pending::Readings, request);
  wipe(key, sizeof(key));
  if (started) {
    builder_.markSent(nowMs);
    sentResults_ = results;
  }
}

void BackendClient::onReadingsAnswer(uint32_t nowMs) {
  const int status = response_.status;
  if (status == 200) {
    if (!parseReadingsAnswer(response_.body, response_.bodyLength, &answer_, response_.truncated)) {
      emit(ClientEventKind::BadAnswer, status);
      backoff(nowMs);
      return;
    }
    const size_t samples = builder_.sampleCount();
    builder_.onAck(answer_.ackSeq);
    relay_.dropResults(sentResults_);
    sentResults_ = 0;
    for (size_t i = 0; i < answer_.commandCount; i++) {
      const RelayCommandAnswer &command = answer_.commands[i];
      relay_.submit(nowMs, answer_.serverTime, command.id, command.nodeId, command.on, command.expiresAt);
    }
    if (answer_.commandCount == 0) {
      relay_.forgetEnded(answer_.serverTime);
    }
    if (!confirmed_) {
      store_.setEnrolled(true);
      confirmed_ = true;
    }
    probing_ = false;
    batchesStored_++;
    resetFailures();
    emit(ClientEventKind::BatchStored, status, static_cast<uint32_t>(samples));
    setDue(nowMs, builder_.waitingSamples() >= kBacklogSamples ? kMinBatchGapMs : kSendIntervalMs);
    return;
  }
  if (status == 409) {
    ErrorAnswer error;
    if (!parseErrorAnswer(response_.body, response_.bodyLength, &error) || !error.hasExpectedSeq) {
      emit(ClientEventKind::BadAnswer, status);
      backoff(nowMs);
      return;
    }
    builder_.onExpectedSeq(error.expectedSeq);
    // The backend answered, so the link works, and the failures before it do not count for the pause.
    failureCount_ = 0;
    emit(ClientEventKind::Conflict, status, error.expectedSeq);
    consecutive409_++;
    if (consecutive409_ >= kConflictLimit) {
      consecutive409_ = 0;
      backoff(nowMs);
    } else {
      setDue(nowMs, kMinBatchGapMs);
    }
    return;
  }
  if (status == 401) {
    consecutive401_++;
    if (probing_) {
      goNeedEnroll(nowMs);
      return;
    }
    if (consecutive401_ >= kKeyRefusedLimit) {
      state_ = ClientState::Refused;
      emit(ClientEventKind::KeyRefused, status);
      return;
    }
    backoff(nowMs);
    return;
  }
  if (status == 422) {
    consecutive422_++;
    if (consecutive422_ >= kBadBatchLimit) {
      consecutive422_ = 0;
      const size_t samples = builder_.sampleCount();
      builder_.onAck(builder_.seq());
      emit(ClientEventKind::BatchDropped, status, static_cast<uint32_t>(samples));
      setDue(nowMs, kMinBatchGapMs);
    } else {
      emit(ClientEventKind::RequestFailed, status, 0);
      backoff(nowMs);
    }
    return;
  }
  if (status == 429) {
    waitForRetryAfter(nowMs);
    return;
  }
  emit(ClientEventKind::RequestFailed, status, 0);
  backoff(nowMs);
}

}  // namespace wattlink
