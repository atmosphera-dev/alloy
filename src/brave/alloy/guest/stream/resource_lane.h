// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_RESOURCE_LANE_H_
#define BRAVE_ALLOY_GUEST_STREAM_RESOURCE_LANE_H_

#include <vector>
#include "base/files/scoped_file.h"
#include "brave/alloy/guest/protocol/session.h"

namespace alloy::guest::stream {
struct Resource {
  Envelope envelope;
  std::vector<base::ScopedFD> handles;
};
// The inherited lane is only a capability carrier under the control session's
// identity. It cannot create views, admit documents or change session lifetime.
// Metadata is explicitly encoded; every received FD is RAII-owned, even on error.
class ResourceLane {
 public:
  ResourceLane(int fd, SessionId session) : fd_(fd), session_(session) {}
  bool Send(Envelope envelope, const std::vector<int>& handles);
  bool Receive(Resource* resource);
 private:
  const int fd_;
  const SessionId session_;
  uint64_t sent_ = 0;
  uint64_t received_ = 0;
};
}  // namespace alloy::guest::stream
#endif
