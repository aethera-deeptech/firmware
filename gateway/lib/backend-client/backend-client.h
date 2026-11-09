#pragma once

#include <stddef.h>
#include <stdint.h>

#include "api-answer.h"
#include "batch-builder.h"
#include "command-relay.h"
#include "hkdf-sha256.h"
#include "settings-store.h"

namespace wattlink {

// The conversation of the hub with the backend. The contract is openapi.yaml of the backend (the routes under /device/v1) and its docs/firmware-guide.md.
// The client enrolls the hub and shows the claim code, polls until the owner has typed the code, sends the batches of readings, reads the answers, and hands
// the relay commands to the command relay. It opens no connection and has no clock of its own. The network is behind HttpPort, the key and the enrolled flag are
// behind ClientStore, and the time comes from outside. One task must call all of it, together with the sample store, the batch builder and the relay.

// The buffer that a batch is written into before it is sent. The batch builder gets this size as its budget, so every batch fits. The send runs on a copy of
// the batch, so the sample store is never read while the network works.
constexpr size_t kClientBodyBytes = 16384;

// The wait between two polls of an enrollment. The backend allows one poll every 3 seconds for the address of a hub and asks for 5 seconds with one hub.
constexpr uint32_t kEnrollPollMs = 5000;
// The wait between two batches when the store holds less than kBacklogSamples. A node sends a sample every 10 seconds, so a batch of 30 seconds holds 3
// samples of a node, which the backend needs to judge a motor.
constexpr uint32_t kSendIntervalMs = 30000;
constexpr size_t kBacklogSamples = 100;
// How often the client looks again when there is nothing to send yet, the clock is not set, or the batch builder asks to wait.
constexpr uint32_t kIdleCheckMs = 1000;
// The pause after a failure grows from the first value by doubling up to the largest, as the firmware guide says (5 seconds to 5 minutes).
constexpr uint32_t kBackoffFirstMs = 5000;
constexpr uint32_t kBackoffMaxMs = 300000;
// The wait that the answer 429 names is kept between 1 second and an hour.
constexpr uint32_t kRetryAfterMaxSeconds = 3600;
// A request that has not ended after this time is cancelled and counts as a failure.
constexpr uint32_t kRequestWatchdogMs = 40000;
// The answer 401 this many times in a row, with no other answer between, means that the key is refused.
constexpr uint32_t kKeyRefusedLimit = 3;
// The answer 422 this many times in a row for one batch means that the batch cannot be fixed, so its samples are dropped.
constexpr uint32_t kBadBatchLimit = 3;
// The answer 409 this many times in a row is treated as a failure, because one answer should have been enough to learn the number.
constexpr uint32_t kConflictLimit = 5;
// A claim code that the backend lets live longer than this is shown for this long at the most.
constexpr uint32_t kMaxClaimSeconds = 3600;

// A hub that is probing its key (the poll answered 404) gives up after this time when no batch could be sent, and asks for a new code.
constexpr uint32_t kProbeWaitMs = 60000;

constexpr size_t kHardwareIdMaxLength = 64;
constexpr size_t kHardwareIdMinLength = 6;
constexpr size_t kFirmwareMaxLength = 32;

enum class HttpMethod : uint8_t { Get, Post };

enum class HttpFailure : uint8_t {
  None,     // an answer came, whatever its status
  NoLink,   // the Wi-Fi is down
  Connect,  // no connection to the host
  Tls,      // the handshake or the certificate failed
  Timeout,  // no answer in time, or the request was cancelled
  Other,
};

struct HttpRequest {
  HttpMethod method;
  const char *path;    // starts with a slash, and is valid until begin() returns
  const char *bearer;  // the key text for the header Authorization, or null. The port copies it and wipes its copy when the request ends, because it is valid until begin() returns
  const char *body;    // null for a GET. It stays valid until poll() has returned true
  size_t bodyLength;
};

struct HttpResponse {
  HttpFailure failure;
  int status;                  // 0 when there is no answer
  uint32_t retryAfterSeconds;  // the header Retry-After, or 0
  size_t bodyLength;           // the bytes in body, at most kMaxAnswerBytes
  bool truncated;              // the body was longer than the buffer
  char body[kMaxAnswerBytes];
};

// One request at a time. On the chip the request runs in a task of its own, so poll() only looks at a flag.
class HttpPort {
 public:
  virtual ~HttpPort() = default;
  // Starts a request. Returns false when it cannot start now (busy, no memory, no link).
  virtual bool begin(const HttpRequest &request) = 0;
  // Returns true when the request has ended, and then fills the response (failure None with a status, or a failure). Returns false while it runs.
  virtual bool poll(HttpResponse *response) = 0;
  // Stops the request that runs. The next begin() may start a new one.
  virtual void cancel() = 0;
};

// The key of the hub and the flag that says that the owner claimed the hub. They live in flash.
class ClientStore {
 public:
  virtual ~ClientStore() = default;
  virtual bool loadKey(char out[kApiKeySize]) = 0;
  // Loads the key, or makes one and stores it.
  virtual bool ensureKey(char out[kApiKeySize]) = 0;
  // Replaces the key with a new one, for a new enrollment.
  virtual bool renewKey() = 0;
  virtual bool enrolled() = 0;
  virtual bool setEnrolled(bool value) = 0;
};

// SHA-256 of a text, for the hash of the key that goes to the backend. The backend never sees the key.
typedef bool (*Sha256Fn)(void *context, const uint8_t *data, size_t length, uint8_t out[kSha256Size]);

struct ClientConfig {
  const char *hardwareId;  // 6 to 64 characters from A-Z a-z 0-9 : _ -
  const char *firmware;    // up to 32 characters from A-Z a-z 0-9 . _ + -, or null to leave it out
  Sha256Fn sha256;
  void *sha256Context;
};

enum class ClientState : uint8_t {
  Stopped,     // begin() was not called, or the configuration is not valid
  NeedEnroll,  // no code yet, so the next request asks for one
  ShowCode,    // the code is on the display and the client polls until it is claimed
  Running,     // the hub belongs to a business and sends batches
  Refused,     // the backend refused the key, so nothing is sent until requestNewEnroll()
};

enum class ClientEventKind : uint8_t {
  None,
  ConfigInvalid,  // the hardware id or the firmware text is not valid, so the client stopped
  CodeShown,      // a claim code is ready, see claimCode()
  CodeExpired,    // the code ran out, so the client asks for a new one
  Claimed,        // the owner typed the code
  ClaimUnknown,   // the poll answered 404, so the claim is not known and the first batch tests the key
  ProbeExpired,   // no batch could test the key in time, so the client asks for a new code
  BatchStored,    // value is the number of samples in the batch that the backend acknowledged
  Conflict,       // the answer 409, value is the number that the backend waits for
  RateLimited,    // the answer 429, value is the wait in seconds
  KeyRefused,     // the key is refused, so the client stopped
  BatchDropped,   // the backend refused a batch 3 times (400, 413, 415 or 422), so its samples were dropped, value is the number of samples
  RequestFailed,  // status is the HTTP status or 0, value is the failure kind
  BadAnswer,      // the answer could not be read
  NoClock,        // the batch waits because the Unix time is not set
  KeyMissing,     // the key could not be read or made
  BodyError,      // the batch could not be written into the buffer
};

struct ClientEvent {
  ClientEventKind kind;
  int status;
  uint32_t value;
};

class BackendClient {
 public:
  // The batch builder must have been made with kClientBodyBytes as its budget, and begin() refuses a builder with a larger one. The object holds the buffer of
  // the body and of the answer, about 26 KB, so it must be a static object and never on the stack of a task (like SampleStore).
  BackendClient(HttpPort &http, ClientStore &store, BatchBuilder &builder, CommandRelay &relay, const ClientConfig &config);

  BackendClient(const BackendClient &) = delete;
  BackendClient &operator=(const BackendClient &) = delete;

  // Checks the configuration (also the budget of the batch builder) and reads the enrolled flag. A hub that is enrolled starts to send, and any other hub starts to enroll. Returns false, and
  // stays stopped, when the configuration is not valid.
  bool begin(uint32_t nowMs);

  // Call it on every pass of loop(). linkUp is true when the Wi-Fi is joined. It starts at most one request and reads at most one answer.
  void update(uint32_t nowMs, bool linkUp);

  // The Unix time, which the chip gets from a time server. A batch needs it, so the client sends nothing while it is not set (the event NoClock says so once).
  // The time of the backend in an answer does not set it, because the first answer needs a batch. unixTime() returns 0 while the time is not set.
  void setUnixTime(uint32_t nowMs, uint32_t epochSeconds);
  uint32_t unixTime(uint32_t nowMs);

  // Throws the key away, makes a new one, and asks for a new claim code. For a deliberate action of the owner on the hub, and the only way out of Refused.
  // The samples that wait, the batch that was sent and not acknowledged, the results of relay commands and the commands that wait all stay, because the backend
  // may know the hub already. So the gesture is meant for the same owner. A hub that goes to another business needs the stores cleared first (not built).
  // The flag is cleared before the key is replaced. Returns false, and changes nothing, when a request runs, when the client is stopped or when the flag cannot be
  // cleared. Returns false too when the flash refused the new key, and then the client still asks for a code, with its old key.
  bool requestNewEnroll(uint32_t nowMs);

  ClientState state() const { return state_; }
  // The claim code while state() is ShowCode, and an empty text otherwise.
  const char *claimCode() const { return state_ == ClientState::ShowCode ? claimCode_ : ""; }
  // The seconds until the code expires, or 0.
  uint32_t claimSecondsLeft(uint32_t nowMs) const;
  bool busy() const { return inFlight_; }
  // The status of the last answer, and the counters for the log.
  int lastStatus() const { return lastStatus_; }
  uint32_t batchesStored() const { return batchesStored_; }
  uint32_t failures() const { return failures_; }

  // Takes the oldest event that nobody read, or returns false. A few events are held, and the oldest is dropped when more come.
  bool takeEvent(ClientEvent *out);

 private:
  enum class Pending : uint8_t { None, Enroll, Poll, Readings };

  bool configValid() const;
  bool due(uint32_t nowMs) const;
  void setDue(uint32_t nowMs, uint32_t delayMs);
  void advanceClock(uint32_t nowMs);
  void backoff(uint32_t nowMs);
  void resetFailures();
  void emit(ClientEventKind kind, int status = 0, uint32_t value = 0);

  bool startRequest(uint32_t nowMs, Pending kind, const HttpRequest &request);
  void startEnroll(uint32_t nowMs);
  void startPoll(uint32_t nowMs);
  void startBatch(uint32_t nowMs);
  void finish(uint32_t nowMs);
  void onEnrollAnswer(uint32_t nowMs);
  void onPollAnswer(uint32_t nowMs);
  void onReadingsAnswer(uint32_t nowMs);
  void waitForRetryAfter(uint32_t nowMs);
  void goNeedEnroll(uint32_t nowMs);
  void endExpiry(uint32_t nowMs);

  HttpPort &http_;
  ClientStore &store_;
  BatchBuilder &builder_;
  CommandRelay &relay_;
  ClientConfig config_;

  ClientState state_;
  Pending pending_;
  bool inFlight_;
  uint32_t startedMs_;
  bool hasDue_;
  uint32_t dueMs_;
  uint32_t failureCount_;
  uint32_t consecutive401_;
  uint32_t consecutive409_;
  uint32_t consecutive422_;
  // True after an enrollment whose answer was lost, so the first batch tests the key. A refused key then means that the code was never claimed.
  bool probing_;
  uint32_t probeDeadlineMs_;
  // True when the code has run out and the last poll is on its way, so a claim in the last seconds is not lost.
  bool expiring_;
  // True once a batch or a claim has proven the key in this run, so the flag in flash is written once.
  bool confirmed_;
  bool noClockReported_;

  char claimCode_[kClaimCodeSize];
  char pollId_[kPollIdSize];
  uint32_t codeExpiresMs_;

  // The clock, in whole seconds from the base.
  bool clockSet_;
  uint32_t clockEpoch_;
  uint32_t clockMs_;

  size_t sentResults_;
  size_t bodyLength_;
  char path_[kPollIdSize + 24];

  int lastStatus_;
  uint32_t batchesStored_;
  uint32_t failures_;

  static constexpr size_t kEventSlots = 4;
  ClientEvent events_[kEventSlots];
  size_t eventHead_;
  size_t eventCount_;

  HttpResponse response_;
  ReadingsAnswer answer_;
  char body_[kClientBodyBytes];
};

}  // namespace wattlink
