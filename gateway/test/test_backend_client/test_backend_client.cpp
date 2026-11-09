#include <string.h>
#include <unity.h>

#include <deque>
#include <map>
#include <string>

#include "../support/fake-crypto.h"
#include "backend-client.h"
#include "flash-client-store.h"

using namespace wattlink;
using namespace wattlink::testsupport;

namespace {

constexpr uint32_t kBoot = 0x105;
constexpr uint32_t kEpoch = 1793618400;
constexpr uint32_t kNodeBoot = 3;
const char kKey[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";  // 43 characters
const char kPoll[] = "0123456789abcdef0123456789abcdef";
const uint8_t kNodeKey[kNodeKeySize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

void makeMac(uint8_t mac[kMacSize], uint32_t number) {
  mac[0] = 0x40;
  mac[1] = 0x00;
  mac[2] = 0x00;
  mac[3] = static_cast<uint8_t>(number >> 16);
  mac[4] = static_cast<uint8_t>(number >> 8);
  mac[5] = static_cast<uint8_t>(number);
}

// The digest is the numbers 0 to 31, so the hex text of the hash is known.
bool fakeSha256(void *, const uint8_t *, size_t, uint8_t out[kSha256Size]) {
  for (size_t i = 0; i < kSha256Size; i++) {
    out[i] = static_cast<uint8_t>(i);
  }
  return true;
}
const char kDigestHex[] = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";

bool failingSha256(void *, const uint8_t *, size_t, uint8_t[kSha256Size]) {
  return false;
}

struct Seen {
  HttpMethod method;
  std::string path;
  bool hasBearer;
  std::string bearer;
  std::string body;
};

class FakeHttp : public HttpPort {
 public:
  bool begin(const HttpRequest &request, HttpResponse *response) override {
    beginCalls++;
    out_ = response;
    if (refuse) {
      return false;
    }
    Seen seen;
    seen.method = request.method;
    seen.path = request.path;
    seen.hasBearer = request.bearer != nullptr;
    seen.bearer = request.bearer != nullptr ? request.bearer : "";
    seen.body = request.body != nullptr ? std::string(request.body, request.bodyLength) : "";
    requests.push_back(seen);
    running = true;
    waited = 0;
    return true;
  }

  bool poll() override {
    HttpResponse *out = out_;
    if (!running || hang) {
      return false;
    }
    if (waited < delayPolls) {
      waited++;
      return false;
    }
    running = false;
    Reply reply = replies.empty() ? Reply() : replies.front();
    if (!replies.empty()) {
      replies.pop_front();
    }
    memset(out, 0, sizeof(*out));
    out->failure = reply.failure;
    out->status = reply.status;
    out->retryAfterSeconds = reply.retryAfter;
    out->truncated = reply.truncated;
    out->bodyLength = reply.body.size() < kMaxAnswerBytes ? reply.body.size() : kMaxAnswerBytes;
    memcpy(out->body, reply.body.data(), out->bodyLength);
    return true;
  }

  void cancel() override {
    cancels++;
    running = false;
    hang = false;
  }

  struct Reply {
    HttpFailure failure = HttpFailure::None;
    int status = 200;
    uint32_t retryAfter = 0;
    bool truncated = false;
    std::string body;
  };

  void reply(int status, const std::string &body = "", uint32_t retryAfter = 0) {
    Reply r;
    r.status = status;
    r.body = body;
    r.retryAfter = retryAfter;
    replies.push_back(r);
  }
  void fail(HttpFailure failure) {
    Reply r;
    r.failure = failure;
    r.status = 0;
    replies.push_back(r);
  }

  std::deque<Reply> replies;
  std::deque<Seen> requests;
  HttpResponse *out_ = nullptr;
  bool refuse = false;
  bool hang = false;
  bool running = false;
  int delayPolls = 0;
  int waited = 0;
  int beginCalls = 0;
  int cancels = 0;
};

class FakeClientStore : public ClientStore {
 public:
  bool loadKey(char out[kApiKeySize]) override {
    if (noKey) {
      out[0] = '\0';
      return false;
    }
    strcpy(out, key);
    return true;
  }
  bool ensureKey(char out[kApiKeySize]) override {
    ensures++;
    if (failEnsure) {
      out[0] = '\0';
      return false;
    }
    noKey = false;
    strcpy(out, key);
    return true;
  }
  bool renewKey() override {
    renews++;
    if (failRenew) {
      return false;
    }
    strcpy(key, "ZYXWVUTSRQPONMLKJIHGFEDCBAzyxwvutsr");
    return true;
  }
  bool enrolled() override { return enrolledFlag; }
  bool setEnrolled(bool value) override {
    flagWrites++;
    if (failSetEnrolled) {
      return false;
    }
    enrolledFlag = value;
    return true;
  }

  char key[kApiKeySize] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ";
  bool noKey = false;
  bool failEnsure = false;
  bool failRenew = false;
  bool failSetEnrolled = false;
  bool enrolledFlag = false;
  int ensures = 0;
  int renews = 0;
  int flagWrites = 0;
};

class FakeSender : public CommandSender {
 public:
  bool sendCommand(const uint8_t[kMacSize], uint32_t, uint32_t, uint16_t, uint8_t) override { return true; }
};

class FakeLog : public CommandLog {
 public:
  bool read(size_t slot, CommandRecord *out) override {
    if (!present[slot]) {
      return false;
    }
    *out = slots[slot];
    return true;
  }
  bool write(size_t slot, const CommandRecord &record) override {
    slots[slot] = record;
    present[slot] = true;
    return true;
  }
  CommandRecord slots[kCommandLogSlots] = {};
  bool present[kCommandLogSlots] = {};
};

class MapStore : public KeyValueStore {
 public:
  bool get(const char *key, char *out, size_t size) override {
    const auto found = data.find(key);
    if (size > 0) {
      out[0] = '\0';
    }
    if (found == data.end() || found->second.size() + 1 > size) {
      return false;
    }
    memcpy(out, found->second.c_str(), found->second.size() + 1);
    return true;
  }
  bool put(const char *key, const char *value) override {
    data[key] = value;
    return true;
  }
  bool remove(const char *key) override {
    data.erase(key);
    return true;
  }
  std::map<std::string, std::string> data;
};

uint32_t countingRandom(void *context) {
  uint32_t *counter = static_cast<uint32_t *>(context);
  *counter += 0x9E3779B9u;
  return *counter;
}

struct Rig {
  Rig() : registry(flash), builder(samples, registry, kClientBodyBytes), relay(sender, registry, log, kBoot), client(http, store, builder, relay, config()) {
    registry.load();
    relay.load();
  }

  static ClientConfig config(const char *hardwareId = "ESP32-A1B2C3D4E5F6", const char *firmware = "0.1.0") {
    ClientConfig c;
    c.hardwareId = hardwareId;
    c.firmware = firmware;
    c.sha256 = fakeSha256;
    c.sha256Context = nullptr;
    return c;
  }

  void pairPlug(uint32_t number) {
    uint8_t mac[kMacSize];
    makeMac(mac, number);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(registry.pair(kKindPlug, mac, kNodeKey)));
  }

  void addSample(uint32_t nowMs, uint32_t node, uint32_t seq) {
    uint8_t mac[kMacSize];
    makeMac(mac, node);
    Sample sample;
    memset(&sample, 0, sizeof(sample));
    sample.sampleSeq = seq;
    sample.ageMs = 0;
    sample.voltageDeciV = kNoU16;
    sample.currentCentiA = kNoU16;
    sample.powerDeciW = 1000;
    sample.energyWh = kNoU32;
    sample.frequencyDeciHz = kNoU16;
    sample.powerFactorCenti = kNoU8;
    sample.condition = kNoU8;
    sample.relay = kNoU8;
    samples.onSamples(nowMs, mac, kNodeBoot, &sample, 1);
  }

  // Runs update() on every 100 ms from the time now up to and including the time end, and returns the time that it stopped at.
  uint32_t run(uint32_t from, uint32_t to, bool linkUp = true) {
    uint32_t t = from;
    for (; t <= to; t += 100) {
      client.update(t, linkUp);
    }
    return t - 100;
  }

  ClientEventKind event() {
    ClientEvent e;
    return client.takeEvent(&e) ? e.kind : ClientEventKind::None;
  }
  bool hasEvent(ClientEventKind kind) {
    ClientEvent e;
    bool found = false;
    while (client.takeEvent(&e)) {
      found = found || e.kind == kind;
    }
    return found;
  }

  FakeStore flash;
  NodeRegistry registry;
  SampleStore samples;
  BatchBuilder builder;
  FakeSender sender;
  FakeLog log;
  CommandRelay relay;
  FakeHttp http;
  FakeClientStore store;
  BackendClient client;
};

std::string enrollAnswer(const char *code = "ABCD2345", uint32_t seconds = 600) {
  return std::string("{\"claimCode\":\"") + code + "\",\"pollId\":\"" + kPoll + "\",\"expiresInSeconds\":" + std::to_string(seconds) + "}";
}

std::string command(const char *id, const char *node, const char *action) {
  return std::string("{\"id\":\"") + id + "\",\"nodeId\":\"" + node + "\",\"action\":\"" + action + "\",\"expiresAt\":\"2026-11-03T08:11:02Z\"}";
}

std::string ack(uint32_t seq, const std::string &commands = "") {
  return "{\"ackSeq\":" + std::to_string(seq) + ",\"serverTime\":\"2026-11-03T08:01:02Z\",\"accepted\":1,\"rejected\":0,\"commands\":[" + commands + "]}";
}

std::string expected(uint32_t seq) {
  return "{\"code\":\"sequence_gap\",\"message\":\"gap\",\"expectedSeq\":" + std::to_string(seq) + "}";
}

// A hub that is enrolled and has a sample and a clock, so the next update sends a batch.
void makeReady(Rig &rig, uint32_t nowMs = 1000) {
  rig.store.enrolledFlag = true;
  TEST_ASSERT_TRUE(rig.client.begin(nowMs));
  rig.client.setUnixTime(nowMs, kEpoch);
  rig.pairPlug(1);
  rig.addSample(nowMs, 1, 1);
}

bool contains(const std::string &text, const std::string &part) {
  return text.find(part) != std::string::npos;
}

void test_a_bad_configuration_stops_the_client_and_says_so() {
  const char *ids[] = {nullptr, "short", "ESP32-A1B2C3D4E5F6 ", "ESP32/A1B2C3", "ESP32-A1B2C3D4E5F6\\\"x"};
  for (const char *id : ids) {
    Rig rig;
    BackendClient client(rig.http, rig.store, rig.builder, rig.relay, Rig::config(id));
    TEST_ASSERT_FALSE(client.begin(0));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Stopped), static_cast<int>(client.state()));
    ClientEvent e;
    TEST_ASSERT_TRUE(client.takeEvent(&e));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientEventKind::ConfigInvalid), static_cast<int>(e.kind));
    client.update(1000, true);
    TEST_ASSERT_EQUAL_INT(0, rig.http.beginCalls);
  }
  Rig rig;
  const std::string longId(65, 'a');
  BackendClient a(rig.http, rig.store, rig.builder, rig.relay, Rig::config(longId.c_str()));
  TEST_ASSERT_FALSE(a.begin(0));
  BackendClient b(rig.http, rig.store, rig.builder, rig.relay, Rig::config("ESP32-A1B2C3D4E5F6", "bad firmware"));
  TEST_ASSERT_FALSE(b.begin(0));
  BackendClient c(rig.http, rig.store, rig.builder, rig.relay, Rig::config("ESP32-A1B2C3D4E5F6", ""));
  TEST_ASSERT_FALSE(c.begin(0));
  const std::string longFirmware(33, '1');
  BackendClient d(rig.http, rig.store, rig.builder, rig.relay, Rig::config("ESP32-A1B2C3D4E5F6", longFirmware.c_str()));
  TEST_ASSERT_FALSE(d.begin(0));
  ClientConfig noHash = Rig::config();
  noHash.sha256 = nullptr;
  BackendClient e(rig.http, rig.store, rig.builder, rig.relay, noHash);
  TEST_ASSERT_FALSE(e.begin(0));
  // A firmware text may be left out.
  BackendClient f(rig.http, rig.store, rig.builder, rig.relay, Rig::config("ESP32-A1B2C3D4E5F6", nullptr));
  TEST_ASSERT_TRUE(f.begin(0));
}

void test_a_hub_that_is_not_enrolled_asks_for_a_code_and_never_sends_its_key() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.client.begin(0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::NeedEnroll), static_cast<int>(rig.client.state()));
  rig.client.update(0, false);
  TEST_ASSERT_EQUAL_INT(0, rig.http.beginCalls);  // no link, no request
  rig.http.reply(201, enrollAnswer());
  rig.client.update(10, true);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  const Seen &seen = rig.http.requests[0];
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpMethod::Post), static_cast<int>(seen.method));
  TEST_ASSERT_EQUAL_STRING("/device/v1/enroll", seen.path.c_str());
  TEST_ASSERT_FALSE(seen.hasBearer);
  TEST_ASSERT_EQUAL_STRING((std::string("{\"hardwareId\":\"ESP32-A1B2C3D4E5F6\",\"firmware\":\"0.1.0\",\"keyHash\":\"") + kDigestHex + "\"}").c_str(), seen.body.c_str());
  TEST_ASSERT_FALSE(contains(seen.body, kKey));
  TEST_ASSERT_EQUAL_INT(1, rig.store.ensures);
}

void test_the_enroll_body_leaves_out_a_firmware_text_that_is_not_given() {
  Rig rig;
  BackendClient client(rig.http, rig.store, rig.builder, rig.relay, Rig::config("ESP32-A1B2C3D4E5F6", nullptr));
  TEST_ASSERT_TRUE(client.begin(0));
  client.update(0, true);
  TEST_ASSERT_EQUAL_STRING((std::string("{\"hardwareId\":\"ESP32-A1B2C3D4E5F6\",\"keyHash\":\"") + kDigestHex + "\"}").c_str(), rig.http.requests[0].body.c_str());
}

void test_a_claim_code_is_shown_and_polled_every_5_seconds_until_the_owner_types_it() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("ZXCV2345", 600));
  rig.client.update(0, true);
  rig.client.update(10, true);  // the answer
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::ShowCode), static_cast<int>(rig.client.state()));
  TEST_ASSERT_EQUAL_STRING("ZXCV2345", rig.client.claimCode());
  TEST_ASSERT_EQUAL_UINT32(600, rig.client.claimSecondsLeft(10));
  TEST_ASSERT_EQUAL_UINT32(595, rig.client.claimSecondsLeft(5010));
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::CodeShown));
  rig.http.reply(202);
  rig.http.reply(202);
  rig.http.reply(200, "{\"gatewayId\":\"6f1c7e0a-0000-4000-8000-000000000001\"}");
  // The first poll comes 5 seconds after the answer, then every 5 seconds.
  rig.run(10, 4900);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.run(5000, 5200);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(HttpMethod::Get), static_cast<int>(rig.http.requests[1].method));
  TEST_ASSERT_EQUAL_STRING((std::string("/device/v1/enroll/") + kPoll).c_str(), rig.http.requests[1].path.c_str());
  TEST_ASSERT_FALSE(rig.http.requests[1].hasBearer);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::ShowCode), static_cast<int>(rig.client.state()));
  rig.run(5300, 10400);
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  rig.run(10500, 15400);
  TEST_ASSERT_EQUAL_UINT(4, rig.http.requests.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
  TEST_ASSERT_TRUE(rig.store.enrolledFlag);
  TEST_ASSERT_EQUAL_STRING("", rig.client.claimCode());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::Claimed));
}

void test_a_code_that_runs_out_is_replaced_by_a_new_one() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA", 60));
  // The polls at 5, 10 and so on up to 55 seconds, one last poll when the code runs out, and then a new code.
  for (int i = 0; i < 12; i++) {
    rig.http.reply(202);
  }
  rig.http.reply(201, enrollAnswer("BBBBBBBB", 60));
  for (int i = 0; i < 20; i++) {
    rig.http.reply(202);
  }
  rig.run(0, 59000);
  TEST_ASSERT_EQUAL_STRING("AAAAAAAA", rig.client.claimCode());
  rig.run(59100, 62000);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::CodeExpired));
  TEST_ASSERT_EQUAL_STRING("BBBBBBBB", rig.client.claimCode());
  // The new code was asked for with a new request to the enroll route.
  size_t enrolls = 0;
  for (const Seen &seen : rig.http.requests) {
    enrolls += seen.path == "/device/v1/enroll" ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_UINT(2, enrolls);
}

void test_a_poll_answer_of_410_asks_for_a_new_code_at_once() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA"));
  rig.http.reply(410, "{\"code\":\"enrollment_expired\",\"message\":\"x\"}");
  rig.http.reply(201, enrollAnswer("CCCCCCCC"));
  rig.run(0, 5600);
  TEST_ASSERT_EQUAL_STRING("CCCCCCCC", rig.client.claimCode());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::CodeExpired));
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
}

void test_a_poll_answer_of_404_sends_one_batch_to_test_the_key_and_a_401_then_asks_for_a_new_code() {
  Rig rig;
  rig.client.begin(0);
  rig.client.setUnixTime(0, kEpoch);
  rig.pairPlug(1);
  rig.addSample(0, 1, 1);
  rig.http.reply(201, enrollAnswer("AAAAAAAA"));
  rig.http.reply(404, "{\"code\":\"not_found\",\"message\":\"x\"}");
  rig.http.reply(401, "{\"code\":\"invalid_gateway_key\",\"message\":\"x\"}");
  rig.http.reply(201, enrollAnswer("DDDDDDDD"));
  rig.run(0, 6000);
  TEST_ASSERT_EQUAL_STRING("DDDDDDDD", rig.client.claimCode());
  TEST_ASSERT_EQUAL_STRING("/device/v1/readings", rig.http.requests[2].path.c_str());
  TEST_ASSERT_EQUAL_STRING("/device/v1/enroll", rig.http.requests[3].path.c_str());
  // One answer 401 was enough, because the code was never claimed.
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
}

void test_a_poll_answer_of_404_and_a_good_batch_prove_the_key_and_set_the_flag() {
  Rig rig;
  rig.client.begin(0);
  rig.client.setUnixTime(0, kEpoch);
  rig.pairPlug(1);
  rig.addSample(0, 1, 1);
  rig.http.reply(201, enrollAnswer());
  rig.http.reply(404);
  rig.http.reply(409, expected(5));
  rig.http.reply(200, ack(5));
  rig.run(0, 12000);
  TEST_ASSERT_TRUE(rig.store.enrolledFlag);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
}

void test_a_flag_without_a_key_starts_a_new_enrollment() {
  Rig rig;
  rig.store.enrolledFlag = true;
  rig.store.noKey = true;
  TEST_ASSERT_TRUE(rig.client.begin(0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::NeedEnroll), static_cast<int>(rig.client.state()));
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
}

void test_a_failure_of_the_enroll_route_waits_longer_each_time_up_to_five_minutes() {
  Rig rig;
  rig.client.begin(0);
  for (int i = 0; i < 12; i++) {
    rig.http.fail(HttpFailure::Connect);
  }
  rig.http.reply(201, enrollAnswer());
  uint32_t t = 0;
  uint32_t starts[14];
  size_t seen = 0;
  while (t < 3000000 && seen < 13) {
    const size_t before = rig.http.requests.size();
    rig.client.update(t, true);
    if (rig.http.requests.size() > before) {
      starts[seen++] = t;
    }
    t += 100;
  }
  TEST_ASSERT_EQUAL_UINT(13, seen);
  const uint32_t gaps[] = {5000, 10000, 20000, 40000, 80000, 160000, 300000, 300000};
  for (size_t i = 0; i < 8; i++) {
    const uint32_t gap = starts[i + 1] - starts[i];
    TEST_ASSERT_TRUE(gap >= gaps[i] && gap <= gaps[i] + 300);
  }
  TEST_ASSERT_TRUE(rig.client.failures() >= 12);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::RequestFailed));
}

void test_an_answer_429_waits_as_long_as_the_service_says_in_the_body_or_the_header() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(429, "{\"code\":\"rate_limited\",\"message\":\"x\",\"retryAfterSeconds\":40}", 10);
  rig.http.reply(201, enrollAnswer());
  rig.run(0, 39000);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.run(39100, 40300);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  ClientEvent e;
  bool found = false;
  while (rig.client.takeEvent(&e)) {
    if (e.kind == ClientEventKind::RateLimited) {
      found = true;
      TEST_ASSERT_EQUAL_UINT32(40, e.value);
    }
  }
  TEST_ASSERT_TRUE(found);
}

void test_the_wait_of_an_answer_429_is_kept_between_one_second_and_an_hour() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(429, "not json", 0);
  rig.http.reply(429, "{\"retryAfterSeconds\":999999}");
  rig.run(0, 500);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.run(600, 1300);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  rig.run(1400, 3500000);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  rig.run(3500100, 3603000);
  TEST_ASSERT_TRUE(rig.http.requests.size() >= 3);
}

void test_a_bad_enroll_answer_is_a_failure_and_the_client_tries_again() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, "{\"claimCode\":\"abc\"}");
  rig.http.reply(500);
  rig.http.reply(201, enrollAnswer());
  rig.run(0, 20000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::ShowCode), static_cast<int>(rig.client.state()));
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::BadAnswer));
}

void test_a_key_that_cannot_be_made_or_hashed_is_reported_and_retried() {
  Rig rig;
  rig.store.failEnsure = true;
  rig.client.begin(0);
  rig.run(0, 200);
  TEST_ASSERT_EQUAL_INT(0, rig.http.beginCalls);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::KeyMissing));
  Rig other;
  ClientConfig config = Rig::config();
  config.sha256 = failingSha256;
  BackendClient client(other.http, other.store, other.builder, other.relay, config);
  client.begin(0);
  client.update(0, true);
  TEST_ASSERT_EQUAL_INT(0, other.http.beginCalls);
}

void test_a_batch_is_sent_with_the_key_and_the_first_one_carries_the_probe_number() {
  Rig rig;
  makeReady(rig);
  rig.http.reply(409, expected(57));
  rig.http.reply(200, ack(57));
  rig.client.update(1000, true);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  const Seen &first = rig.http.requests[0];
  TEST_ASSERT_EQUAL_STRING("/device/v1/readings", first.path.c_str());
  TEST_ASSERT_TRUE(first.hasBearer);
  TEST_ASSERT_EQUAL_STRING(kKey, first.bearer.c_str());
  TEST_ASSERT_TRUE(contains(first.body, "\"seq\":2147483647,"));
  TEST_ASSERT_TRUE(contains(first.body, "\"nodeId\":\"PLUG-000001\""));
  TEST_ASSERT_TRUE(rig.client.busy());
  // The answer 409 gives the number, and the same batch goes again with it after the minimum gap between batches.
  rig.run(1100, 6900);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.run(7000, 7300);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  TEST_ASSERT_TRUE(contains(rig.http.requests[1].body, "\"seq\":57,"));
  rig.run(7400, 7600);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  TEST_ASSERT_EQUAL_UINT(0, rig.samples.size());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::Conflict));
}

void test_the_next_batch_comes_30_seconds_after_an_answer_and_holds_the_new_samples() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, ack(1));
  rig.http.reply(200, ack(2));
  rig.run(1000, 1300);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  rig.addSample(2000, 1, 2);
  rig.addSample(12000, 1, 3);
  rig.addSample(22000, 1, 4);
  rig.run(1400, 30900);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.run(31000, 31300);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  TEST_ASSERT_TRUE(contains(rig.http.requests[1].body, "\"seq\":2,"));
  size_t samples = 0;
  for (size_t at = rig.http.requests[1].body.find("\"t\":"); at != std::string::npos; at = rig.http.requests[1].body.find("\"t\":", at + 1)) {
    samples++;
  }
  TEST_ASSERT_EQUAL_UINT(3, samples);
}

void test_a_backlog_of_samples_is_sent_at_the_minimum_gap_and_not_every_30_seconds() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  for (uint32_t i = 2; i < 400; i++) {
    rig.addSample(1000, 1 + (i % 8), i);
    if (i % 8 == 0) {
      rig.pairPlug(1 + i / 8);
    }
  }
  for (int i = 0; i < 12; i++) {
    rig.http.reply(200, ack(static_cast<uint32_t>(i + 1)));
  }
  rig.run(1000, 7500);
  // Two batches in the first 7 seconds, because the store holds more than one batch.
  TEST_ASSERT_TRUE(rig.http.requests.size() >= 2);
  TEST_ASSERT_TRUE(rig.samples.size() < 399);
}

void test_nothing_is_sent_without_samples_or_while_the_clock_is_not_set() {
  Rig rig;
  rig.store.enrolledFlag = true;
  rig.client.begin(0);
  rig.pairPlug(1);
  rig.run(0, 5000);
  TEST_ASSERT_EQUAL_INT(0, rig.http.beginCalls);
  rig.addSample(5000, 1, 1);
  rig.run(5100, 7000);
  TEST_ASSERT_EQUAL_INT(0, rig.http.beginCalls);
  // The event comes once and not once for each look.
  ClientEvent e;
  int noClock = 0;
  while (rig.client.takeEvent(&e)) {
    noClock += e.kind == ClientEventKind::NoClock ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(1, noClock);
  rig.client.setUnixTime(7000, kEpoch);
  rig.http.reply(409, expected(3));
  rig.run(7100, 8300);
  TEST_ASSERT_EQUAL_INT(1, rig.http.beginCalls);
}

void test_the_clock_counts_seconds_survives_the_wrap_and_refuses_a_time_before_2023() {
  Rig rig;
  rig.client.begin(0);
  TEST_ASSERT_EQUAL_UINT32(0, rig.client.unixTime(0));
  rig.client.setUnixTime(100, 1000);
  TEST_ASSERT_EQUAL_UINT32(0, rig.client.unixTime(100));
  rig.client.setUnixTime(0xFFFFFF00u, kEpoch);
  TEST_ASSERT_EQUAL_UINT32(kEpoch, rig.client.unixTime(0xFFFFFF00u));
  TEST_ASSERT_EQUAL_UINT32(kEpoch + 1, rig.client.unixTime(0xFFFFFF00u + 1500));
  TEST_ASSERT_EQUAL_UINT32(kEpoch + 2, rig.client.unixTime(0xFFFFFF00u + 2500));
  // The millisecond counter wraps at 0xFFFFFF00 + 256, and the seconds go on counting.
  TEST_ASSERT_EQUAL_UINT32(kEpoch + 2, rig.client.unixTime(0xFFFFFF00u + 2999));
  TEST_ASSERT_EQUAL_UINT32(kEpoch + 3, rig.client.unixTime(0xFFFFFF00u + 3000));
  TEST_ASSERT_EQUAL_UINT32(kEpoch + 3, rig.client.unixTime(0xFFFFFF00u + 3500));
  TEST_ASSERT_EQUAL_UINT32(kEpoch + 4, rig.client.unixTime(0xFFFFFF00u + 4000));
}

void test_a_clock_that_is_set_is_not_moved_by_the_time_of_the_answer() {
  Rig rig;
  rig.store.enrolledFlag = true;
  rig.client.begin(0);
  rig.pairPlug(1);
  rig.addSample(0, 1, 1);
  rig.client.setUnixTime(0, kEpoch);
  TEST_ASSERT_EQUAL_UINT32(kEpoch, rig.client.unixTime(0));
  // The time of the service differs from the clock of the test by about 20 hours, and the clock stays as it was.
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, ack(1));
  rig.run(0, 300);
  TEST_ASSERT_EQUAL_UINT32(kEpoch, rig.client.unixTime(300));
}

void test_the_commands_of_an_answer_go_to_the_relay_and_the_results_go_out_with_the_next_batch() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  // A command for the paired plug is queued, and a command for a node that does not exist gets a result with ok false.
  rig.http.reply(200, ack(1, command("cmd-1", "PLUG-000001", "off") + "," + command("cmd-2", "PLUG-0000FF", "on")));
  rig.run(1000, 1300);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  rig.addSample(2000, 1, 2);
  rig.http.reply(409, expected(9));
  rig.http.reply(200, ack(9));
  rig.run(1400, 31500);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  const std::string &second = rig.http.requests[1].body;
  TEST_ASSERT_TRUE(contains(second, "\"results\":[{\"id\":\"cmd-2\",\"ok\":false}]"));
  // The retry after the answer 409 carries the same result, and the acknowledgement drops it.
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  rig.run(31600, 38500);
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  TEST_ASSERT_TRUE(contains(rig.http.requests[2].body, "\"id\":\"cmd-2\""));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
}

void test_an_answer_without_commands_lets_the_relay_forget_commands_that_ended() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, ack(1));
  rig.run(1000, 1300);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
}

void test_a_batch_that_was_not_acknowledged_goes_again_with_the_same_number_after_a_failure() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(7);
  rig.http.fail(HttpFailure::Tls);
  rig.http.reply(503);
  rig.http.reply(200, ack(7));
  rig.run(1000, 40000);
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  for (const Seen &seen : rig.http.requests) {
    TEST_ASSERT_TRUE(contains(seen.body, "\"seq\":7,"));
    TEST_ASSERT_TRUE(contains(seen.body, "\"nodeId\":\"PLUG-000001\""));
  }
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  TEST_ASSERT_EQUAL_UINT(0, rig.samples.size());
}

void test_an_answer_that_cannot_be_read_is_a_failure_and_the_batch_is_kept() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, "<html>proxy</html>");
  rig.http.reply(200, "{\"ackSeq\":1}");
  rig.http.reply(200, ack(1));
  rig.run(1000, 30000);
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::BadAnswer));
}

void test_a_long_answer_that_was_cut_still_acknowledges_the_batch() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  std::string commands;
  for (int i = 0; i < 100; i++) {
    commands += (i > 0 ? "," : "") + command(("id-" + std::to_string(i)).c_str(), "PLUG-0000FF", "on");
  }
  FakeHttp::Reply reply;
  reply.status = 200;
  reply.body = ack(1, commands);
  reply.truncated = true;
  rig.http.replies.push_back(reply);
  rig.run(1000, 1300);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  TEST_ASSERT_EQUAL_UINT(0, rig.samples.size());
  // The commands that fit before the cut each have a result for the unknown plug, and at most 24 were taken.
  TEST_ASSERT_TRUE(rig.relay.resultCount() > 0 && rig.relay.resultCount() <= kMaxAnswerCommands);
}

void test_three_answers_401_in_a_row_stop_the_client_and_the_gesture_starts_a_new_enrollment() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  for (int i = 0; i < 3; i++) {
    rig.http.reply(401, "{\"code\":\"invalid_gateway_key\",\"message\":\"x\"}");
  }
  rig.run(1000, 30000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Refused), static_cast<int>(rig.client.state()));
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::KeyRefused));
  // Nothing more goes out, and the samples stay in the store.
  rig.run(30100, 700000);
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  TEST_ASSERT_EQUAL_UINT(1, rig.samples.size());
  // The flag stays set until the owner asks for a new enrollment.
  TEST_ASSERT_TRUE(rig.store.enrolledFlag);
  rig.http.reply(201, enrollAnswer("EEEEEEEE"));
  TEST_ASSERT_TRUE(rig.client.requestNewEnroll(700100));
  TEST_ASSERT_EQUAL_INT(1, rig.store.renews);
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
  rig.run(700200, 700600);
  TEST_ASSERT_EQUAL_STRING("EEEEEEEE", rig.client.claimCode());
  TEST_ASSERT_EQUAL_STRING("/device/v1/enroll", rig.http.requests[3].path.c_str());
}

void test_an_answer_between_two_answers_401_starts_the_count_again() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(401);
  rig.http.reply(401);
  rig.http.reply(409, expected(1));
  rig.http.reply(401);
  rig.http.reply(401);
  rig.http.reply(200, ack(1));
  rig.run(1000, 60000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
}

void test_a_failure_to_connect_between_answers_401_does_not_end_the_count() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(401);
  rig.http.fail(HttpFailure::Connect);
  rig.http.reply(401);
  rig.http.fail(HttpFailure::Timeout);
  rig.http.reply(401);
  rig.run(1000, 120000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Refused), static_cast<int>(rig.client.state()));
}

void test_the_gesture_is_refused_while_a_request_runs_when_stopped_or_when_the_flash_refuses_the_key() {
  Rig rig;
  TEST_ASSERT_FALSE(rig.client.requestNewEnroll(0));  // not started
  makeReady(rig);
  rig.http.hang = true;
  rig.client.update(1000, true);
  TEST_ASSERT_TRUE(rig.client.busy());
  TEST_ASSERT_FALSE(rig.client.requestNewEnroll(1100));
  TEST_ASSERT_EQUAL_INT(0, rig.store.renews);
  // A flag that cannot be cleared stops the gesture before anything changes, and the key is not touched.
  Rig other;
  makeReady(other);
  other.store.failSetEnrolled = true;
  TEST_ASSERT_FALSE(other.client.requestNewEnroll(1100));
  TEST_ASSERT_TRUE(other.store.enrolledFlag);
  TEST_ASSERT_EQUAL_INT(0, other.store.renews);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(other.client.state()));
}

void test_a_key_that_the_flash_refuses_still_ends_in_a_new_enrollment_with_the_old_key() {
  Rig rig;
  makeReady(rig);
  rig.store.failRenew = true;
  rig.http.reply(201, enrollAnswer("HHHHHHHH"));
  TEST_ASSERT_FALSE(rig.client.requestNewEnroll(1100));
  // The flag is cleared in flash and in RAM, so a restart now would ask for a code too.
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::NeedEnroll), static_cast<int>(rig.client.state()));
  rig.run(1200, 1600);
  TEST_ASSERT_EQUAL_STRING("HHHHHHHH", rig.client.claimCode());
  TEST_ASSERT_EQUAL_STRING("/device/v1/enroll", rig.http.requests[0].path.c_str());
}

void test_a_restart_after_the_flag_was_cleared_and_before_the_key_changed_asks_for_a_code() {
  Rig rig;
  MapStore flash;
  SettingsStore settings(flash);
  uint32_t counter = 1;
  FlashClientStore store(settings, countingRandom, &counter);
  bool created = false;
  char key[kApiKeySize];
  TEST_ASSERT_TRUE(settings.ensureApiKey(countingRandom, &counter, key, &created));
  TEST_ASSERT_TRUE(settings.saveEnrolled(true));
  // The power fails after the flag is cleared and before the key is replaced, so the flash holds the old key and no flag.
  TEST_ASSERT_TRUE(settings.saveEnrolled(false));
  BackendClient client(rig.http, store, rig.builder, rig.relay, Rig::config());
  TEST_ASSERT_TRUE(client.begin(0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::NeedEnroll), static_cast<int>(client.state()));
}

void test_a_batch_that_the_service_refuses_three_times_with_422_is_dropped() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  for (int i = 0; i < 3; i++) {
    rig.http.reply(422, "{\"code\":\"validation_failed\",\"message\":\"x\",\"fields\":{\"nodes\":\"invalid\"}}");
  }
  rig.run(1000, 90000);
  TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
  TEST_ASSERT_EQUAL_UINT(0, rig.samples.size());
  ClientEvent e;
  bool dropped = false;
  while (rig.client.takeEvent(&e)) {
    if (e.kind == ClientEventKind::BatchDropped) {
      dropped = true;
      TEST_ASSERT_EQUAL_UINT32(1, e.value);
    }
  }
  TEST_ASSERT_TRUE(dropped);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
}

void test_an_answer_429_to_a_batch_waits_and_sends_the_same_batch() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(429, "{\"retryAfterSeconds\":120}");
  rig.http.reply(200, ack(1));
  rig.run(1000, 100000);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.run(100100, 122000);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  TEST_ASSERT_TRUE(contains(rig.http.requests[1].body, "\"seq\":1,"));
}

void test_from_the_fifth_answer_409_in_a_row_the_pause_grows_like_after_a_failure() {
  Rig rig;
  makeReady(rig);
  for (int i = 0; i < 12; i++) {
    rig.http.reply(409, expected(static_cast<uint32_t>(10 + i)));
  }
  uint32_t t = 1000;
  std::deque<uint32_t> starts;
  size_t seen = 0;
  while (t < 400000 && seen < 9) {
    const size_t before = rig.http.requests.size();
    rig.client.update(t, true);
    if (rig.http.requests.size() > before) {
      starts.push_back(t);
      seen++;
    }
    t += 100;
  }
  TEST_ASSERT_EQUAL_UINT(9, seen);
  // The first four answers are followed by the minimum gap of 6 seconds.
  for (size_t i = 1; i <= 4; i++) {
    const uint32_t gap = starts[i] - starts[i - 1];
    TEST_ASSERT_TRUE(gap >= kMinBatchGapMs && gap <= kMinBatchGapMs + 300);
  }
  // The fifth answer starts a pause of 5 seconds, which the gap of 6 seconds covers, and then the pause doubles.
  const uint32_t gaps[] = {kMinBatchGapMs, 10000, 20000, 40000};
  for (size_t i = 0; i < 4; i++) {
    const uint32_t gap = starts[i + 5] - starts[i + 4];
    TEST_ASSERT_TRUE(gap >= gaps[i] && gap <= gaps[i] + 300);
  }
}

void test_an_answer_409_without_a_number_is_a_bad_answer() {
  Rig rig;
  makeReady(rig);
  rig.http.reply(409, "{\"code\":\"sequence_gap\",\"message\":\"x\"}");
  rig.http.reply(409, expected(4));
  rig.run(1000, 20000);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::BadAnswer));
  TEST_ASSERT_TRUE(contains(rig.http.requests.back().body, "\"seq\":4,"));
}

void test_a_request_that_does_not_end_is_cancelled_after_40_seconds_and_counts_as_a_failure() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.hang = true;
  rig.client.update(1000, true);
  TEST_ASSERT_TRUE(rig.client.busy());
  rig.run(1100, 40900);
  TEST_ASSERT_EQUAL_INT(0, rig.http.cancels);
  rig.run(41000, 41200);
  TEST_ASSERT_EQUAL_INT(1, rig.http.cancels);
  TEST_ASSERT_FALSE(rig.client.busy());
  rig.http.reply(200, ack(1));
  rig.run(41300, 50000);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
}

void test_a_request_in_flight_is_read_when_it_ends_even_when_the_link_is_down() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.delayPolls = 3;
  rig.http.reply(200, ack(1));
  rig.client.update(1000, true);
  rig.client.update(1100, false);
  rig.client.update(1200, false);
  rig.client.update(1300, false);
  rig.client.update(1400, false);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  // No new request starts while the link is down.
  rig.addSample(2000, 1, 2);
  rig.run(1500, 100000, false);
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  rig.http.reply(200, ack(2));
  rig.run(100100, 100400, true);
  TEST_ASSERT_EQUAL_UINT(2, rig.http.requests.size());
}

void test_a_request_that_cannot_start_is_tried_again_and_the_batch_is_not_marked_as_sent() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.refuse = true;
  rig.run(1000, 3000);
  TEST_ASSERT_TRUE(rig.http.beginCalls >= 2);
  TEST_ASSERT_EQUAL_UINT(0, rig.http.requests.size());
  rig.http.refuse = false;
  rig.http.reply(200, ack(1));
  rig.run(3100, 4300);
  // The first real request is not held back by the gap between batches, because no batch had left.
  TEST_ASSERT_EQUAL_UINT(1, rig.http.requests.size());
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
}

void test_a_missing_key_is_reported_when_a_batch_is_due() {
  Rig rig;
  makeReady(rig);
  rig.store.noKey = true;
  rig.run(1000, 1500);
  TEST_ASSERT_EQUAL_INT(0, rig.http.requests.size());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::KeyMissing));
}

void test_events_are_kept_in_order_and_the_oldest_is_dropped_when_nobody_reads_them() {
  Rig rig;
  rig.client.begin(0);
  for (int i = 0; i < 10; i++) {
    rig.http.fail(HttpFailure::Connect);
  }
  rig.run(0, 400000);
  ClientEvent e;
  size_t count = 0;
  while (rig.client.takeEvent(&e)) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientEventKind::RequestFailed), static_cast<int>(e.kind));
    count++;
  }
  TEST_ASSERT_EQUAL_UINT(4, count);
}

void test_the_key_is_never_in_a_body_or_a_path() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, ack(1));
  rig.run(1000, 1300);
  for (const Seen &seen : rig.http.requests) {
    TEST_ASSERT_FALSE(contains(seen.body, kKey));
    TEST_ASSERT_FALSE(contains(seen.path, kKey));
  }
}

void test_a_probe_that_cannot_send_a_batch_gives_up_after_a_minute_and_asks_for_a_new_code() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA"));
  rig.http.reply(404);
  rig.http.reply(201, enrollAnswer("FFFFFFFF"));
  // No paired node and no clock, so no batch can test the key.
  rig.run(0, 5400);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::ClaimUnknown));
  rig.run(5500, 64500);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
  rig.run(64600, 66000);
  TEST_ASSERT_EQUAL_STRING("FFFFFFFF", rig.client.claimCode());
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
}

void test_the_poll_answer_404_is_not_reported_as_a_claim() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer());
  rig.http.reply(404);
  rig.run(0, 5600);
  ClientEvent e;
  while (rig.client.takeEvent(&e)) {
    TEST_ASSERT_TRUE(e.kind != ClientEventKind::Claimed);
  }
}

void test_a_link_that_was_down_for_more_than_25_days_does_not_hold_the_client_back() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, ack(1));
  rig.run(1000, 1300);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  rig.addSample(2000, 1, 2);
  // 26 days in steps of one hour, with no link.
  uint32_t t = 2000;
  for (int hour = 0; hour < 26 * 24; hour++) {
    t += 3600000u;
    rig.client.update(t, false);
  }
  rig.client.setUnixTime(t, kEpoch);
  rig.http.reply(409, expected(2));
  rig.run(t + 100, t + 1300);
  TEST_ASSERT_TRUE(rig.http.requests.size() >= 2);
}

void test_the_timers_work_across_the_wrap_of_the_millisecond_counter() {
  Rig rig;
  const uint32_t base = 0xFFFFFFFFu - 30000u;
  rig.client.begin(base);
  rig.http.reply(201, enrollAnswer("AAAAAAAA", 60));
  for (int i = 0; i < 12; i++) {
    rig.http.reply(202);
  }
  rig.http.reply(201, enrollAnswer("BBBBBBBB", 60));
  for (int i = 0; i < 20; i++) {
    rig.http.reply(202);
  }
  for (uint32_t i = 0; i < 700; i++) {
    rig.client.update(base + i * 100u, true);
  }
  TEST_ASSERT_EQUAL_STRING("BBBBBBBB", rig.client.claimCode());
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::CodeExpired));
  // The polls were 5 seconds apart on both sides of the wrap.
  size_t polls = 0;
  for (const Seen &seen : rig.http.requests) {
    polls += seen.method == HttpMethod::Get ? 1 : 0;
  }
  TEST_ASSERT_TRUE(polls >= 12);
  // A watchdog and a pause across the wrap.
  Rig other;
  const uint32_t start = 0xFFFFFFFFu - 20000u;
  makeReady(other, start);
  other.builder.onExpectedSeq(1);
  other.http.hang = true;
  other.client.update(start, true);
  for (uint32_t i = 1; i < 500; i++) {
    other.client.update(start + i * 100u, true);
  }
  TEST_ASSERT_EQUAL_INT(1, other.http.cancels);
}

void test_an_owner_who_types_the_code_in_the_last_seconds_is_not_asked_for_a_second_code() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA", 60));
  for (int i = 0; i < 11; i++) {
    rig.http.reply(202);
  }
  rig.http.reply(200, "{\"gatewayId\":\"6f1c7e0a-0000-4000-8000-000000000001\"}");
  rig.run(0, 62000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(rig.client.state()));
  TEST_ASSERT_TRUE(rig.store.enrolledFlag);
  TEST_ASSERT_FALSE(rig.hasEvent(ClientEventKind::CodeExpired));
  size_t enrolls = 0;
  for (const Seen &seen : rig.http.requests) {
    enrolls += seen.path == "/device/v1/enroll" ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_UINT(1, enrolls);
}

void test_a_final_poll_that_fails_still_ends_the_code() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA", 60));
  for (int i = 0; i < 11; i++) {
    rig.http.reply(202);
  }
  rig.http.fail(HttpFailure::Timeout);
  rig.http.reply(201, enrollAnswer("CCCCCCCC", 60));
  for (int i = 0; i < 20; i++) {
    rig.http.reply(202);
  }
  rig.run(0, 70000);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::CodeExpired));
  TEST_ASSERT_EQUAL_STRING("CCCCCCCC", rig.client.claimCode());
}

void test_a_code_that_runs_out_does_not_write_the_flag_again() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA", 20));
  for (int i = 0; i < 4; i++) {
    rig.http.reply(202);
  }
  rig.http.reply(201, enrollAnswer("BBBBBBBB", 20));
  for (int i = 0; i < 20; i++) {
    rig.http.reply(202);
  }
  rig.run(0, 30000);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::CodeExpired));
  TEST_ASSERT_EQUAL_INT(0, rig.store.flagWrites);
}

void test_the_answers_that_blame_the_batch_drop_it_after_three_tries_and_a_413_counts_too() {
  const int statuses[] = {400, 413, 415};
  for (int status : statuses) {
    Rig rig;
    makeReady(rig);
    rig.builder.onExpectedSeq(1);
    for (int i = 0; i < 3; i++) {
      rig.http.reply(status);
    }
    rig.run(1000, 90000);
    TEST_ASSERT_EQUAL_UINT(3, rig.http.requests.size());
    TEST_ASSERT_EQUAL_UINT(0, rig.samples.size());
    TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::BatchDropped));
  }
}

void test_a_dropped_batch_keeps_the_results_and_the_next_batch_gets_a_new_number() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(1);
  rig.http.reply(200, ack(1, command("cmd-9", "PLUG-0000FF", "on")));
  rig.run(1000, 1300);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  rig.addSample(2000, 1, 2);
  for (int i = 0; i < 3; i++) {
    rig.http.reply(422);
  }
  rig.http.reply(409, expected(40));
  rig.http.reply(200, ack(40));
  rig.run(1400, 55000);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::BatchDropped));
  TEST_ASSERT_EQUAL_UINT(0, rig.samples.size());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  rig.addSample(55000, 1, 3);
  rig.run(55100, 150000);
  TEST_ASSERT_TRUE(rig.http.requests.size() >= 6);
  TEST_ASSERT_TRUE(contains(rig.http.requests.back().body, "\"seq\":40,"));
  TEST_ASSERT_TRUE(contains(rig.http.requests.back().body, "\"id\":\"cmd-9\""));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
}

void test_an_acknowledgement_below_the_number_of_the_batch_is_a_bad_answer() {
  Rig rig;
  makeReady(rig);
  rig.builder.onExpectedSeq(5);
  rig.http.reply(200, ack(4, command("cmd-1", "PLUG-0000FF", "on")));
  rig.http.reply(200, ack(5));
  rig.run(1000, 1300);
  TEST_ASSERT_TRUE(rig.hasEvent(ClientEventKind::BadAnswer));
  TEST_ASSERT_EQUAL_UINT32(0, rig.client.batchesStored());
  TEST_ASSERT_EQUAL_UINT(1, rig.samples.size());
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  rig.run(1400, 8000);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
}

void test_a_builder_with_a_budget_above_the_buffer_is_refused() {
  Rig rig;
  BatchBuilder big(rig.samples, rig.registry);
  BackendClient client(rig.http, rig.store, big, rig.relay, Rig::config());
  TEST_ASSERT_FALSE(client.begin(0));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Stopped), static_cast<int>(client.state()));
}

void test_a_flag_that_could_not_be_written_is_written_again_by_the_next_good_answer() {
  Rig rig;
  rig.store.enrolledFlag = false;
  rig.client.begin(0);
  rig.client.setUnixTime(0, kEpoch);
  rig.pairPlug(1);
  rig.addSample(0, 1, 1);
  rig.http.reply(201, enrollAnswer());
  rig.http.reply(404);
  rig.store.failSetEnrolled = true;
  rig.http.reply(409, expected(2));
  rig.http.reply(200, ack(2));
  rig.http.reply(200, ack(3));
  rig.run(0, 12000);
  TEST_ASSERT_EQUAL_UINT32(1, rig.client.batchesStored());
  TEST_ASSERT_FALSE(rig.store.enrolledFlag);
  const int writes = rig.store.flagWrites;
  rig.store.failSetEnrolled = false;
  rig.addSample(12000, 1, 2);
  rig.run(12100, 45000);
  TEST_ASSERT_EQUAL_UINT32(2, rig.client.batchesStored());
  TEST_ASSERT_TRUE(rig.store.enrolledFlag);
  TEST_ASSERT_TRUE(rig.store.flagWrites > writes);
}

void test_the_gesture_works_while_a_code_is_shown_and_asks_for_another_code() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer("AAAAAAAA"));
  rig.http.reply(201, enrollAnswer("GGGGGGGG"));
  rig.run(0, 300);
  TEST_ASSERT_EQUAL_STRING("AAAAAAAA", rig.client.claimCode());
  TEST_ASSERT_TRUE(rig.client.requestNewEnroll(400));
  TEST_ASSERT_EQUAL_STRING("", rig.client.claimCode());
  rig.run(500, 900);
  TEST_ASSERT_EQUAL_STRING("GGGGGGGG", rig.client.claimCode());
}

void test_a_key_is_not_in_the_enroll_body_or_the_poll_path() {
  Rig rig;
  rig.client.begin(0);
  rig.http.reply(201, enrollAnswer());
  rig.http.reply(200);
  rig.run(0, 6000);
  TEST_ASSERT_TRUE(rig.http.requests.size() >= 2);
  for (const Seen &seen : rig.http.requests) {
    TEST_ASSERT_FALSE(contains(seen.body, kKey));
    TEST_ASSERT_FALSE(contains(seen.path, kKey));
    TEST_ASSERT_FALSE(seen.hasBearer);
  }
}

void test_the_flash_store_keeps_the_key_and_the_flag_and_a_new_enrollment_changes_both() {
  Rig rig;
  MapStore flash;
  SettingsStore settings(flash);
  uint32_t counter = 1;
  FlashClientStore store(settings, countingRandom, &counter);
  BackendClient client(rig.http, store, rig.builder, rig.relay, Rig::config());
  TEST_ASSERT_TRUE(client.begin(0));
  rig.http.reply(201, enrollAnswer());
  rig.http.reply(200);
  for (uint32_t t = 0; t <= 6000; t += 100) {
    client.update(t, true);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(client.state()));
  TEST_ASSERT_EQUAL_STRING("1", flash.data["enrolled"].c_str());
  const std::string firstKey = flash.data["api_key"];
  TEST_ASSERT_EQUAL_UINT(kGeneratedApiKeyLength, firstKey.size());
  // The key that was made at the enrollment is the one that the next start finds, and the hub is enrolled.
  BackendClient again(rig.http, store, rig.builder, rig.relay, Rig::config());
  TEST_ASSERT_TRUE(again.begin(7000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::Running), static_cast<int>(again.state()));
  TEST_ASSERT_EQUAL_STRING(firstKey.c_str(), flash.data["api_key"].c_str());
  TEST_ASSERT_TRUE(again.requestNewEnroll(7100));
  TEST_ASSERT_TRUE(flash.data["api_key"] != firstKey);
  TEST_ASSERT_TRUE(flash.data.find("enrolled") == flash.data.end());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ClientState::NeedEnroll), static_cast<int>(again.state()));
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_bad_configuration_stops_the_client_and_says_so);
  RUN_TEST(test_a_hub_that_is_not_enrolled_asks_for_a_code_and_never_sends_its_key);
  RUN_TEST(test_the_enroll_body_leaves_out_a_firmware_text_that_is_not_given);
  RUN_TEST(test_a_claim_code_is_shown_and_polled_every_5_seconds_until_the_owner_types_it);
  RUN_TEST(test_a_code_that_runs_out_is_replaced_by_a_new_one);
  RUN_TEST(test_a_poll_answer_of_410_asks_for_a_new_code_at_once);
  RUN_TEST(test_a_poll_answer_of_404_sends_one_batch_to_test_the_key_and_a_401_then_asks_for_a_new_code);
  RUN_TEST(test_a_poll_answer_of_404_and_a_good_batch_prove_the_key_and_set_the_flag);
  RUN_TEST(test_a_flag_without_a_key_starts_a_new_enrollment);
  RUN_TEST(test_a_failure_of_the_enroll_route_waits_longer_each_time_up_to_five_minutes);
  RUN_TEST(test_an_answer_429_waits_as_long_as_the_service_says_in_the_body_or_the_header);
  RUN_TEST(test_the_wait_of_an_answer_429_is_kept_between_one_second_and_an_hour);
  RUN_TEST(test_a_bad_enroll_answer_is_a_failure_and_the_client_tries_again);
  RUN_TEST(test_a_key_that_cannot_be_made_or_hashed_is_reported_and_retried);
  RUN_TEST(test_a_batch_is_sent_with_the_key_and_the_first_one_carries_the_probe_number);
  RUN_TEST(test_the_next_batch_comes_30_seconds_after_an_answer_and_holds_the_new_samples);
  RUN_TEST(test_a_backlog_of_samples_is_sent_at_the_minimum_gap_and_not_every_30_seconds);
  RUN_TEST(test_nothing_is_sent_without_samples_or_while_the_clock_is_not_set);
  RUN_TEST(test_the_clock_counts_seconds_survives_the_wrap_and_refuses_a_time_before_2023);
  RUN_TEST(test_a_clock_that_is_set_is_not_moved_by_the_time_of_the_answer);
  RUN_TEST(test_the_commands_of_an_answer_go_to_the_relay_and_the_results_go_out_with_the_next_batch);
  RUN_TEST(test_an_answer_without_commands_lets_the_relay_forget_commands_that_ended);
  RUN_TEST(test_a_batch_that_was_not_acknowledged_goes_again_with_the_same_number_after_a_failure);
  RUN_TEST(test_an_answer_that_cannot_be_read_is_a_failure_and_the_batch_is_kept);
  RUN_TEST(test_a_long_answer_that_was_cut_still_acknowledges_the_batch);
  RUN_TEST(test_three_answers_401_in_a_row_stop_the_client_and_the_gesture_starts_a_new_enrollment);
  RUN_TEST(test_an_answer_between_two_answers_401_starts_the_count_again);
  RUN_TEST(test_a_failure_to_connect_between_answers_401_does_not_end_the_count);
  RUN_TEST(test_the_gesture_is_refused_while_a_request_runs_when_stopped_or_when_the_flash_refuses_the_key);
  RUN_TEST(test_a_batch_that_the_service_refuses_three_times_with_422_is_dropped);
  RUN_TEST(test_an_answer_429_to_a_batch_waits_and_sends_the_same_batch);
  RUN_TEST(test_from_the_fifth_answer_409_in_a_row_the_pause_grows_like_after_a_failure);
  RUN_TEST(test_an_answer_409_without_a_number_is_a_bad_answer);
  RUN_TEST(test_a_request_that_does_not_end_is_cancelled_after_40_seconds_and_counts_as_a_failure);
  RUN_TEST(test_a_request_in_flight_is_read_when_it_ends_even_when_the_link_is_down);
  RUN_TEST(test_a_request_that_cannot_start_is_tried_again_and_the_batch_is_not_marked_as_sent);
  RUN_TEST(test_a_missing_key_is_reported_when_a_batch_is_due);
  RUN_TEST(test_events_are_kept_in_order_and_the_oldest_is_dropped_when_nobody_reads_them);
  RUN_TEST(test_the_key_is_never_in_a_body_or_a_path);
  RUN_TEST(test_a_probe_that_cannot_send_a_batch_gives_up_after_a_minute_and_asks_for_a_new_code);
  RUN_TEST(test_the_poll_answer_404_is_not_reported_as_a_claim);
  RUN_TEST(test_a_link_that_was_down_for_more_than_25_days_does_not_hold_the_client_back);
  RUN_TEST(test_the_timers_work_across_the_wrap_of_the_millisecond_counter);
  RUN_TEST(test_an_owner_who_types_the_code_in_the_last_seconds_is_not_asked_for_a_second_code);
  RUN_TEST(test_a_final_poll_that_fails_still_ends_the_code);
  RUN_TEST(test_a_code_that_runs_out_does_not_write_the_flag_again);
  RUN_TEST(test_the_answers_that_blame_the_batch_drop_it_after_three_tries_and_a_413_counts_too);
  RUN_TEST(test_a_dropped_batch_keeps_the_results_and_the_next_batch_gets_a_new_number);
  RUN_TEST(test_an_acknowledgement_below_the_number_of_the_batch_is_a_bad_answer);
  RUN_TEST(test_a_builder_with_a_budget_above_the_buffer_is_refused);
  RUN_TEST(test_a_flag_that_could_not_be_written_is_written_again_by_the_next_good_answer);
  RUN_TEST(test_the_gesture_works_while_a_code_is_shown_and_asks_for_another_code);
  RUN_TEST(test_a_key_is_not_in_the_enroll_body_or_the_poll_path);
  RUN_TEST(test_a_key_that_the_flash_refuses_still_ends_in_a_new_enrollment_with_the_old_key);
  RUN_TEST(test_a_restart_after_the_flag_was_cleared_and_before_the_key_changed_asks_for_a_code);
  RUN_TEST(test_the_flash_store_keeps_the_key_and_the_flag_and_a_new_enrollment_changes_both);
  return UNITY_END();
}
