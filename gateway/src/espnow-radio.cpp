#include "espnow-radio.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <string.h>

namespace wattlink {

namespace {

// Sixteen frames cover a burst of the whole table of nodes in one 6 second turn for a short while. A frame that does not fit is dropped and the
// node sends its samples again, because it never got an ACK for them.
constexpr size_t kQueueDepth = 16;
constexpr TickType_t kSendWaitTicks = pdMS_TO_TICKS(50);

StaticQueue_t queueControl;
uint8_t queueStorage[kQueueDepth * sizeof(EspNowRadio::Received)];
QueueHandle_t queue = nullptr;
StaticSemaphore_t sendDoneControl;
SemaphoreHandle_t sendDone = nullptr;
// The send that waits for its report. The send callback gives the semaphore only for the node of that send, so a late report of an earlier send
// that timed out cannot release the wait of the next one. Both are written by the sending task and read by the Wi-Fi task.
volatile bool waiting = false;
uint8_t waitingMac[kMacSize];
// Each counter has one writer. The receive callback owns the first two and the send callback owns the third.
volatile uint32_t callbackReceived = 0;
volatile uint32_t callbackDropped = 0;
volatile uint32_t callbackFailed = 0;

// The callbacks run in the Wi-Fi task. They copy and count and nothing else.
void onReceive(const uint8_t *mac, const uint8_t *data, int length) {
  if (queue == nullptr || mac == nullptr || data == nullptr || length <= 0 || length > static_cast<int>(kMaxFrameSize)) {
    return;
  }
  EspNowRadio::Received item;
  memcpy(item.mac, mac, kMacSize);
  item.length = static_cast<uint8_t>(length);
  memcpy(item.data, data, static_cast<size_t>(length));
  callbackReceived = callbackReceived + 1;
  if (xQueueSend(queue, &item, 0) != pdTRUE) {
    callbackDropped = callbackDropped + 1;
  }
}

void onSent(const uint8_t *mac, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    callbackFailed = callbackFailed + 1;
  }
  if (waiting && mac != nullptr && memcmp(mac, waitingMac, kMacSize) == 0) {
    xSemaphoreGive(sendDone);
  }
}

}  // namespace

bool EspNowRadio::begin() {
  if (active_) {
    return true;
  }
  // The queue and the semaphore are made at the first start and not in a constructor, because the object is static and its constructor runs before the
  // system is up. Both live in static memory, so there is no heap use and nothing to free.
  if (queue == nullptr) {
    queue = xQueueCreateStatic(kQueueDepth, sizeof(Received), queueStorage, &queueControl);
    sendDone = xSemaphoreCreateBinaryStatic(&sendDoneControl);
  }
  const esp_err_t init = esp_now_init();
  if (init != ESP_OK) {
    Serial.printf("ESP-NOW: init failed (%d)\n", static_cast<int>(init));
    return false;
  }
  if (esp_now_register_recv_cb(&onReceive) != ESP_OK || esp_now_register_send_cb(&onSent) != ESP_OK) {
    Serial.println("ESP-NOW: callbacks refused");
    esp_now_deinit();
    return false;
  }
  xQueueReset(queue);
  xSemaphoreTake(sendDone, 0);
  waiting = false;
  addErrorLogged_ = false;
  active_ = true;
  Serial.println("ESP-NOW: ready");
  return true;
}

void EspNowRadio::end() {
  if (!active_) {
    return;
  }
  // The flag goes first, so a send() or receive() on this task sees that the link is gone. The callbacks go before the stack.
  active_ = false;
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_now_deinit();
  xQueueReset(queue);
  Serial.println("ESP-NOW: stopped");
}

void EspNowRadio::onRadioOn() {
  begin();
}

void EspNowRadio::onRadioOff() {
  end();
}

bool EspNowRadio::receive(Received *out) {
  if (out == nullptr || !active_) {
    return false;
  }
  return xQueueReceive(queue, out, 0) == pdTRUE;
}

bool EspNowRadio::send(const uint8_t mac[kMacSize], const uint8_t *frame, size_t length) {
  if (!active_ || mac == nullptr || frame == nullptr || length == 0 || length > kMaxFrameSize) {
    return false;
  }
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, mac, kMacSize);
  // Channel 0 means the channel that the radio is on, which is the channel of the Wi-Fi of the building. The frame is encrypted by the firmware.
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  // A peer that is there already is not ours to remove, so it is used and left in place.
  const esp_err_t added = esp_now_add_peer(&peer);
  if (added != ESP_OK && added != ESP_ERR_ESPNOW_EXIST) {
    if (!addErrorLogged_) {
      addErrorLogged_ = true;
      Serial.printf("ESP-NOW: add peer failed (%d)\n", static_cast<int>(added));
    }
    return false;
  }
  memcpy(waitingMac, mac, kMacSize);
  xSemaphoreTake(sendDone, 0);
  waiting = true;
  const bool taken = esp_now_send(mac, frame, length) == ESP_OK;
  if (taken) {
    sent_++;
    // The peer stays until the link layer has finished with the frame, or until the wait is over.
    if (xSemaphoreTake(sendDone, kSendWaitTicks) != pdTRUE) {
      timeouts_++;
    }
  }
  waiting = false;
  if (added == ESP_OK) {
    esp_now_del_peer(mac);
  }
  return taken;
}

uint32_t EspNowRadio::received() const {
  return callbackReceived;
}

uint32_t EspNowRadio::dropped() const {
  return callbackDropped;
}

uint32_t EspNowRadio::failed() const {
  return callbackFailed;
}

uint32_t EspNowRadio::timeouts() const {
  return timeouts_;
}

}  // namespace wattlink
