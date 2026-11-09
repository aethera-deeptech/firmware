#pragma once

#include <stddef.h>
#include <stdint.h>

#include "batch-builder.h"
#include "node-registry.h"

namespace wattlink {

// Reads the answers of the backend to the hub. The contract is openapi.yaml of the backend (the routes under /device/v1) and its docs/firmware-guide.md.
// The scanner has a fixed depth and a fixed size for every text, uses no heap and never reads past the length that it is given. An answer is data from the
// network, so a text that is too long, a number out of range or a value of the wrong type makes the field invalid and never a crash. A key that the scanner does
// not know is skipped, so a field that the backend adds later does not break the hub.

// The largest answer that the hub reads. The client reads at most this many bytes of a body, and an answer that is longer fails to parse and is retried.
constexpr size_t kMaxAnswerBytes = 8192;

// The commands that one answer can hand over. A command that does not fit comes again in the next answer, because the service repeats a command until
// its result is reported, so dropping the rest loses nothing.
constexpr size_t kMaxAnswerCommands = 24;

constexpr size_t kErrorCodeSize = 41;
constexpr size_t kClaimCodeSize = 9;
constexpr size_t kPollIdSize = 65;

struct RelayCommandAnswer {
  char id[kResultIdSize];
  char nodeId[kNodeIdSize];
  bool on;
  uint32_t expiresAt;  // Unix seconds
};

// The answer 200 of POST /device/v1/readings.
struct ReadingsAnswer {
  uint32_t ackSeq;
  uint32_t serverTime;  // Unix seconds
  size_t commandCount;
  // The commands in the order of the answer. A command with an id or a node id that does not pass, with an action that is not on or off, or with an
  // expiry that is not a time is not listed and is counted in badCommands. A command beyond kMaxAnswerCommands is counted in extraCommands.
  RelayCommandAnswer commands[kMaxAnswerCommands];
  size_t badCommands;
  size_t extraCommands;
};

// The body of an error answer (the schema Error). Every field is optional, so a body that is not what is expected gives an empty code and no numbers.
struct ErrorAnswer {
  char code[kErrorCodeSize];
  bool hasExpectedSeq;
  uint32_t expectedSeq;
  uint32_t retryAfterSeconds;  // 0 when the answer names none
};

// The answer 201 of POST /device/v1/enroll.
struct EnrollAnswer {
  char claimCode[kClaimCodeSize];
  char pollId[kPollIdSize];
  uint32_t expiresInSeconds;
};

// Each function returns false when the body is not a JSON object, when it is cut short or has bytes after the object, or when a field that the answer
// needs is missing or invalid. The out value is then not to be used.
bool parseReadingsAnswer(const char *body, size_t length, ReadingsAnswer *out);
bool parseEnrollAnswer(const char *body, size_t length, EnrollAnswer *out);

// An error body never makes the hub fail, because the status says what happened. The function fills what it can, and returns false only when the body is
// not a JSON object (the out value is empty then).
bool parseErrorAnswer(const char *body, size_t length, ErrorAnswer *out);

// Reads a time of the form 2026-11-03T08:01:02Z, with an optional fraction of a second and an offset such as +07:00 in place of the Z. Returns false for any other
// form, for a day or an hour that does not exist, and for a time before 1970 or after 2106 (it must fit 32 bits).
bool parseIsoTime(const char *text, uint32_t *unixSeconds);

}  // namespace wattlink
