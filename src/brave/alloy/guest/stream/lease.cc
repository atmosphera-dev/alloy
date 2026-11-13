// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/lease.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <utility>

namespace alloy::guest::stream {
namespace {
class Frame final : public AlloyGuestFrame {
 public:
  Frame(FrameInfo info, void* pixels, std::function<void()> release)
      : info_(info), pixels_(pixels), release_(std::move(release)) {}
  uint64_t GetViewId() override { return info_.owner.view; }
  uint64_t GetDocumentId() override { return info_.owner.document; }
  uint64_t GetGeneration() override { return info_.generation; }
  uint64_t GetSurface() override { return info_.surface; }
  uint64_t GetSequence() override { return info_.sequence; }
  uint64_t GetDroppedCount() override { return info_.dropped; }
  uint64_t GetCaptureTimeUs() override { return info_.time_us; }
  int GetWidth() override { return info_.width; }
  int GetHeight() override { return info_.height; }
  size_t GetStride() override { return info_.stride; }
  size_t GetByteSize() override { return info_.bytes; }
  size_t ReadPixels(void* buffer, size_t size, size_t offset) override {
    if (!buffer || offset >= info_.bytes) return 0;
    size = std::min(size, info_.bytes - offset);
    memcpy(buffer, static_cast<const char*>(pixels_) + offset, size);
    return size;
  }
 private:
  ~Frame() override {
    munmap(pixels_, info_.bytes);
    release_();
  }
  const FrameInfo info_;
  void* const pixels_;
  const std::function<void()> release_;
  IMPLEMENT_REFCOUNTING(Frame);
};
}
CefRefPtr<AlloyGuestFrame> ImportFrame(FrameInfo info, base::ScopedFD handle,
                                      std::function<void()> release) {
  struct stat metadata = {};
  constexpr int seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL;
  const int actual_seals = handle.is_valid() ? fcntl(handle.get(), F_GET_SEALS) : -1;
  if (!handle.is_valid() || fstat(handle.get(), &metadata) ||
      !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
      static_cast<uint64_t>(metadata.st_size) != info.bytes ||
      actual_seals < 0 || (actual_seals & seals) != seals) {
    release();
    return nullptr;
  }
  void* pixels = mmap(nullptr, info.bytes, PROT_READ, MAP_SHARED, handle.get(), 0);
  if (pixels == MAP_FAILED) {
    release();
    return nullptr;
  }
  return new Frame(info, pixels, std::move(release));
}
}  // namespace alloy::guest::stream
