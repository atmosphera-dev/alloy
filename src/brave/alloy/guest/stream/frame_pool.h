// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_FRAME_POOL_H_
#define BRAVE_ALLOY_GUEST_STREAM_FRAME_POOL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "base/files/scoped_file.h"
#include "brave/alloy/guest/protocol/session.h"

namespace alloy::guest::stream {
// Full frames are always independently usable. Damage is conservatively the
// whole image, including after a gap. CPU OnPaint supplies no capture timestamp:
// time_us is the session-clock callback capture time, not source video time.
struct FrameInfo {
  Owner owner;
  uint64_t token = 0;
  uint64_t generation = 0;
  uint64_t sequence = 0;
  uint64_t dropped = 0;
  uint64_t time_us = 0;
  uint64_t surface = 0;
  int width = 0;
  int height = 0;
  size_t stride = 0;
  size_t bytes = 0;
};
bool FrameBytes(int width, int height, size_t* stride, size_t* bytes);
bool PackFrame(const FrameInfo& info, std::string* payload);
bool UnpackFrame(const Owner& owner, const std::string& payload, FrameInfo* info);

class FramePool {
 public:
  FramePool();
  ~FramePool();
  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;
  bool Configure(uint32_t slots, uint64_t byte_budget);
  // Only the producer sequence writes slots. A leased slot is immutable until
  // the exact original owner/token releases it; generations never reuse tokens.
  std::optional<FrameInfo> Capture(Owner owner, uint64_t generation,
                                  uint64_t surface, int width, int height,
                                  const void* pixels, uint64_t time_us);
  int Handle(uint64_t token) const;
  bool Release(const Owner& owner, uint64_t token);
  void Drop(const Owner& owner, uint64_t token);
  void Retire();
 private:
  struct Slot;
  std::vector<std::unique_ptr<Slot>> slots_;
  uint32_t capacity_ = 0;
  uint64_t budget_ = 0;
  uint64_t next_token_ = 1;
  uint64_t sequence_[2] = {};
  uint64_t dropped_[2] = {};
  bool retired_ = false;
};
}  // namespace alloy::guest::stream
#endif
