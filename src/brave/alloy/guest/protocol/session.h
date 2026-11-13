// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_PROTOCOL_SESSION_H_
#define BRAVE_ALLOY_GUEST_PROTOCOL_SESSION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "brave/alloy/guest/protocol/wire.h"

namespace alloy::guest {

// Private control protocol, independent of the public CEF ABI and resource
// lanes. Integers are encoded big endian; no native struct layout is sent.
inline constexpr uint16_t kProtocolVersion = ALLOY_GUEST_PROTOCOL_VERSION;
inline constexpr size_t kEnvelopeSize = ALLOY_GUEST_ENVELOPE_SIZE;
// Packetization is independent of logical control length. The wire bound
// accommodates Chromium's actual URL limit; it is not a navigation quota.
inline constexpr size_t kMaxControlPayload = ALLOY_GUEST_MAX_CONTROL_PAYLOAD;
using SessionId = std::array<uint8_t, 16>;

enum class Message : uint16_t {
  kHello = 1,
  kPrepared,
  kAccept,
  kActive,
  kCreateView,
  kViewCreated,
  kNavigate,
  kTitle,
  kNavigationComplete,
  kCloseView,
  kViewClosed,
  kCancel,
  kClose,
  kClosed,
  kFailure,
  kUnavailable,
  kDocumentStarted,
  kGenerationChanged,
  // All extension providers share these ordered control slots. Resource FDs
  // travel only on the inherited resource lane, not in control messages.
  kExtensionCall,
  kExtensionReply,
  kExtensionEvent,
  kExtensionResource,
};
static_assert(static_cast<int>(Message::kClose) == ALLOY_GUEST_CLOSE_MESSAGE);
static_assert(static_cast<int>(Message::kClosed) == ALLOY_GUEST_CLOSED_MESSAGE);
static_assert(static_cast<int>(Message::kFailure) == ALLOY_GUEST_FAILURE_MESSAGE);

struct Owner {
  uint64_t view = 0;
  uint64_t document = 0;
  uint64_t request = 0;
  uint64_t generation = 0;
  bool operator==(const Owner&) const = default;
};

struct Envelope {
  Message message = Message::kFailure;
  SessionId session = {};
  uint64_t sequence = 0;
  Owner owner;
  std::string payload;
};

// Every receive returns an owned value. M1 control admits no resource handles;
// unexpected SCM_RIGHTS are closed before reporting failure, including on
// malformed/truncated packets. Later resource lanes retain this control owner.
bool Encode(const Envelope& envelope, std::vector<uint8_t>* bytes);
bool Decode(const std::vector<uint8_t>& bytes, Envelope* envelope);
bool SendEnvelope(int channel, const Envelope& envelope);
bool ReceiveEnvelope(int channel, Envelope* envelope);

class EnvelopeReader {
 public:
  // EAGAIN retains a partial frame. Other failures retire it. No native handle
  // is ever accepted on control, including in a continuation packet.
  bool Receive(int channel, Envelope* envelope);
 private:
  std::vector<uint8_t> incoming_;
  size_t expected_ = 0;
};

// Authentication comes from the inherited channel, never this token. Validate
// the negotiated session and exact ordered sequence before dispatching owners.
class Peer {
 public:
  explicit Peer(SessionId session, uint64_t received = 0)
      : session_(session), received_(received) {}
  bool Send(int channel, const Envelope& envelope);
  bool Receive(int channel, Envelope* envelope);
  bool HasPendingSend() const { return !outgoing_.empty(); }

 private:
  SessionId session_;
  uint64_t sent_ = 0;
  uint64_t received_ = 0;
  EnvelopeReader reader_;
  std::vector<uint8_t> outgoing_;
  size_t offset_ = 0;
};

// Length-prefixed fields avoid delimiter ambiguities and embedded-NUL paths.
bool PackFields(const std::vector<std::string>& fields, std::string* payload);
bool UnpackFields(const std::string& payload, size_t expected,
                  std::vector<std::string>* fields);

}  // namespace alloy::guest
#endif  // BRAVE_ALLOY_GUEST_PROTOCOL_SESSION_H_
