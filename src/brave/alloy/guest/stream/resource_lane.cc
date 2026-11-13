// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/resource_lane.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/socket.h>
#include <utility>

namespace alloy::guest::stream {
namespace {
// Linux SCM_MAX_FD is 253. Protocol payloads fit one atomic control-sized packet;
// large media lives in transferred resources, never an unbounded socket queue.
constexpr size_t kHandles = 253;
}
bool ResourceLane::Send(Envelope envelope, const std::vector<int>& handles) {
  if (sent_ == std::numeric_limits<uint64_t>::max() || handles.size() > kHandles)
    return false;
  envelope.message = Message::kExtensionResource;
  envelope.session = session_;
  envelope.sequence = sent_ + 1;
  std::vector<uint8_t> bytes;
  if (!Encode(envelope, &bytes) || bytes.size() > ALLOY_GUEST_CONTROL_CHUNK_SIZE)
    return false;
  alignas(cmsghdr) char ancillary[CMSG_SPACE(kHandles * sizeof(int))] = {};
  iovec io = {bytes.data(), bytes.size()};
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  if (!handles.empty()) {
    message.msg_control = ancillary;
    message.msg_controllen = CMSG_SPACE(handles.size() * sizeof(int));
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(handles.size() * sizeof(int));
    memcpy(CMSG_DATA(header), handles.data(), handles.size() * sizeof(int));
  }
  ssize_t count;
  do { count = sendmsg(fd_, &message, MSG_NOSIGNAL | MSG_DONTWAIT); }
  while (count < 0 && errno == EINTR);
  if (count != static_cast<ssize_t>(bytes.size())) return false;
  ++sent_;
  return true;
}
bool ResourceLane::Receive(Resource* resource) {
  std::vector<uint8_t> bytes(ALLOY_GUEST_CONTROL_CHUNK_SIZE);
  alignas(cmsghdr) char ancillary[CMSG_SPACE(kHandles * sizeof(int))] = {};
  iovec io = {bytes.data(), bytes.size()};
  msghdr message = {};
  message.msg_iov = &io;
  message.msg_iovlen = 1;
  message.msg_control = ancillary;
  message.msg_controllen = sizeof(ancillary);
  ssize_t count;
  do { count = recvmsg(fd_, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); }
  while (count < 0 && errno == EINTR);
  Resource value;
  bool invalid = false;
  for (auto* header = CMSG_FIRSTHDR(&message); header;
       header = CMSG_NXTHDR(&message, header)) {
    if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(0)) {
      invalid = true;
      continue;
    }
    size_t length = header->cmsg_len - CMSG_LEN(0);
    if (length % sizeof(int)) invalid = true;
    for (size_t i = 0; i < length / sizeof(int); ++i) {
      int fd;
      memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
      value.handles.emplace_back(fd);
    }
  }
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
  if (count <= 0 || invalid || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
    errno = EPROTO;
    return false;
  }
  bytes.resize(count);
  if (!Decode(bytes, &value.envelope) ||
      value.envelope.message != Message::kExtensionResource ||
      value.envelope.session != session_ ||
      received_ == std::numeric_limits<uint64_t>::max() ||
      value.envelope.sequence != received_ + 1) {
    errno = EPROTO;
    return false;
  }
  ++received_;
  *resource = std::move(value);
  return true;
}
}  // namespace alloy::guest::stream
