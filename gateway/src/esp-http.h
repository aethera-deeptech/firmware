#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "backend-client.h"

namespace wattlink {

// The HTTPS connection of the backend client, on top of WiFiClientSecure and HTTPClient of the Arduino core. A request runs in a task of its own, so the
// loop of the hub keeps reading the radio and the button while the handshake and the transfer work. The task is pinned to core 0, where the Wi-Fi stack runs,
// and the loop runs on core 1.
//
// The certificate of the server is always checked against the bundle of root certificates that the caller gives. A port without a bundle fails every request
// with HttpFailure::Tls and never connects, and nothing here switches the check off.
//
// The object holds the stack of its task (16 KB), so it must be a static object. Nothing in it prints a key, an address or a body.
class EspHttpPort : public HttpPort {
 public:
  // The host is the name of the backend, without the scheme, and it must stay valid. The bundle is the binary that WiFiClientSecure::setCACertBundle takes.
  EspHttpPort(const char *host, uint16_t port, const uint8_t *caBundle);

  EspHttpPort(const EspHttpPort &) = delete;
  EspHttpPort &operator=(const EspHttpPort &) = delete;

  // Creates the task. Call it once. Returns false when the task could not be made.
  bool start();

  bool begin(const HttpRequest &request, HttpResponse *response) override;
  bool poll() override;
  void cancel() override;

 private:
  enum State : uint8_t {
    Idle,       // no request
    Requested,  // begin() stored a request, and the task has not taken it yet
    Working,    // the task runs the request
    Finished,   // the task wrote the answer, and poll() has not returned it yet
    Abandoned,  // cancel() gave the request up while the task worked on it, and the task will go back to Idle
  };

  static constexpr size_t kStackBytes = 16384;
  static constexpr size_t kPathBytes = 96;

  static void taskEntry(void *self);
  void loop();
  void execute();
  // Writes into the response of the caller, only while the request is still wanted. Returns false when it was given up.
  bool deliver(HttpFailure failure, int status, uint32_t retryAfterSeconds);
  bool appendBody(const uint8_t *data, size_t length);

  const char *host_;
  uint16_t port_;
  const uint8_t *caBundle_;

  std::atomic<uint8_t> state_;
  TaskHandle_t task_;
  StaticTask_t taskBuffer_;
  StaticSemaphore_t lockBuffer_;
  SemaphoreHandle_t lock_;

  // The request, copied by begin().
  HttpMethod method_;
  char path_[kPathBytes];
  bool hasBearer_;
  char bearer_[kApiKeySize];
  const char *body_;
  size_t bodyLength_;
  HttpResponse *response_;
  size_t received_;

  StackType_t stack_[kStackBytes / sizeof(StackType_t)];
};

}  // namespace wattlink
