// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Explicit synthetic acceptance consumer of the translated public SDK only.
#include "include/alloy/alloy_guest_session.h"
#include "include/cef_api_hash.h"
#include "include/cef_parser.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::mutex output_mutex;
std::string Quote(const std::string& text) {
  std::ostringstream output;
  output << '"';
  for (unsigned char c : text) {
    if (c == '\\' || c == '"') output << '\\' << static_cast<char>(c);
    else if (c < 32) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
    else output << static_cast<char>(c);
  }
  output << '"';
  return output.str();
}
void Event(const std::string& event, const std::string& fields = {}) {
  std::lock_guard guard(output_mutex);
  std::cout << "GUEST_STREAM_EVENT {\"event\":" << Quote(event)
            << ",\"thread\":" << std::hash<std::thread::id>{}(std::this_thread::get_id())
            << fields << "}" << std::endl;
}
uint64_t Checksum(CefRefPtr<AlloyGuestFrame> frame) {
  std::vector<uint8_t> pixels(frame->GetByteSize());
  if (frame->ReadPixels(pixels.data(), pixels.size(), 0) != pixels.size()) return 0;
  uint64_t hash = 1469598103934665603ULL;
  for (auto byte : pixels) hash = (hash ^ byte) * 1099511628211ULL;
  return hash;
}
struct Held {
  CefRefPtr<AlloyGuestFrame> frame;
  uint64_t checksum;
};
class Observer final : public AlloyGuestStreamHandler {
 public:
  using Window = std::function<void(uint64_t, CefRefPtr<AlloyGuestPopup>, const CefString&,
                                    const CefString&, int, bool, const CefRect&)>;
  Observer(uint64_t view, std::function<void(CefRefPtr<AlloyGuestFrame>)> frame, Window window)
      : view_(view), frame_(std::move(frame)), window_(std::move(window)) {}
  void OnConfigured(uint64_t generation, int width, int height, double scale) override {
    Event("configured", Field() + ",\"generation\":" + std::to_string(generation) +
          ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
          ",\"scale\":" + std::to_string(scale));
  }
  void OnFrame(CefRefPtr<AlloyGuestFrame> frame) override { frame_(frame); }
  void OnInteraction(CefRefPtr<AlloyGuestInteraction> value) override {
    auto data = CefValue::Create(); data->SetDictionary(value->GetDetails());
    Event("interaction", Field() + ",\"id\":" + std::to_string(value->GetId()) +
          ",\"document\":" + std::to_string(value->GetDocumentId()) +
          ",\"generation\":" + std::to_string(value->GetGeneration()) +
          ",\"surface\":" + std::to_string(value->GetSurface()) +
          ",\"source\":" + Quote(value->GetSource().ToString()) +
          ",\"kind\":" + Quote(value->GetKind().ToString()) +
          ",\"capture_us\":" + std::to_string(value->GetAppCaptureTimeUs()) +
          ",\"dispatch_us\":" + std::to_string(value->GetDispatchTimeUs()) +
          ",\"outcome\":" + Quote(value->GetOutcome().ToString()) +
          ",\"details\":" + CefWriteJSON(data, JSON_WRITER_DEFAULT).ToString());
  }
  void OnPopup(bool visible, const CefRect& bounds, uint64_t generation) override {
    Event("popup", Field() + ",\"visible\":" + (visible ? "true" : "false") +
          ",\"x\":" + std::to_string(bounds.x) + ",\"y\":" + std::to_string(bounds.y) +
          ",\"width\":" + std::to_string(bounds.width) + ",\"height\":" + std::to_string(bounds.height) +
          ",\"generation\":" + std::to_string(generation));
  }
  void OnWindowOpen(CefRefPtr<AlloyGuestPopup> popup, const CefString& url,
      const CefString& name, int disposition, bool gesture, const CefRect& bounds) override {
    window_(view_, popup, url, name, disposition, gesture, bounds);
  }
  void OnWindowRetired(uint64_t id, const CefString& reason) override {
    Event("window-retired", Field() + ",\"id\":" + std::to_string(id) + ",\"reason\":" + Quote(reason.ToString()));
  }
  void OnCursor(int type, CefRefPtr<CefBinaryValue> pixels, int width, int height,
      int hx, int hy, double scale, uint64_t generation) override {
    std::vector<uint8_t> bytes(pixels ? pixels->GetSize() : 0);
    if (pixels) pixels->GetData(bytes.data(), bytes.size(), 0);
    uint64_t checksum = 1469598103934665603ULL;
    for (auto byte : bytes) checksum = (checksum ^ byte) * 1099511628211ULL;
    Event("cursor", Field() + ",\"type\":" + std::to_string(type) +
          ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
          ",\"hotspot_x\":" + std::to_string(hx) + ",\"hotspot_y\":" + std::to_string(hy) +
          ",\"scale\":" + std::to_string(scale) + ",\"generation\":" + std::to_string(generation) +
          ",\"bytes\":" + std::to_string(bytes.size()) + ",\"checksum\":" + std::to_string(checksum) +
          ",\"b\":" + std::to_string(bytes.empty() ? 0 : bytes[0]) +
          ",\"g\":" + std::to_string(bytes.empty() ? 0 : bytes[1]) +
          ",\"r\":" + std::to_string(bytes.empty() ? 0 : bytes[2]) +
          ",\"a\":" + std::to_string(bytes.empty() ? 0 : bytes[3]));
  }
  void OnImeBounds(const CefRange& selection, const std::vector<CefRect>& bounds, uint64_t generation) override {
    std::string rectangles = "[";
    for (const auto& rect : bounds) {
      if (rectangles.size() > 1) rectangles += ",";
      rectangles += "[" + std::to_string(rect.x) + "," + std::to_string(rect.y) + "," +
                      std::to_string(rect.width) + "," + std::to_string(rect.height) + "]";
    }
    Event("ime-bounds", Field() + ",\"generation\":" + std::to_string(generation) +
          ",\"from\":" + std::to_string(selection.from) + ",\"to\":" + std::to_string(selection.to) +
          ",\"bounds\":" + rectangles + "]");
  }
  void OnUnavailable() override { Event("unavailable", Field()); }
  void OnClosed() override { Event("stream-closed", Field()); }
 private:
  std::string Field() const { return ",\"view\":" + std::to_string(view_); }
  const uint64_t view_;
  const std::function<void(CefRefPtr<AlloyGuestFrame>)> frame_;
  const Window window_;
  IMPLEMENT_REFCOUNTING(Observer);
};
class Consumer final : public AlloyGuestSessionHandler,
                       public AlloyGuestViewHandler {
 public:
  void Start() {
    auto settings = AlloyCreateGuestSettings("memory", "keep", "unavailable", true, "");
    auto session = AlloyStartGuestSession(settings, this);
    std::lock_guard guard(mutex_);
    if (!session_) session_ = std::move(session);
  }
  void OnPrepared(CefRefPtr<AlloyGuestSession> session,
                    CefRefPtr<AlloyGuestProtection>,
                    CefRefPtr<AlloyGuestCapabilities> capabilities) override {
    { std::lock_guard guard(mutex_); session_ = session; }
    Event("prepared", ",\"stream\":" + std::string(capabilities->IsAvailable("stream") ? "true" : "false"));
    session->Accept();
  }
  void OnStateChanged(CefRefPtr<AlloyGuestSession> session, const CefString& state) override {
    Event("state", ",\"state\":" + Quote(state.ToString()));
    if (state == "ACTIVE") session->CreateView(128, 96, this);
  }
  void OnClosed(CefRefPtr<AlloyGuestSession>, const CefString& reason) override {
    Event("closed", ",\"reason\":" + Quote(reason.ToString()));
    std::lock_guard guard(mutex_);
    closed_ = true; session_ = nullptr; closed_cv_.notify_all();
  }
  void OnReady(CefRefPtr<AlloyGuestView> view) override {
    auto stream = view->GetStream();
    Geometry geometry{128, 96, 1};
    {
      std::lock_guard guard(mutex_);
      if (!view_) { view_ = view; stream_ = stream; }
      views_[view->GetId()] = view; streams_[view->GetId()] = stream;
      if (auto found = geometry_.find(view->GetId()); found != geometry_.end()) geometry = found->second;
    }
    Event("view-ready", ",\"view\":" + std::to_string(view->GetId()));
    auto self = CefRefPtr<Consumer>(this);
    CefRefPtr<Observer> observer = new Observer(view->GetId(), [self](CefRefPtr<AlloyGuestFrame> frame) {
      self->Frame(frame);
    }, [self](uint64_t parent, CefRefPtr<AlloyGuestPopup> popup, const CefString& url,
               const CefString& name, int disposition, bool gesture, const CefRect& bounds) {
      { std::lock_guard guard(self->mutex_); self->windows_[popup->GetId()] = popup; }
      Event("window-open", ",\"view\":" + std::to_string(parent) + ",\"id\":" + std::to_string(popup->GetId()) +
            ",\"url\":" + Quote(url.ToString()) + ",\"name\":" + Quote(name.ToString()) +
            ",\"disposition\":" + std::to_string(disposition) + ",\"gesture\":" + (gesture ? "true" : "false") +
            ",\"width\":" + std::to_string(bounds.width) + ",\"height\":" + std::to_string(bounds.height));
    });
    bool queued = stream && stream->Start(observer, 3, 8 * 1024 * 1024);
    Event("start-queued", ",\"ok\":" + std::string(queued ? "true" : "false"));
    if (queued) stream->Configure(geometry.width, geometry.height, geometry.scale, 30);
  }
  void OnTitleChanged(CefRefPtr<AlloyGuestView> view, uint64_t document, const CefString& title) override {
    Event("title", ",\"view\":" + std::to_string(view->GetId()) + ",\"document\":" + std::to_string(document) + ",\"title\":" + Quote(title.ToString()));
  }
  void OnNavigationComplete(CefRefPtr<AlloyGuestView> view, uint64_t document, int status) override {
    Event("load", ",\"view\":" + std::to_string(view->GetId()) + ",\"document\":" + std::to_string(document) + ",\"status\":" + std::to_string(status));
  }
  void OnClosed(CefRefPtr<AlloyGuestView> view, const CefString& reason) override {
    Event("view-closed", ",\"view\":" + std::to_string(view->GetId()) + ",\"reason\":" + Quote(reason.ToString()));
  }
  void Frame(CefRefPtr<AlloyGuestFrame> frame) {
    const auto checksum = Checksum(frame);
    uint8_t first[4] = {}, middle[4] = {}, last[4] = {}, left[4] = {}, right[4] = {};
    frame->ReadPixels(first, 4, 0);
    frame->ReadPixels(middle, 4, frame->GetStride() * (frame->GetHeight() / 2) + 4 * (frame->GetWidth() / 2));
    frame->ReadPixels(last, 4, frame->GetByteSize() - 4);
    frame->ReadPixels(left, 4, frame->GetStride() * (frame->GetHeight() / 2) + 4 * (frame->GetWidth() / 4));
    frame->ReadPixels(right, 4, frame->GetStride() * (frame->GetHeight() / 2) + 4 * (frame->GetWidth() * 3 / 4));
    bool pattern = first[3] == 255 && first[2] == (first[0] * 3) % 256 &&
                   first[1] == (first[0] * 5) % 256;
    for (int i = 0; i < 4; ++i) pattern = pattern && first[i] == middle[i] && first[i] == last[i];
    bool split = left[3] == 255 && right[3] == 255 &&
        left[2] == left[0] * 3 % 256 && left[1] == left[0] * 5 % 256 &&
        right[2] == right[0] * 3 % 256 && right[1] == right[0] * 5 % 256;
    for (int i = 0; i < 4; ++i) split = split && first[i] == left[i] && last[i] == right[i];
    Event("frame", ",\"view\":" + std::to_string(frame->GetViewId()) +
          ",\"document\":" + std::to_string(frame->GetDocumentId()) +
          ",\"generation\":" + std::to_string(frame->GetGeneration()) +
          ",\"surface\":" + std::to_string(frame->GetSurface()) +
          ",\"sequence\":" + std::to_string(frame->GetSequence()) +
          ",\"dropped\":" + std::to_string(frame->GetDroppedCount()) +
          ",\"capture_us\":" + std::to_string(frame->GetCaptureTimeUs()) +
          ",\"width\":" + std::to_string(frame->GetWidth()) +
          ",\"height\":" + std::to_string(frame->GetHeight()) +
          ",\"stride\":" + std::to_string(frame->GetStride()) +
          ",\"bytes\":" + std::to_string(frame->GetByteSize()) +
          ",\"checksum\":" + std::to_string(checksum) +
          ",\"source_frame\":" + std::to_string(first[0]) +
          ",\"left_frame\":" + std::to_string(left[0]) +
          ",\"right_frame\":" + std::to_string(right[0]) +
          ",\"split_pattern\":" + std::string(split ? "true" : "false") +
          ",\"pattern\":" + std::string(pattern ? "true" : "false"));
    std::unique_lock lock(mutex_);
    if (!frame->GetSurface()) latest_[frame->GetViewId()] = frame;
    if (retain_ && frame->GetDocumentId() && (pattern || split)) {
      presentation_.push_back({frame, checksum});
      recorder_.push_back({frame, checksum});
    }
    stall_cv_.wait(lock, [this] { return !stall_; });
  }
  bool Command(const std::string& line) {
    std::istringstream input(line);
    std::string operation;
    input >> operation;
    CefRefPtr<AlloyGuestView> view;
    CefRefPtr<AlloyGuestStream> stream;
    CefRefPtr<AlloyGuestSession> session;
    { std::lock_guard guard(mutex_); view = view_; stream = stream_; session = session_; }
    if (operation == "input") {
      uint64_t target = 0, document = 0, generation = 0, capture = 0;
      std::string source, kind;
      input >> target >> source >> document >> generation >> capture >> kind;
      {
        std::lock_guard guard(mutex_);
        if (!target && view_) target = view_->GetId();
        auto found = streams_.find(target); stream = found == streams_.end() ? nullptr : found->second;
      }
      uint64_t id = 0;
      if (stream && kind == "pointer") {
        std::string action; int x = 0, y = 0, button = 0, clicks = 0; uint64_t surface = 0; uint32_t modifiers = 0;
        input >> action >> x >> y >> surface >> button >> clicks >> modifiers;
        id = stream->SendPointer(source, document, generation, surface, x, y, action, button, clicks, modifiers, capture);
      } else if (stream && kind == "wheel") {
        int x = 0, y = 0, dx = 0, dy = 0; uint64_t surface = 0; uint32_t modifiers = 0;
        input >> x >> y >> surface >> dx >> dy >> modifiers;
        id = stream->SendWheel(source, document, generation, surface, x, y, dx, dy, modifiers, capture);
      } else if (stream && kind == "key") {
        CefKeyEvent event; int type = 0, character = 0, unmodified = 0, system = 0, editable = 0;
        input >> type >> event.windows_key_code >> event.native_key_code >> character >> unmodified >> event.modifiers >> system >> editable;
        event.type = static_cast<cef_key_event_type_t>(type); event.character = character;
        event.unmodified_character = unmodified; event.is_system_key = system; event.focus_on_editable_field = editable;
        id = stream->SendKey(source, document, generation, event, capture);
      } else if (stream && kind == "touch") {
        CefTouchEvent event; int type = 0, pointer = 0; uint64_t surface = 0;
        input >> event.id >> type >> event.x >> event.y >> surface >> event.pressure >> event.radius_x >> event.radius_y >> event.rotation_angle >> event.modifiers >> pointer;
        event.type = static_cast<cef_touch_event_type_t>(type); event.pointer_type = static_cast<cef_pointer_type_t>(pointer);
        id = stream->SendTouch(source, document, generation, surface, event, capture);
      } else if (stream && kind == "focus") {
        int focus = 0; input >> focus; id = stream->SendFocus(source, document, generation, focus != 0, capture);
      } else if (stream && kind == "capture-lost") {
        id = stream->SendCaptureLost(source, document, generation, capture);
      } else if (stream && kind == "ime-set") {
        std::string text; int from = 0, to = 0, replacement_from = -1, replacement_to = -1;
        input >> std::quoted(text) >> from >> to >> replacement_from >> replacement_to;
        CefString value(text); CefCompositionUnderline underline;
        underline.range = CefRange(0, value.length()); underline.color = 0xff202020;
        underline.background_color = 0; underline.thick = false; underline.style = CEF_CUS_SOLID;
        id = stream->SetComposition(source, document, generation, value, {underline},
            CefRange(replacement_from, replacement_to), CefRange(from, to), capture);
      } else if (stream && kind == "ime-commit") {
        std::string text; int relative = 0, from = -1, to = -1;
        input >> std::quoted(text) >> relative >> from >> to;
        id = stream->CommitText(source, document, generation, text, CefRange(from, to), relative, capture);
      } else if (stream && kind == "ime-finish") {
        int keep = 0; input >> keep; id = stream->FinishComposition(source, document, generation, keep != 0, capture);
      } else if (stream && kind == "ime-cancel") {
        id = stream->CancelComposition(source, document, generation, capture);
      }
      Event("queued-input", ",\"view\":" + std::to_string(target) + ",\"id\":" + std::to_string(id) +
            ",\"source\":" + Quote(source) + ",\"kind\":" + Quote(kind) +
            ",\"document\":" + std::to_string(document) + ",\"generation\":" + std::to_string(generation) +
            ",\"capture_us\":" + std::to_string(capture));
    } else if (operation == "window") {
      uint64_t id = 0; std::string decision; int width = 0, height = 0; double scale = 1;
      input >> id >> decision;
      CefRefPtr<AlloyGuestPopup> popup;
      { std::lock_guard guard(mutex_); if (auto found = windows_.find(id); found != windows_.end()) popup = found->second; }
      if (decision == "attach") {
        input >> width >> height >> scale;
        { std::lock_guard guard(mutex_); geometry_[id] = {width, height, scale}; }
        bool queued = popup && popup->Attach(width, height, scale, this);
        Event("window-attach-queued", ",\"id\":" + std::to_string(id) + ",\"ok\":" + (queued ? "true" : "false"));
      } else if (decision == "deny") {
        if (popup) popup->Deny();
        Event("window-deny-called", ",\"id\":" + std::to_string(id));
      }
    } else if (operation == "pixel") {
      uint64_t target = 0; int x = 0, y = 0; input >> target >> x >> y;
      CefRefPtr<AlloyGuestFrame> frame;
      {
        std::lock_guard guard(mutex_);
        if (!target && view_) target = view_->GetId();
        if (auto found = latest_.find(target); found != latest_.end()) frame = found->second;
      }
      uint8_t bytes[4] = {};
      bool read = frame && x >= 0 && y >= 0 && x < frame->GetWidth() && y < frame->GetHeight() &&
          frame->ReadPixels(bytes, 4, frame->GetStride() * y + 4 * x) == 4;
      Event("pixel", ",\"view\":" + std::to_string(target) + ",\"ok\":" + (read ? "true" : "false") +
            ",\"generation\":" + std::to_string(frame ? frame->GetGeneration() : 0) +
            ",\"sequence\":" + std::to_string(frame ? frame->GetSequence() : 0) +
            ",\"capture_us\":" + std::to_string(frame ? frame->GetCaptureTimeUs() : 0) +
            ",\"b\":" + std::to_string(bytes[0]) + ",\"g\":" + std::to_string(bytes[1]) +
            ",\"r\":" + std::to_string(bytes[2]) + ",\"a\":" + std::to_string(bytes[3]));
    } else if (operation == "close-view") {
      uint64_t target = 0; input >> target;
      { std::lock_guard guard(mutex_); if (auto found = views_.find(target); found != views_.end()) view = found->second; else view = nullptr; }
      if (view) view->Close();
    } else if (operation == "navigate-view") {
      uint64_t target = 0; std::string url; input >> target >> url;
      { std::lock_guard guard(mutex_); if (auto found = views_.find(target); found != views_.end()) view = found->second; else view = nullptr; }
      Event("navigate-queued", ",\"ok\":" + std::string(view && view->Navigate(url) ? "true" : "false"));
    } else if (operation == "navigate") {
      std::string url; input >> url;
      Event("navigate-queued", ",\"ok\":" + std::string(view && view->Navigate(url) ? "true" : "false"));
    } else if (operation == "configure") {
      int width = 0, height = 0; double scale = 0; input >> width >> height >> scale;
      Event("configure-queued", ",\"ok\":" + std::string(stream && stream->Configure(width, height, scale, 30) ? "true" : "false"));
    } else if (operation == "retain") {
      std::string value; input >> value;
      { std::lock_guard guard(mutex_); retain_ = value == "on"; }
      Event("retaining", ",\"on\":" + std::string(value == "on" ? "true" : "false"));
    } else if (operation == "release") {
      std::string consumer; input >> consumer;
      std::vector<Held> released;
      {
        std::lock_guard guard(mutex_);
        if (consumer == "presentation" || consumer == "all") released.swap(presentation_);
        if (consumer == "recorder" || consumer == "all") {
          released.insert(released.end(), recorder_.begin(), recorder_.end()); recorder_.clear();
        }
      }
      released.clear();
      Event("released", ",\"consumer\":" + Quote(consumer));
    } else if (operation == "held") {
      std::vector<Held> held;
      { std::lock_guard guard(mutex_); held = presentation_; held.insert(held.end(), recorder_.begin(), recorder_.end()); }
      for (const auto& entry : held)
        Event("held-frame", ",\"generation\":" + std::to_string(entry.frame->GetGeneration()) +
              ",\"sequence\":" + std::to_string(entry.frame->GetSequence()) +
              ",\"expected\":" + std::to_string(entry.checksum) +
              ",\"actual\":" + std::to_string(Checksum(entry.frame)));
      Event("held-complete", ",\"references\":" + std::to_string(held.size()));
    } else if (operation == "stall") {
      std::string value; input >> value;
      { std::lock_guard guard(mutex_); stall_ = value == "on"; stall_cv_.notify_all(); }
      Event("stalling", ",\"on\":" + std::string(value == "on" ? "true" : "false"));
    } else if (operation == "close") {
      if (session) session->Close();
    } else if (operation == "terminate") {
      if (session) session->Terminate();
    } else if (operation == "quit") {
      if (session) session->Terminate();
      std::unique_lock lock(mutex_);
      stall_ = false; stall_cv_.notify_all();
      closed_cv_.wait(lock, [this] { return closed_; });
      lock.unlock();
      Command("release all");
      Event("finished");
      return false;
    } else {
      Event("invalid-command", ",\"command\":" + Quote(line));
    }
    return true;
  }
 private:
  struct Geometry { int width; int height; double scale; };
  std::mutex mutex_;
  std::condition_variable closed_cv_, stall_cv_;
  bool closed_ = false, retain_ = false, stall_ = false;
  CefRefPtr<AlloyGuestSession> session_;
  CefRefPtr<AlloyGuestView> view_;
  CefRefPtr<AlloyGuestStream> stream_;
  std::vector<Held> presentation_, recorder_;
  std::map<uint64_t, CefRefPtr<AlloyGuestView>> views_;
  std::map<uint64_t, CefRefPtr<AlloyGuestStream>> streams_;
  std::map<uint64_t, CefRefPtr<AlloyGuestPopup>> windows_;
  std::map<uint64_t, CefRefPtr<AlloyGuestFrame>> latest_;
  std::map<uint64_t, Geometry> geometry_;
  IMPLEMENT_REFCOUNTING(Consumer);
};
}
int main() {
  // Select/verify the SDK version without CefInitialize or CefExecuteProcess.
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  if (!hash || strcmp(hash, CEF_API_HASH_PLATFORM)) return 1;
  CefRefPtr<Consumer> consumer = new Consumer;
  consumer->Start();
  std::string line;
  while (std::getline(std::cin, line)) if (!consumer->Command(line)) return 0;
  consumer->Command("quit");
  return 0;
}
