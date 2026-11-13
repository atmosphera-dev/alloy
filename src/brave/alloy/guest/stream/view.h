// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_VIEW_H_
#define BRAVE_ALLOY_GUEST_STREAM_VIEW_H_

#include <functional>
#include "brave/alloy/guest/session/extensions.h"
#include "brave/alloy/guest/stream/frame_pool.h"
#include "cef/include/cef_browser.h"

namespace alloy::guest::stream {
using StreamEvent = std::function<void(Owner, std::string, std::string)>;
using SendResource = std::function<bool(Envelope, const std::vector<int>&)>;
class View {
 public:
  View(Owner owner, int width, int height, StreamEvent event,
       SendResource resource, std::function<uint64_t()> clock);
  void Bind(CefRefPtr<CefBrowser> browser);
  void SetOwner(Owner owner);
  void Retire();
  CefRect rect() const { return CefRect(0, 0, width_, height_); }
  bool Screen(CefScreenInfo& screen) const;
  bool ScreenPoint(int x, int y, int& screen_x, int& screen_y) const;
  void Paint(uint64_t surface, const void* pixels, int width, int height);
  void Call(const Owner& owner, const std::string& operation,
            const std::string& payload, ProviderReply reply);
  void Release(const Owner& original, const std::string& payload, ProviderReply reply);
 private:
  void Configured();
  Owner owner_;
  int width_;
  int height_;
  double scale_ = 1;
  int fps_ = 30;
  uint64_t generation_ = 1;
  uint64_t byte_budget_ = 0;
  bool started_ = false;
  bool retired_ = false;
  bool starved_ = false;
  FramePool pool_;
  CefRefPtr<CefBrowser> browser_;
  const StreamEvent event_;
  const SendResource resource_;
  const std::function<uint64_t()> clock_;
};
}  // namespace alloy::guest::stream
#endif
