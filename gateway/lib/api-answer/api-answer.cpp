#include "api-answer.h"

#include <string.h>

namespace wattlink {

namespace {

// Objects and arrays may nest this deep in the parts that are skipped. The answers of the backend nest 3 deep at the most.
constexpr int kMaxDepth = 8;
constexpr size_t kKeySize = 24;

enum class Field : uint8_t {
  Unknown,    // the scanner skips the value
  Consumed,   // the callback read the value
  Malformed,  // the JSON is broken, so the scan stops
};

class Scanner;
typedef Field (*FieldFn)(void *context, Scanner &scanner, const char *key);

// A cursor over the body. Every function checks the end first, and none of them reads past it.
class Scanner {
 public:
  Scanner(const char *begin, size_t length) : cur_(begin), end_(begin + length) {}

  void skipSpace() {
    while (cur_ < end_ && (*cur_ == ' ' || *cur_ == '\t' || *cur_ == '\n' || *cur_ == '\r')) {
      cur_++;
    }
  }

  // The next character after the white space, or 0 at the end. A zero byte in the body is not valid JSON, so 0 never means a real character here.
  char peek() {
    skipSpace();
    return cur_ < end_ ? *cur_ : '\0';
  }

  bool consume(char expected) {
    if (peek() != expected) {
      return false;
    }
    cur_++;
    return true;
  }

  bool atEnd() {
    skipSpace();
    return cur_ == end_;
  }

  // True when the scan stopped because the body ended, which is the case for a body that was cut.
  bool stoppedAtEnd() const { return cur_ == end_; }

  // Reads a string and writes its text with a terminating zero. *fits is false when the text did not fit or when it holds a character that cannot be written
  // (the buffer then holds the part that fitted). Returns false when the string is not valid JSON.
  bool readString(char *out, size_t size, bool *fits) {
    *fits = true;
    size_t length = 0;
    if (!consume('"')) {
      return false;
    }
    while (true) {
      if (cur_ >= end_) {
        return false;
      }
      unsigned char c = static_cast<unsigned char>(*cur_++);
      if (c == '"') {
        break;
      }
      if (c < 0x20) {
        return false;
      }
      if (c == '\\') {
        if (cur_ >= end_) {
          return false;
        }
        const char escape = *cur_++;
        switch (escape) {
          case '"':
          case '\\':
          case '/':
            c = static_cast<unsigned char>(escape);
            break;
          case 'b':
            c = '\b';
            break;
          case 'f':
            c = '\f';
            break;
          case 'n':
            c = '\n';
            break;
          case 'r':
            c = '\r';
            break;
          case 't':
            c = '\t';
            break;
          case 'u': {
            if (end_ - cur_ < 4) {
              return false;
            }
            uint32_t code = 0;
            for (int i = 0; i < 4; i++) {
              const int digit = hexValue(*cur_++);
              if (digit < 0) {
                return false;
              }
              code = code * 16 + static_cast<uint32_t>(digit);
            }
            c = code < 0x100 ? static_cast<unsigned char>(code) : 0xFF;
            break;
          }
          default:
            return false;
        }
      }
      // Only printable ASCII is kept, from the raw bytes and from the escapes. Every field that the hub reads is plain text of that kind, and a text with
      // another character (a control character, a byte above 127) is not one of them. It never reaches a log line or the display.
      if (c < 0x20 || c >= 0x7F) {
        *fits = false;
        continue;
      }
      if (length + 1 < size) {
        out[length++] = static_cast<char>(c);
      } else {
        *fits = false;
      }
    }
    if (size > 0) {
      out[length] = '\0';
    }
    return true;
  }

  // Reads a number and says whether it is a plain integer from 0 to 2^32-1 (no sign, no fraction, no exponent). The grammar of JSON is checked for every
  // number, so a number that is not valid makes the scan fail. Returns false for that.
  bool readNumber(bool *isUint32, uint32_t *value) {
    *isUint32 = false;
    *value = 0;
    skipSpace();
    bool negative = false;
    if (cur_ < end_ && *cur_ == '-') {
      negative = true;
      cur_++;
    }
    if (cur_ >= end_ || *cur_ < '0' || *cur_ > '9') {
      return false;
    }
    uint64_t number = 0;
    bool overflow = false;
    if (*cur_ == '0') {
      cur_++;
    } else {
      while (cur_ < end_ && *cur_ >= '0' && *cur_ <= '9') {
        number = number * 10 + static_cast<uint64_t>(*cur_ - '0');
        if (number > 0xFFFFFFFFull) {
          overflow = true;
          number = 0xFFFFFFFFull + 1;
        }
        cur_++;
      }
    }
    bool plain = !negative && !overflow;
    if (cur_ < end_ && *cur_ == '.') {
      plain = false;
      cur_++;
      if (!skipDigits()) {
        return false;
      }
    }
    if (cur_ < end_ && (*cur_ == 'e' || *cur_ == 'E')) {
      plain = false;
      cur_++;
      if (cur_ < end_ && (*cur_ == '+' || *cur_ == '-')) {
        cur_++;
      }
      if (!skipDigits()) {
        return false;
      }
    }
    if (plain) {
      *isUint32 = true;
      *value = static_cast<uint32_t>(number);
    }
    return true;
  }

  // Skips one value of any kind.
  bool skipValue(int depth) {
    if (depth > kMaxDepth) {
      return false;
    }
    const char c = peek();
    if (c == '"') {
      char none[1];
      bool fits = true;
      return readString(none, sizeof(none), &fits);
    }
    if (c == '{') {
      return object(nullptr, nullptr, depth + 1);
    }
    if (c == '[') {
      return array(nullptr, nullptr, depth + 1);
    }
    if (c == 't') {
      return literal("true");
    }
    if (c == 'f') {
      return literal("false");
    }
    if (c == 'n') {
      return literal("null");
    }
    bool isUint = false;
    uint32_t value = 0;
    return readNumber(&isUint, &value);
  }

  // Walks an object. The callback gets each key, and reads the value or says that it does not know the key. A key of more than kKeySize - 1 characters is
  // given to the callback as an empty text, which it does not know.
  bool object(FieldFn fn, void *context, int depth) {
    if (depth > kMaxDepth || !consume('{')) {
      return false;
    }
    if (consume('}')) {
      return true;
    }
    while (true) {
      char key[kKeySize];
      bool fits = true;
      if (!readString(key, sizeof(key), &fits)) {
        return false;
      }
      if (!fits) {
        key[0] = '\0';
      }
      if (!consume(':')) {
        return false;
      }
      Field result = Field::Unknown;
      if (fn != nullptr && key[0] != '\0') {
        result = fn(context, *this, key);
      }
      if (result == Field::Malformed) {
        return false;
      }
      if (result == Field::Unknown && !skipValue(depth)) {
        return false;
      }
      if (consume(',')) {
        continue;
      }
      return consume('}');
    }
  }

  // Walks an array. The callback reads one element at the cursor, and returns false when the JSON is broken.
  typedef bool (*ElementFn)(void *context, Scanner &scanner);
  bool array(ElementFn fn, void *context, int depth) {
    if (depth > kMaxDepth || !consume('[')) {
      return false;
    }
    if (consume(']')) {
      return true;
    }
    while (true) {
      const bool ok = fn != nullptr ? fn(context, *this) : skipValue(depth);
      if (!ok) {
        return false;
      }
      if (consume(',')) {
        continue;
      }
      return consume(']');
    }
  }

  // The depth of the callbacks is one more than the object that holds them, and the callbacks pass it on to skipValue.
  static constexpr int kInnerDepth = 2;

 private:
  static int hexValue(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  bool skipDigits() {
    if (cur_ >= end_ || *cur_ < '0' || *cur_ > '9') {
      return false;
    }
    while (cur_ < end_ && *cur_ >= '0' && *cur_ <= '9') {
      cur_++;
    }
    return true;
  }

  bool literal(const char *word) {
    const size_t length = strlen(word);
    const size_t left = static_cast<size_t>(end_ - cur_);
    if (left < length) {
      // A word that is cut by the end of the body is the end of the body, and not a broken word.
      if (memcmp(cur_, word, left) == 0) {
        cur_ = end_;
      }
      return false;
    }
    if (memcmp(cur_, word, length) != 0) {
      return false;
    }
    cur_ += length;
    return true;
  }

  const char *cur_;
  const char *end_;
};

// Reads a value that has to be a string. A value of another kind is skipped and counts as invalid. Returns false when the JSON is broken.
bool readStringField(Scanner &scanner, char *out, size_t size, bool *valid) {
  if (scanner.peek() != '"') {
    *valid = false;
    return scanner.skipValue(Scanner::kInnerDepth);
  }
  bool fits = true;
  if (!scanner.readString(out, size, &fits)) {
    return false;
  }
  *valid = fits;
  return true;
}

// Reads a value that has to be an integer from 0 to 2^32-1.
bool readUintField(Scanner &scanner, uint32_t *out, bool *valid) {
  const char c = scanner.peek();
  if (c != '-' && (c < '0' || c > '9')) {
    *valid = false;
    return scanner.skipValue(Scanner::kInnerDepth);
  }
  bool isUint = false;
  if (!scanner.readNumber(&isUint, out)) {
    return false;
  }
  *valid = isUint;
  return true;
}

bool isLeapYear(int64_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int daysInMonth(int64_t year, int month) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && isLeapYear(year) ? 29 : kDays[month - 1];
}

// Reads exactly count digits and returns their value, or -1.
int digits(const char **cursor, int count) {
  int value = 0;
  for (int i = 0; i < count; i++) {
    const char c = **cursor;
    if (c < '0' || c > '9') {
      return -1;
    }
    value = value * 10 + (c - '0');
    (*cursor)++;
  }
  return value;
}

bool expect(const char **cursor, char c) {
  if (**cursor != c) {
    return false;
  }
  (*cursor)++;
  return true;
}

// ---- The answer of the readings route

// The characters that the contract allows in a node id.
bool isNodeIdText(const char *text) {
  for (const char *p = text; *p != '\0'; p++) {
    const char c = *p;
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == ':' || c == '_' || c == '-')) {
      return false;
    }
  }
  return true;
}

struct CommandContext {
  char id[kResultIdSize];
  char nodeId[kNodeIdSize];
  char action[4];
  char expires[40];
  bool haveId, haveNode, haveAction, haveExpires;
};

Field onCommandField(void *raw, Scanner &scanner, const char *key) {
  CommandContext &c = *static_cast<CommandContext *>(raw);
  bool ok = true;
  if (strcmp(key, "id") == 0) {
    ok = readStringField(scanner, c.id, sizeof(c.id), &c.haveId);
  } else if (strcmp(key, "nodeId") == 0) {
    ok = readStringField(scanner, c.nodeId, sizeof(c.nodeId), &c.haveNode);
  } else if (strcmp(key, "action") == 0) {
    ok = readStringField(scanner, c.action, sizeof(c.action), &c.haveAction);
  } else if (strcmp(key, "expiresAt") == 0) {
    ok = readStringField(scanner, c.expires, sizeof(c.expires), &c.haveExpires);
  } else {
    return Field::Unknown;
  }
  return ok ? Field::Consumed : Field::Malformed;
}

struct ReadingsContext {
  ReadingsAnswer *out;
  bool haveAck;
  bool haveTime;
};

bool onCommandElement(void *raw, Scanner &scanner) {
  ReadingsContext &context = *static_cast<ReadingsContext *>(raw);
  ReadingsAnswer &answer = *context.out;
  if (scanner.peek() != '{') {
    answer.badCommands++;
    return scanner.skipValue(Scanner::kInnerDepth + 1);
  }
  CommandContext command;
  memset(&command, 0, sizeof(command));
  if (!scanner.object(onCommandField, &command, Scanner::kInnerDepth + 1)) {
    return false;
  }
  uint32_t expiresAt = 0;
  const bool good = command.haveId && isSafeResultId(command.id) && command.haveNode && command.nodeId[0] != '\0' && isNodeIdText(command.nodeId) && command.haveAction &&
                    (strcmp(command.action, "on") == 0 || strcmp(command.action, "off") == 0) && command.haveExpires &&
                    parseIsoTime(command.expires, &expiresAt);
  if (!good) {
    answer.badCommands++;
  } else if (answer.commandCount >= kMaxAnswerCommands) {
    answer.extraCommands++;
  } else {
    RelayCommandAnswer &slot = answer.commands[answer.commandCount++];
    memcpy(slot.id, command.id, sizeof(slot.id));
    memcpy(slot.nodeId, command.nodeId, sizeof(slot.nodeId));
    slot.on = strcmp(command.action, "on") == 0;
    slot.expiresAt = expiresAt;
  }
  return true;
}

Field onReadingsField(void *raw, Scanner &scanner, const char *key) {
  ReadingsContext &context = *static_cast<ReadingsContext *>(raw);
  ReadingsAnswer &answer = *context.out;
  if (strcmp(key, "ackSeq") == 0) {
    return readUintField(scanner, &answer.ackSeq, &context.haveAck) ? Field::Consumed : Field::Malformed;
  }
  if (strcmp(key, "serverTime") == 0) {
    char text[40];
    bool valid = false;
    if (!readStringField(scanner, text, sizeof(text), &valid)) {
      return Field::Malformed;
    }
    context.haveTime = valid && parseIsoTime(text, &answer.serverTime);
    return Field::Consumed;
  }
  if (strcmp(key, "commands") == 0) {
    if (scanner.peek() != '[') {
      // A value that is not a list holds no commands. The batch is stored anyway, so this is not a reason to fail.
      return Field::Unknown;
    }
    // A second list replaces the first.
    answer.commandCount = 0;
    answer.badCommands = 0;
    answer.extraCommands = 0;
    return scanner.array(onCommandElement, &context, Scanner::kInnerDepth) ? Field::Consumed : Field::Malformed;
  }
  return Field::Unknown;
}

// ---- The error body

struct ErrorContext {
  ErrorAnswer *out;
};

Field onErrorField(void *raw, Scanner &scanner, const char *key) {
  ErrorAnswer &answer = *static_cast<ErrorContext *>(raw)->out;
  bool valid = false;
  if (strcmp(key, "code") == 0) {
    char text[kErrorCodeSize];
    memset(text, 0, sizeof(text));
    if (!readStringField(scanner, text, sizeof(text), &valid)) {
      return Field::Malformed;
    }
    if (valid) {
      memcpy(answer.code, text, sizeof(answer.code));
    } else {
      answer.code[0] = '\0';
    }
    return Field::Consumed;
  }
  if (strcmp(key, "expectedSeq") == 0) {
    uint32_t value = 0;
    if (!readUintField(scanner, &value, &valid)) {
      return Field::Malformed;
    }
    answer.hasExpectedSeq = valid;
    answer.expectedSeq = valid ? value : 0;
    return Field::Consumed;
  }
  if (strcmp(key, "retryAfterSeconds") == 0) {
    uint32_t value = 0;
    if (!readUintField(scanner, &value, &valid)) {
      return Field::Malformed;
    }
    answer.retryAfterSeconds = valid ? value : 0;
    return Field::Consumed;
  }
  return Field::Unknown;
}

// ---- The answer of the enroll route

struct EnrollContext {
  EnrollAnswer *out;
  bool haveCode;
  bool havePoll;
  bool haveExpires;
};

bool isPollIdChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_'; }

Field onEnrollField(void *raw, Scanner &scanner, const char *key) {
  EnrollContext &context = *static_cast<EnrollContext *>(raw);
  EnrollAnswer &answer = *context.out;
  if (strcmp(key, "claimCode") == 0) {
    if (!readStringField(scanner, answer.claimCode, sizeof(answer.claimCode), &context.haveCode)) {
      return Field::Malformed;
    }
    // The code has 8 characters from the alphabet of the service (A to Z without I and O, and 2 to 9), and the hub shows it, so any other text is not shown.
    if (context.haveCode) {
      context.haveCode = strlen(answer.claimCode) == kClaimCodeSize - 1;
      for (size_t i = 0; context.haveCode && i < kClaimCodeSize - 1; i++) {
        const char c = answer.claimCode[i];
        context.haveCode = (c >= 'A' && c <= 'Z' && c != 'I' && c != 'O') || (c >= '2' && c <= '9');
      }
    }
    return Field::Consumed;
  }
  if (strcmp(key, "pollId") == 0) {
    if (!readStringField(scanner, answer.pollId, sizeof(answer.pollId), &context.havePoll)) {
      return Field::Malformed;
    }
    // The poll id goes into a path without escaping, so it has 32 to 64 characters that need none.
    if (context.havePoll) {
      const size_t length = strlen(answer.pollId);
      context.havePoll = length >= 32;
      for (size_t i = 0; context.havePoll && i < length; i++) {
        context.havePoll = isPollIdChar(answer.pollId[i]);
      }
    }
    return Field::Consumed;
  }
  if (strcmp(key, "expiresInSeconds") == 0) {
    return readUintField(scanner, &answer.expiresInSeconds, &context.haveExpires) ? Field::Consumed : Field::Malformed;
  }
  return Field::Unknown;
}

// A body that was cut at the end of the buffer (truncated) counts as read when the scan ran out of bytes and not when it met bytes that are wrong. The
// caller then checks that the fields that it needs were read before the cut.
bool scanWholeObject(const char *body, size_t length, bool truncated, FieldFn fn, void *context) {
  if (body == nullptr || length == 0 || length > kMaxAnswerBytes) {
    return false;
  }
  Scanner scanner(body, length);
  if (scanner.object(fn, context, 0) && scanner.atEnd()) {
    return true;
  }
  return truncated && scanner.stoppedAtEnd();
}

}  // namespace

bool parseIsoTime(const char *text, uint32_t *unixSeconds) {
  if (text == nullptr) {
    return false;
  }
  const char *p = text;
  const int year = digits(&p, 4);
  if (year < 1970 || !expect(&p, '-')) {
    return false;
  }
  const int month = digits(&p, 2);
  if (month < 1 || month > 12 || !expect(&p, '-')) {
    return false;
  }
  const int day = digits(&p, 2);
  if (day < 1 || day > daysInMonth(year, month) || !expect(&p, 'T')) {
    return false;
  }
  const int hour = digits(&p, 2);
  if (hour < 0 || hour > 23 || !expect(&p, ':')) {
    return false;
  }
  const int minute = digits(&p, 2);
  if (minute < 0 || minute > 59 || !expect(&p, ':')) {
    return false;
  }
  const int second = digits(&p, 2);
  if (second < 0 || second > 59) {
    return false;
  }
  if (*p == '.') {
    p++;
    if (*p < '0' || *p > '9') {
      return false;
    }
    while (*p >= '0' && *p <= '9') {
      p++;
    }
  }
  int64_t offsetSeconds = 0;
  if (*p == 'Z') {
    p++;
  } else if (*p == '+' || *p == '-') {
    const bool negative = *p == '-';
    p++;
    const int offsetHour = digits(&p, 2);
    if (offsetHour < 0 || offsetHour > 23 || !expect(&p, ':')) {
      return false;
    }
    const int offsetMinute = digits(&p, 2);
    if (offsetMinute < 0 || offsetMinute > 59) {
      return false;
    }
    offsetSeconds = (offsetHour * 3600 + offsetMinute * 60) * (negative ? -1 : 1);
  } else {
    return false;
  }
  if (*p != '\0') {
    return false;
  }
  // Days since 1970-01-01 for the civil date, which works from the year that starts in March.
  int64_t y = year;
  const int64_t m = month;
  y -= m <= 2 ? 1 : 0;
  const int64_t era = y / 400;
  const int64_t yearOfEra = y - era * 400;
  const int64_t dayOfYear = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const int64_t days = era * 146097 + dayOfEra - 719468;
  const int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offsetSeconds;
  if (seconds < 0 || seconds > 0xFFFFFFFFll) {
    return false;
  }
  *unixSeconds = static_cast<uint32_t>(seconds);
  return true;
}

bool parseReadingsAnswer(const char *body, size_t length, ReadingsAnswer *out, bool truncated) {
  memset(out, 0, sizeof(*out));
  ReadingsContext context;
  context.out = out;
  context.haveAck = false;
  context.haveTime = false;
  if (!scanWholeObject(body, length, truncated, onReadingsField, &context) || !context.haveAck || !context.haveTime) {
    memset(out, 0, sizeof(*out));
    return false;
  }
  return true;
}

bool parseEnrollAnswer(const char *body, size_t length, EnrollAnswer *out) {
  memset(out, 0, sizeof(*out));
  EnrollContext context;
  context.out = out;
  context.haveCode = false;
  context.havePoll = false;
  context.haveExpires = false;
  if (!scanWholeObject(body, length, false, onEnrollField, &context) || !context.haveCode || !context.havePoll || !context.haveExpires) {
    memset(out, 0, sizeof(*out));
    return false;
  }
  return true;
}

bool parseErrorAnswer(const char *body, size_t length, ErrorAnswer *out) {
  memset(out, 0, sizeof(*out));
  ErrorContext context;
  context.out = out;
  if (!scanWholeObject(body, length, false, onErrorField, &context)) {
    memset(out, 0, sizeof(*out));
    return false;
  }
  return true;
}

}  // namespace wattlink
