// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/view.h"

#include <cmath>
#include <limits>
#include <utility>
#include "base/strings/string_number_conversions.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/point.h"

namespace alloy::guest::stream {
namespace {
// Match the exact float scaling used by pinned CEF's SizeInPixels and
// GetCompositorViewportPixelSize, not a double approximation of that geometry.
bool Pixels(int width, int height, double scale, gfx::Size* result) {
  const float native = static_cast<float>(scale);
  const float physical_width = static_cast<float>(width) * native;
  const float physical_height = static_cast<float>(height) * native;
  if (width <= 0 || height <= 0 || !std::isfinite(native) || native <= 0 ||
      !std::isfinite(physical_width) || !std::isfinite(physical_height) ||
      std::ceil(static_cast<double>(physical_width)) > std::numeric_limits<int>::max() ||
      std::ceil(static_cast<double>(physical_height)) > std::numeric_limits<int>::max()) return false;
  *result = gfx::ScaleToCeiledSize(gfx::Size(width, height), native);
  return !result->IsEmpty();
}
}
View::View(Owner owner, int width, int height, StreamEvent event,
           SendResource resource, std::function<uint64_t()> clock)
    : owner_(owner), width_(width), height_(height), event_(std::move(event)),
      resource_(std::move(resource)), clock_(std::move(clock)) {}
void View::Bind(CefRefPtr<CefBrowser> browser) { browser_ = browser; }
bool View::PixelSize(CefSize& size) const {
  gfx::Size pixels;
  if (!Pixels(width_, height_, scale_, &pixels)) return false;
  size = CefSize(pixels.width(), pixels.height());
  return true;
}
bool View::InitialGeometry(int width, int height, double scale) {
  size_t stride = 0, bytes = 0;
  scale = static_cast<float>(scale);
  gfx::Size physical;
  if (started_ || retired_ || !Pixels(width, height, scale, &physical) ||
      !FrameBytes(physical.width(), physical.height(), &stride, &bytes)) return false;
  width_ = width; height_ = height; scale_ = scale;
  return true;
}
void View::SetOwner(Owner owner) {
  if (retired_ || owner == owner_) return;
  if (owner.document != owner_.document) {
    input_.Reset(); popup_visible_ = false; popup_ = CefRect();
  }
  owner_ = owner;
  if (generation_ == std::numeric_limits<uint64_t>::max()) {
    Retire();
    return;
  }
  ++generation_;
  Configured();
  PopupState();
}
void View::Retire() {
  retired_ = true;
  pool_.Retire();
  browser_ = nullptr;
}
bool View::Screen(CefScreenInfo& screen) const {
  screen.device_scale_factor = scale_;
  screen.depth = 32;
  screen.depth_per_component = 8;
  screen.rect = rect();
  screen.available_rect = rect();
  return true;
}
bool View::ScreenPoint(int x, int y, int& screen_x, int& screen_y) const {
  const double sx = std::round(x * scale_), sy = std::round(y * scale_);
  if (sx < std::numeric_limits<int>::min() || sx > std::numeric_limits<int>::max() ||
      sy < std::numeric_limits<int>::min() || sy > std::numeric_limits<int>::max())
    return false;
  const auto point = gfx::ScaleToRoundedPoint(gfx::Point(x, y), static_cast<float>(scale_));
  screen_x = point.x(); screen_y = point.y();
  return true;
}
void View::Configured() {
  if (!started_ || retired_) return;
  std::string payload;
  PackFields({std::to_string(generation_), std::to_string(width_),
               std::to_string(height_), base::NumberToString(scale_)}, &payload);
  event_(owner_, "configured", std::move(payload));
  if (browser_) browser_->GetHost()->Invalidate(PET_VIEW);
}
void View::Paint(uint64_t surface, const void* pixels, int width, int height) {
  if (retired_ || !started_) return;
  gfx::Size physical;
  const auto dimensions = surface ? popup_ : rect();
  if ((surface && !popup_visible_) || !Pixels(dimensions.width, dimensions.height, scale_, &physical)) return;
  // CEF may deliver an old-size paint while a resize is in flight. It must not
  // acquire the replacement geometry's identity.
  if (width != physical.width() || height != physical.height()) return;
  auto info = pool_.Capture(owner_, generation_, surface, width, height, pixels, clock_());
  if (!info) { starved_ = true; return; }
  std::string description, packet;
  PackFrame(*info, &description);
  PackExtension({Feature::kStream, info->token, "frame", description}, &packet);
  if (!resource_({Message::kExtensionResource, {}, 0, owner_, std::move(packet)},
                  {pool_.Handle(info->token)})) {
    pool_.Drop(info->owner, info->token);
    starved_ = true;
    return;
  }
  // A control announcement is the ordering barrier. A resource arriving first
  // cannot be presented until control admits this exact original owner/token.
  event_(owner_, "frame", std::move(description));
}
void View::PopupState() {
  if (!started_ || retired_) return;
  std::string payload;
  PackFields({std::to_string(generation_), popup_visible_ ? "1" : "0", std::to_string(popup_.x),
               std::to_string(popup_.y), std::to_string(popup_.width), std::to_string(popup_.height), "DIP"}, &payload);
  event_(owner_, "popup", std::move(payload));
}
void View::PopupShow(bool visible) {
  if (retired_ || popup_visible_ == visible) return;
  popup_visible_ = visible;
  if (generation_ == std::numeric_limits<uint64_t>::max()) { Retire(); return; }
  ++generation_; Configured(); PopupState();
}
void View::PopupSize(const CefRect& rectangle) {
  if (retired_ || rectangle.width <= 0 || rectangle.height <= 0) return;
  popup_ = rectangle;
  if (generation_ == std::numeric_limits<uint64_t>::max()) { Retire(); return; }
  ++generation_; Configured(); PopupState();
}
void View::Cursor(cef_cursor_type_t type, const CefCursorInfo& info) {
  if (!started_ || retired_ || type < 0 || type >= CT_NUM_VALUES) return;
  size_t stride = 0, bytes = 0;
  std::string pixels;
  const bool custom = type == CT_CUSTOM;
  if (custom) {
    if (!info.buffer || !FrameBytes(info.size.width, info.size.height, &stride, &bytes) ||
        bytes > kMaxControlPayload || !std::isfinite(info.image_scale_factor) || info.image_scale_factor <= 0 ||
        info.hotspot.x < 0 || info.hotspot.y < 0 || info.hotspot.x >= info.size.width || info.hotspot.y >= info.size.height) {
      event_(owner_, "unavailable", {}); return;
    }
    pixels.assign(static_cast<const char*>(info.buffer), bytes);
  }
  std::string payload;
  if (!PackFields({std::to_string(generation_), std::to_string(type),
      std::to_string(custom ? info.size.width : 0), std::to_string(custom ? info.size.height : 0),
      std::to_string(custom ? info.hotspot.x : 0), std::to_string(custom ? info.hotspot.y : 0),
      base::NumberToString(custom ? info.image_scale_factor : 1.0f), std::move(pixels)}, &payload)) {
    event_(owner_, "unavailable", {}); return;
  }
  event_(owner_, "cursor", std::move(payload));
}
void View::Ime(const CefRange& selection, const std::vector<CefRect>& bounds) {
  if (!started_ || retired_) return;
  auto data = CefDictionaryValue::Create();
  auto range = CefListValue::Create();
  range->SetInt(0, selection.from); range->SetInt(1, selection.to); data->SetList("selection", range);
  auto rectangles = CefListValue::Create();
  for (size_t i = 0; i < bounds.size(); ++i) {
    auto rectangle = CefListValue::Create();
    rectangle->SetInt(0, bounds[i].x); rectangle->SetInt(1, bounds[i].y);
    rectangle->SetInt(2, bounds[i].width); rectangle->SetInt(3, bounds[i].height);
    rectangles->SetList(i, rectangle);
  }
  data->SetList("bounds", rectangles);
  std::string payload;
  if (PackFields({std::to_string(generation_), WriteDetails(data)}, &payload))
    event_(owner_, "ime-bounds", std::move(payload));
  else event_(owner_, "unavailable", {});
}
void View::Call(const Owner& owner, const std::string& operation,
                const std::string& payload, bool document_ready, ProviderReply reply) {
  if (retired_ || owner != owner_ || !browser_) {
    reply(ProviderResult::kCancelled, {});
    return;
  }
  std::vector<std::string> fields;
  uint64_t values[5] = {};
  if (operation == "input") {
    InteractionData input;
    if (!started_ || !UnpackInteraction(payload, &input) || !FitsInteraction(input)) {
      reply(ProviderResult::kFailed, {}); return;
    }
    auto receipt = [this](Owner original, InteractionData value) {
      std::string packed;
      if (PackInteraction(value, &packed)) event_(original, kInteractionOperation, std::move(packed));
    };
    if (!document_ready) {
      input.dispatch_us = clock_(); input.outcome = "rejected:document-not-ready";
      receipt(owner, std::move(input));
    } else {
      input_.Dispatch(owner, generation_, rect(), popup_visible_, popup_, browser_, std::move(input), clock_, std::move(receipt));
    }
  } else if (operation == "start") {
    size_t stride = 0, bytes = 0;
    gfx::Size physical;
    if (started_ || !UnpackFields(payload, 2, &fields) ||
        !ParseUnsigned(fields[0], &values[0]) ||
        values[0] > std::numeric_limits<uint32_t>::max() ||
        !ParseUnsigned(fields[1], &values[1]) ||
        !Pixels(width_, height_, scale_, &physical) ||
        !FrameBytes(physical.width(), physical.height(), &stride, &bytes) ||
        bytes > values[1] || !pool_.Configure(values[0], values[1])) {
      reply(ProviderResult::kFailed, {});
      return;
    }
    started_ = true;
    byte_budget_ = values[1];
    Configured();
  } else if (operation == "configure") {
    double scale = 0;
    int width = 0, height = 0;
    gfx::Size physical;
    size_t stride = 0, bytes = 0;
    if (!started_ || !UnpackFields(payload, 4, &fields) ||
        !base::StringToInt(fields[0], &width) || !base::StringToInt(fields[1], &height) ||
        !base::StringToDouble(fields[2], &scale) ||
        !std::isfinite(static_cast<float>(scale)) || static_cast<float>(scale) <= 0 ||
        !ParseUnsigned(fields[3], &values[3]) || values[3] < 1 || values[3] > 60 ||
        !Pixels(width, height, scale, &physical) ||
        !FrameBytes(physical.width(), physical.height(), &stride, &bytes) ||
        bytes > byte_budget_ ||
        generation_ == std::numeric_limits<uint64_t>::max()) {
      reply(ProviderResult::kFailed, {});
      return;
    }
    width_ = width; height_ = height; scale_ = static_cast<float>(scale); fps_ = values[3];
    ++generation_;
    browser_->GetHost()->SetWindowlessFrameRate(fps_);
    browser_->GetHost()->NotifyScreenInfoChanged();
    browser_->GetHost()->WasResized();
    Configured();
  } else {
    reply(ProviderResult::kUnavailable, {});
    return;
  }
  reply(ProviderResult::kOk, {});
}
void View::Release(const Owner& original, const std::string& payload, ProviderReply reply) {
  uint64_t token = 0;
  bool released = ParseUnsigned(payload, &token) && original.view == owner_.view &&
                   pool_.Release(original, token);
  if (released && !retired_ && browser_ && std::exchange(starved_, false))
    browser_->GetHost()->Invalidate(PET_VIEW);
  reply(released ? ProviderResult::kOk : ProviderResult::kFailed, {});
}
}  // namespace alloy::guest::stream
