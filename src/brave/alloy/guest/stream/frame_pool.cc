// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/frame_pool.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "brave/alloy/guest/session/extensions.h"

namespace alloy::guest::stream {
bool FrameBytes(int width, int height, size_t* stride, size_t* bytes) {
  if (width <= 0 || height <= 0 ||
      static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / 4)
    return false;
  *stride = static_cast<size_t>(width) * 4;
  if (static_cast<size_t>(height) >
      static_cast<size_t>(std::numeric_limits<off_t>::max()) / *stride)
    return false;
  *bytes = *stride * static_cast<size_t>(height);
  return true;
}
bool PackFrame(const FrameInfo& info, std::string* payload) {
  return PackFields({std::to_string(info.token), std::to_string(info.generation),
      std::to_string(info.sequence), std::to_string(info.dropped),
      std::to_string(info.time_us), std::to_string(info.surface),
      std::to_string(info.width), std::to_string(info.height),
      std::to_string(info.stride), std::to_string(info.bytes),
      "BGRA8-premultiplied-sRGB-full"}, payload);
}
bool UnpackFrame(const Owner& owner, const std::string& payload, FrameInfo* info) {
  std::vector<std::string> fields;
  uint64_t values[10] = {};
  if (!UnpackFields(payload, 11, &fields) ||
      fields[10] != "BGRA8-premultiplied-sRGB-full") return false;
  for (size_t i = 0; i < 10; ++i)
    if (!ParseUnsigned(fields[i], &values[i])) return false;
  if (!values[0] || !values[1] || !values[2] || values[5] > 1 ||
      values[6] > std::numeric_limits<int>::max() ||
      values[7] > std::numeric_limits<int>::max()) return false;
  size_t stride = 0, bytes = 0;
  if (!FrameBytes(values[6], values[7], &stride, &bytes) ||
      stride != values[8] || bytes != values[9]) return false;
  *info = {owner, values[0], values[1], values[2], values[3], values[4], values[5],
           static_cast<int>(values[6]), static_cast<int>(values[7]), stride, bytes};
  return true;
}

struct FramePool::Slot {
  ~Slot() { if (mapping != MAP_FAILED) munmap(mapping, bytes); }
  base::ScopedFD handle;
  void* mapping = MAP_FAILED;
  size_t bytes = 0;
  FrameInfo info;
  bool leased = false;
};
FramePool::FramePool() = default;
FramePool::~FramePool() = default;
bool FramePool::Configure(uint32_t slots, uint64_t byte_budget) {
  if (retired_ || capacity_ || !slots || !byte_budget ||
      byte_budget > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
      slots > byte_budget / 4) return false;
  capacity_ = slots;
  budget_ = byte_budget;
  return true;
}
std::optional<FrameInfo> FramePool::Capture(
    Owner owner, uint64_t generation, uint64_t surface, int width, int height,
    const void* pixels, uint64_t time_us) {
  if (retired_ || !capacity_ || surface > 1 || !generation || !pixels ||
      sequence_[surface] == std::numeric_limits<uint64_t>::max() ||
      next_token_ == std::numeric_limits<uint64_t>::max()) return std::nullopt;
  ++sequence_[surface];
  size_t stride = 0, bytes = 0;
  if (!FrameBytes(width, height, &stride, &bytes) || bytes > budget_) {
    ++dropped_[surface];
    return std::nullopt;
  }
  Slot* selected = nullptr;
  uint64_t allocated = 0;
  for (const auto& slot : slots_) {
    allocated += slot->bytes;
    if (!slot->leased && slot->bytes == bytes) selected = slot.get();
  }
  if (!selected) {
    // Free unused extents, never a live lease, before admitting another size.
    std::erase_if(slots_, [&allocated](const auto& slot) {
      if (slot->leased) return false;
      allocated -= slot->bytes;
      return true;
    });
    if (slots_.size() >= capacity_ || bytes > budget_ - allocated) {
      ++dropped_[surface];
      return std::nullopt;
    }
    auto slot = std::make_unique<Slot>();
    slot->handle.reset(memfd_create("alloy-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!slot->handle.is_valid() || ftruncate(slot->handle.get(), bytes)) {
      ++dropped_[surface];
      return std::nullopt;
    }
    slot->bytes = bytes;
    slot->mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED,
                          slot->handle.get(), 0);
    // Existing producer mapping stays writable; receivers cannot create a
    // writable mapping or modify/resize the memfd, even with a duplicated FD.
    if (slot->mapping == MAP_FAILED ||
        fcntl(slot->handle.get(), F_ADD_SEALS,
               F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL)) {
      ++dropped_[surface];
      return std::nullopt;
    }
    selected = slot.get();
    slots_.push_back(std::move(slot));
  }
  memcpy(selected->mapping, pixels, bytes);
  selected->info = {owner, next_token_++, generation, sequence_[surface],
                    dropped_[surface], time_us, surface, width, height,
                    stride, bytes};
  selected->leased = true;
  return selected->info;
}
int FramePool::Handle(uint64_t token) const {
  for (const auto& slot : slots_)
    if (slot->leased && slot->info.token == token) return slot->handle.get();
  return -1;
}
bool FramePool::Release(const Owner& owner, uint64_t token) {
  for (const auto& slot : slots_) {
    if (slot->leased && slot->info.owner == owner && slot->info.token == token) {
      slot->leased = false;
      return true;
    }
  }
  return false;
}
void FramePool::Drop(const Owner& owner, uint64_t token) {
  for (const auto& slot : slots_) {
    if (slot->leased && slot->info.owner == owner && slot->info.token == token) {
      ++dropped_[slot->info.surface];
      slot->leased = false;
      return;
    }
  }
}
void FramePool::Retire() { retired_ = true; }
}  // namespace alloy::guest::stream
