// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/protocol/session.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

namespace alloy::guest {
namespace {
constexpr std::array<uint8_t, 4> kMagic = {'A', 'G', 'S', '1'};

void Put(std::vector<uint8_t>* bytes, uint64_t value, size_t width) {
  for (size_t n = width; n > 0; --n)
    bytes->push_back(static_cast<uint8_t>(value >> (8 * (n - 1))));
}

uint64_t Get(const std::vector<uint8_t>& bytes, size_t start, size_t width) {
  uint64_t value = 0;
  for (size_t n = start; n < start + width; ++n)
    value = (value << 8) | bytes[n];
  return value;
}

bool Known(Message message) {
  return message >= Message::kHello && message <= Message::kExtensionResource;
}

bool Valid(const Envelope& envelope) {
  return Known(envelope.message) && envelope.sequence != 0 &&
         std::any_of(envelope.session.begin(), envelope.session.end(),
                     [](uint8_t byte) { return byte != 0; }) &&
         envelope.payload.size() <= kMaxControlPayload;
}
}  // namespace

bool Encode(const Envelope& envelope, std::vector<uint8_t>* bytes) {
  if (!bytes || !Valid(envelope))
    return false;
  bytes->clear();
  bytes->reserve(kEnvelopeSize + envelope.payload.size());
  bytes->insert(bytes->end(), kMagic.begin(), kMagic.end());
  Put(bytes, kProtocolVersion, 2);
  Put(bytes, static_cast<uint16_t>(envelope.message), 2);
  Put(bytes, envelope.payload.size(), 4);
  Put(bytes, 0, 4);  // Reserved. Unknown flags are incompatible, not ignored.
  bytes->insert(bytes->end(), envelope.session.begin(), envelope.session.end());
  Put(bytes, envelope.sequence, 8);
  Put(bytes, envelope.owner.view, 8);
  Put(bytes, envelope.owner.document, 8);
  Put(bytes, envelope.owner.request, 8);
  Put(bytes, envelope.owner.generation, 8);
  bytes->insert(bytes->end(), envelope.payload.begin(), envelope.payload.end());
  return true;
}

bool Decode(const std::vector<uint8_t>& bytes, Envelope* envelope) {
  if (!envelope || bytes.size() < kEnvelopeSize ||
      !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      Get(bytes, 4, 2) != kProtocolVersion || Get(bytes, 12, 4) != 0 ||
      Get(bytes, 8, 4) != bytes.size() - kEnvelopeSize)
    return false;
  Envelope decoded;
  decoded.message = static_cast<Message>(Get(bytes, 6, 2));
  std::copy_n(bytes.begin() + 16, decoded.session.size(), decoded.session.begin());
  decoded.sequence = Get(bytes, 32, 8);
  decoded.owner = {Get(bytes, 40, 8), Get(bytes, 48, 8), Get(bytes, 56, 8),
                   Get(bytes, 64, 8)};
  decoded.payload.assign(bytes.begin() + kEnvelopeSize, bytes.end());
  if (!Valid(decoded))
    return false;
  *envelope = std::move(decoded);
  return true;
}

bool SendEnvelope(int channel, const Envelope& envelope) {
  std::vector<uint8_t> bytes;
  if (!Encode(envelope, &bytes)) {
    errno = EINVAL;
    return false;
  }
  size_t offset = 0;
  while (offset < bytes.size()) {
    size_t count = std::min(bytes.size() - offset,
                             size_t{ALLOY_GUEST_CONTROL_CHUNK_SIZE});
    ssize_t size;
    do {
      size = send(channel, bytes.data() + offset, count, MSG_NOSIGNAL);
    } while (size < 0 && errno == EINTR);
    if (size != static_cast<ssize_t>(count))
      return false;
    offset += count;
  }
  return true;
}

bool ReceiveEnvelope(int channel, Envelope* envelope) {
  EnvelopeReader reader;
  return reader.Receive(channel, envelope);
}

bool EnvelopeReader::Receive(int channel, Envelope* envelope) {
  for (;;) {
  std::vector<uint8_t> bytes(ALLOY_GUEST_CONTROL_CHUNK_SIZE);
  // Linux's per-message SCM_MAX_FD is 253. A smaller ancillary buffer would
  // force truncation; MSG_CTRUNC still fails closed and the kernel closes any
  // descriptors that do not fit. Close every descriptor we do receive.
  alignas(cmsghdr) char control[CMSG_SPACE(253 * sizeof(int))] = {};
  iovec io = {bytes.data(), bytes.size()};
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = control;
  message.msg_controllen = sizeof(control);
  ssize_t size;
  do {
    size = recvmsg(channel, &message, MSG_CMSG_CLOEXEC);
  } while (size < 0 && errno == EINTR);
  bool handles = false;
  for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg;
       cmsg = CMSG_NXTHDR(&message, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
      handles = true;
      size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
      const int* fds = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
      for (size_t i = 0; i < count; ++i)
        close(fds[i]);
    } else {
      // Control peers never negotiate ancillary data in M1.
      handles = true;
    }
  }
  if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return false;
  if (size <= 0 || handles || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
    errno = EPROTO;
    incoming_.clear();
    expected_ = 0;
    return false;
  }
  bytes.resize(size);
  if (incoming_.empty()) {
    if (bytes.size() < kEnvelopeSize ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
        Get(bytes, 4, 2) != kProtocolVersion || Get(bytes, 12, 4) != 0 ||
        Get(bytes, 8, 4) > kMaxControlPayload) {
      errno = EPROTO;
      return false;
    }
    expected_ = kEnvelopeSize + Get(bytes, 8, 4);
    incoming_.reserve(expected_);
  }
  if (bytes.size() > expected_ - incoming_.size()) {
    incoming_.clear();
    expected_ = 0;
    errno = EPROTO;
    return false;
  }
  incoming_.insert(incoming_.end(), bytes.begin(), bytes.end());
  if (incoming_.size() == expected_) {
    bool result = Decode(incoming_, envelope);
    incoming_.clear();
    expected_ = 0;
    if (!result)
      errno = EPROTO;
    return result;
  }
  }
}

bool Peer::Send(int channel, const Envelope& envelope) {
  if (sent_ == std::numeric_limits<uint64_t>::max())
    return false;
  if (outgoing_.empty()) {
    Envelope value = envelope;
    value.session = session_;
    value.sequence = sent_ + 1;
    if (!Encode(value, &outgoing_)) {
      errno = EINVAL;
      return false;
    }
    offset_ = 0;
  }
  while (offset_ < outgoing_.size()) {
    size_t count = std::min(outgoing_.size() - offset_,
                             size_t{ALLOY_GUEST_CONTROL_CHUNK_SIZE});
    ssize_t size;
    do {
      size = send(channel, outgoing_.data() + offset_, count, MSG_NOSIGNAL);
    } while (size < 0 && errno == EINTR);
    if (size != static_cast<ssize_t>(count))
      return false;
    offset_ += count;
  }
  outgoing_.clear();
  offset_ = 0;
  ++sent_;
  return true;
}

bool Peer::Receive(int channel, Envelope* envelope) {
  Envelope value;
  if (received_ == std::numeric_limits<uint64_t>::max() ||
      !reader_.Receive(channel, &value))
    return false;
  if (value.session != session_ || value.sequence != received_ + 1) {
    errno = EPROTO;
    return false;
  }
  ++received_;
  *envelope = std::move(value);
  return true;
}

bool PackFields(const std::vector<std::string>& fields, std::string* payload) {
  std::string value;
  for (const auto& field : fields) {
    if (field.size() > kMaxControlPayload ||
        value.size() + 4 + field.size() > kMaxControlPayload)
      return false;
    value.push_back(static_cast<char>(field.size() >> 24));
    value.push_back(static_cast<char>(field.size() >> 16));
    value.push_back(static_cast<char>(field.size() >> 8));
    value.push_back(static_cast<char>(field.size()));
    value.append(field);
  }
  *payload = std::move(value);
  return true;
}

bool UnpackFields(const std::string& payload, size_t expected,
                  std::vector<std::string>* fields) {
  std::vector<std::string> value;
  size_t offset = 0;
  while (offset < payload.size() && value.size() < expected) {
    if (payload.size() - offset < 4)
      return false;
    size_t length = 0;
    for (size_t index = 0; index < 4; ++index)
      length = (length << 8) | static_cast<uint8_t>(payload[offset + index]);
    offset += 4;
    if (length > payload.size() - offset)
      return false;
    value.push_back(payload.substr(offset, length));
    offset += length;
  }
  if (offset != payload.size() || value.size() != expected)
    return false;
  *fields = std::move(value);
  return true;
}
}  // namespace alloy::guest
