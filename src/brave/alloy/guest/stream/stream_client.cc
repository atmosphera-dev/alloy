// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/stream_client.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include "base/strings/string_number_conversions.h"
#include "brave/alloy/guest/stream/lease.h"
#include "brave/alloy/guest/stream/input.h"
#include "cef/include/alloy/alloy_guest_session.h"
#include <atomic>

namespace alloy::guest::stream {
namespace {
bool Older(const Owner& value, const Owner& current) {
  return value.document < current.document ||
      (value.document == current.document && value.generation < current.generation);
}
class Popup final : public AlloyGuestPopup {
 public:
  using Decision = std::function<bool(bool, int, int, double, CefRefPtr<AlloyGuestViewHandler>)>;
  Popup(uint64_t id, Decision decision) : id_(id), decision_(std::move(decision)) {}
  uint64_t GetId() override { return id_; }
  bool Attach(int width, int height, double scale, CefRefPtr<AlloyGuestViewHandler> handler) override {
    if (!handler || width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0 || decided_.exchange(true)) return false;
    return decision_(true, width, height, scale, std::move(handler));
  }
  void Deny() override { if (!decided_.exchange(true)) decision_(false, 0, 0, 1, nullptr); }
 private:
  const uint64_t id_;
  const Decision decision_;
  std::atomic<bool> decided_{false};
  IMPLEMENT_REFCOUNTING(Popup);
};
class State final : public ExtensionSink, public std::enable_shared_from_this<State> {
 public:
  State(ExtensionEndpoint endpoint, ViewAdoption adopt)
      : endpoint_(std::move(endpoint)), adopt_(std::move(adopt)) {}
  bool Start(CefRefPtr<AlloyGuestStreamHandler> handler, uint32_t slots, uint64_t budget) {
    if (!handler || !slots || !budget || slots > budget / 4) return false;
    {
      std::lock_guard lock(mutex_);
      if (started_ || retired_) return false;
      started_ = true;
      handler_ = std::move(handler); slots_ = slots; budget_ = budget;
    }
    if (!endpoint_.attach(Feature::kStream, shared_from_this())) {
      std::lock_guard lock(mutex_);
      retired_ = done_ = true;
      handler_ = nullptr;
      return false;
    }
    std::thread([self = shared_from_this()] { self->Run(); }).detach();
    std::string payload;
    PackFields({std::to_string(slots), std::to_string(budget)}, &payload);
    if (!Call("start", std::move(payload))) {
      Retire();
      return false;
    }
    return true;
  }
  bool Call(std::string operation, std::string payload) {
    {
      std::lock_guard lock(mutex_);
      if (!started_ || retired_) return false;
    }
    std::weak_ptr<State> weak = shared_from_this();
    return endpoint_.call(Feature::kStream, std::move(operation), std::move(payload),
        [weak](ProviderResult result, uint64_t, std::string) {
      if (result != ProviderResult::kOk && result != ProviderResult::kCancelled)
        if (auto self = weak.lock()) self->Unavailable();
    });
  }
  uint64_t Send(const CefString& source, uint64_t document, uint64_t generation,
                uint64_t surface, uint64_t capture, const std::string& kind,
                CefRefPtr<CefDictionaryValue> details) {
    if (source.empty() || !generation || surface > 1) return 0;
    const std::string json = WriteDetails(details);
    if (json.empty()) return 0;
    std::lock_guard lock(mutex_);
    if (!started_ || retired_) return 0;
    InteractionData input{0, document, generation, surface, capture, 0,
                           source.ToString(), kind, {}, json};
    Owner original = owner_;
    original.document = document;
    return endpoint_.interact(Feature::kStream, original, "input", std::move(input));
  }
  void Event(Owner owner, std::string operation, std::string payload) override {
    if (operation == "configured") {
      std::vector<std::string> fields;
      uint64_t generation = 0;
      int width = 0, height = 0;
      double scale = 0;
      if (!UnpackFields(payload, 4, &fields) ||
          !ParseUnsigned(fields[0], &generation) || !generation ||
          !base::StringToInt(fields[1], &width) || width <= 0 ||
          !base::StringToInt(fields[2], &height) || height <= 0 ||
          !base::StringToDouble(fields[3], &scale) || !std::isfinite(scale) || scale <= 0) {
        Unavailable();
        return;
      }
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_ || generation <= generation_) return;
      generation_ = generation;
      tasks_.push_back([this, generation, width, height, scale] {
        handler_->OnConfigured(generation, width, height, scale);
      });
      cv_.notify_one();
    } else if (operation == "window-open") {
      std::vector<std::string> outer, fields;
      uint64_t id = 0; int values[6] = {};
      if (!UnpackFields(payload, 2, &outer) || !ParseUnsigned(outer[0], &id) ||
          !UnpackFields(outer[1], 8, &fields)) { Unavailable(); return; }
      for (int i = 0; i < 6; ++i)
        if (!base::StringToInt(fields[i + 2], &values[i])) { Unavailable(); return; }
      CefRect rectangle(values[2], values[3], values[4], values[5]);
      std::weak_ptr<State> weak = shared_from_this();
      CefRefPtr<AlloyGuestPopup> popup = new Popup(id,
          [weak, id](bool allow, int width, int height, double scale, CefRefPtr<AlloyGuestViewHandler> handler) {
        auto self = weak.lock();
        return self && self->DecideWindow(id, allow, width, height, scale, std::move(handler));
      });
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_ || windows_.contains(id)) return;
      windows_.emplace(id, Window{owner, false});
      tasks_.push_back([this, id, popup, url = fields[0], name = fields[1], rectangle,
                        disposition = values[0], gesture = values[1] != 0] {
        {
          std::lock_guard guard(mutex_);
          auto found = windows_.find(id);
          if (retired_ || found == windows_.end()) return;
          found->second.announced = true;
        }
        handler_->OnWindowOpen(popup, url, name, disposition, gesture, rectangle);
      });
      cv_.notify_one();
    } else if (operation == kInteractionOperation) {
      InteractionData input;
      if (!UnpackInteraction(payload, &input) || input.outcome.empty()) { Unavailable(); return; }
      std::lock_guard lock(mutex_);
      if (retired_) return;
      QueueInteraction(owner, std::move(input));
    } else if (operation == "popup") {
      std::vector<std::string> fields;
      uint64_t generation = 0; int values[5] = {};
      if (!UnpackFields(payload, 7, &fields) || !ParseUnsigned(fields[0], &generation)) { Unavailable(); return; }
      for (int i = 0; i < 5; ++i)
        if (!base::StringToInt(fields[i + 1], &values[i])) { Unavailable(); return; }
      CefRect rectangle(values[1], values[2], values[3], values[4]);
      if ((values[0] != 0 && values[0] != 1) || rectangle.width < 0 || rectangle.height < 0 || fields[6] != "DIP") {
        Unavailable(); return;
      }
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_ || generation != generation_) return;
      tasks_.push_back([this, generation, rectangle, show = values[0] != 0] {
        handler_->OnPopup(show, rectangle, generation);
      });
      cv_.notify_one();
    } else if (operation == "cursor") {
      std::vector<std::string> fields;
      uint64_t generation = 0; int values[5] = {}; double scale = 0;
      if (!UnpackFields(payload, 8, &fields) || !ParseUnsigned(fields[0], &generation) ||
          !base::StringToDouble(fields[6], &scale) || !std::isfinite(scale) || scale <= 0) { Unavailable(); return; }
      for (int i = 0; i < 5; ++i)
        if (!base::StringToInt(fields[i + 1], &values[i])) { Unavailable(); return; }
      size_t stride = 0, bytes = 0;
      if (values[0] < 0 || values[0] >= CT_NUM_VALUES ||
          ((values[0] == CT_CUSTOM) != !fields[7].empty()) ||
          (!fields[7].empty() && (!FrameBytes(values[1], values[2], &stride, &bytes) ||
             bytes != fields[7].size() || values[3] < 0 || values[4] < 0 ||
             values[3] >= values[1] || values[4] >= values[2]))) { Unavailable(); return; }
      auto pixels = fields[7].empty() ? nullptr : CefBinaryValue::Create(fields[7].data(), fields[7].size());
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_ || generation != generation_) return;
      tasks_.push_back([this, generation, pixels, scale, type = values[0], width = values[1], height = values[2],
                        hx = values[3], hy = values[4]] {
        handler_->OnCursor(type, pixels, width, height, hx, hy, scale, generation);
      });
      cv_.notify_one();
    } else if (operation == "ime-bounds") {
      std::vector<std::string> fields;
      uint64_t generation = 0;
      if (!UnpackFields(payload, 2, &fields) || !ParseUnsigned(fields[0], &generation)) { Unavailable(); return; }
      auto data = ParseDetails(fields[1]);
      auto range = data ? data->GetList("selection") : nullptr;
      auto rectangles = data ? data->GetList("bounds") : nullptr;
      if (!range || range->GetSize() != 2 || !rectangles ||
          range->GetType(0) != VTYPE_INT || range->GetType(1) != VTYPE_INT ||
          !((range->GetInt(0) == -1 && range->GetInt(1) == -1) ||
            (range->GetInt(0) >= 0 && range->GetInt(1) >= 0))) { Unavailable(); return; }
      CefRange selection(range->GetInt(0), range->GetInt(1));
      std::vector<CefRect> bounds;
      for (size_t i = 0; i < rectangles->GetSize(); ++i) {
        auto rectangle = rectangles->GetList(i);
        if (!rectangle || rectangle->GetSize() != 4) { Unavailable(); return; }
        for (size_t j = 0; j < 4; ++j) if (rectangle->GetType(j) != VTYPE_INT) { Unavailable(); return; }
        if (rectangle->GetInt(2) < 0 || rectangle->GetInt(3) < 0) { Unavailable(); return; }
        bounds.emplace_back(rectangle->GetInt(0), rectangle->GetInt(1), rectangle->GetInt(2), rectangle->GetInt(3));
      }
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_ || generation != generation_) return;
      tasks_.push_back([this, selection, bounds, generation] { handler_->OnImeBounds(selection, bounds, generation); });
      cv_.notify_one();
    } else if (operation == "frame") {
      Accept(owner, payload, nullptr);
    } else {
      Unavailable();
    }
  }
  void Resource(stream::Resource resource) override {
    ExtensionRequest request;
    FrameInfo info;
    if (!UnpackExtension(resource.envelope.payload, &request) ||
        request.feature != Feature::kStream || request.operation != "frame" ||
        resource.handles.size() != 1 ||
        !UnpackFrame(resource.envelope.owner, request.payload, &info) || request.id != info.token) {
      Unavailable();
      return;
    }
    Accept(resource.envelope.owner, request.payload, &resource);
  }
  void OwnerChanged(Owner owner) override {
    std::vector<FrameInfo> release;
    {
      std::lock_guard lock(mutex_);
      if (Older(owner, owner_)) return;
      owner_ = owner;
      for (auto found = windows_.begin(); found != windows_.end();) {
        if (found->second.owner != owner) {
          if (found->second.announced) QueueWindowRetired(found->first, "owner-retired");
          found = windows_.erase(found);
        } else { ++found; }
      }
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (Older(it->second.info.owner, owner)) {
          release.push_back(it->second.info);
          it = pending_.erase(it);
        } else { ++it; }
      }
    }
    for (auto info : release) Released(info);
  }
  void ViewRetired(uint64_t view) override {
    std::lock_guard lock(mutex_);
    auto found = windows_.find(view);
    if (retired_ || found == windows_.end()) return;
    if (found->second.announced) QueueWindowRetired(view, "child-closed");
    windows_.erase(found);
  }
  void Retire() override {
    std::map<uint64_t, Pending> pending;
    {
      std::lock_guard lock(mutex_);
      if (retired_) return;
      retired_ = true;
      windows_.clear();
      pending.swap(pending_);
      cv_.notify_one();
    }
  }
  void Wait() override {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return done_; });
  }
 private:
  struct Window { Owner owner; bool announced; };
  bool DecideWindow(uint64_t id, bool allow, int width, int height, double scale,
                     CefRefPtr<AlloyGuestViewHandler> handler) {
    std::lock_guard lock(mutex_);
    auto found = windows_.find(id);
    if (retired_ || found == windows_.end() || found->second.owner != owner_) return false;
    if (allow && !adopt_(id, std::move(handler))) return false;
    std::string payload;
    if (!PackFields({std::to_string(id), allow ? "1" : "0", std::to_string(width),
                     std::to_string(height), base::NumberToString(scale)}, &payload)) return false;
    const Owner original = found->second.owner;
    std::weak_ptr<State> weak = shared_from_this();
    bool queued = endpoint_.owned_call(Feature::kStream, original, "window-decision", std::move(payload),
        [weak](ProviderResult result, uint64_t, std::string) {
      if (result == ProviderResult::kFailed || result == ProviderResult::kUnavailable)
        if (auto self = weak.lock()) self->Unavailable();
    });
    if (queued) windows_.erase(found);
    return queued;
  }
  void QueueWindowRetired(uint64_t id, std::string reason) {
    tasks_.push_back([this, id, reason = std::move(reason)] { handler_->OnWindowRetired(id, reason); });
    cv_.notify_one();
  }
  struct Pending {
    FrameInfo info;
    std::string description;
    bool announced = false;
    base::ScopedFD handle;
  };
  void QueueInteraction(Owner owner, InteractionData input) {
    auto receipt = MakeInteraction(owner, std::move(input));
    tasks_.push_back([this, receipt] { handler_->OnInteraction(receipt); });
    cv_.notify_one();
  }
  void Unavailable() {
    std::lock_guard lock(mutex_);
    if (retired_) return;
    tasks_.push_back([this] { handler_->OnUnavailable(); });
    cv_.notify_one();
  }
  void Released(FrameInfo info) {
    {
      std::lock_guard lock(mutex_);
      auto found = live_.find(info.token);
      if (found == live_.end()) return;
      bytes_ -= found->second;
      live_.erase(found);
      if (retired_) return;
    }
    // Release is a session-owned lease operation, not a stale document action.
    // The native pool separately checks the complete original owner and token.
    endpoint_.release(Feature::kStream, info.owner, std::to_string(info.token));
  }
  void Accept(Owner owner, const std::string& description, stream::Resource* resource) {
    FrameInfo info;
    if (!UnpackFrame(owner, description, &info)) { Unavailable(); return; }
    base::ScopedFD ready;
    bool stale = false;
    {
      std::lock_guard lock(mutex_);
      if (retired_) return;
      uint64_t& last = resource ? last_resource_ : last_announcement_;
      if (info.token <= last) return;
      last = info.token;
      auto found = pending_.find(info.token);
      if (found == pending_.end()) {
        if (live_.contains(info.token) || live_.size() >= slots_ ||
            info.bytes > budget_ - bytes_) return;
        live_.emplace(info.token, info.bytes);
        bytes_ += info.bytes;
        Pending pending;
        pending.info = info; pending.description = description;
        found = pending_.emplace(info.token, std::move(pending)).first;
      }
      auto& pending = found->second;
      if (pending.info.owner != owner || pending.description != description) return;
      if (resource) {
        if (pending.handle.is_valid()) return;
        pending.handle = std::move(resource->handles[0]);
      } else {
        if (pending.announced) return;
        pending.announced = true;
      }
      stale = Older(owner, owner_);
      if (!stale && (!pending.announced || !pending.handle.is_valid())) return;
      ready = std::move(pending.handle);
      pending_.erase(found);
    }
    if (stale) { Released(info); return; }
    std::weak_ptr<State> weak = shared_from_this();
    auto frame = ImportFrame(info, std::move(ready), [weak, info] {
      if (auto self = weak.lock()) self->Released(info);
    });
    if (!frame) { Unavailable(); return; }
    std::lock_guard lock(mutex_);
    if (retired_) return;
    tasks_.push_back([this, info, frame] {
      bool current;
      { std::lock_guard guard(mutex_); current = !retired_ && info.owner == owner_ &&
                                                    info.generation == generation_; }
      if (current) handler_->OnFrame(frame);
    });
    cv_.notify_one();
  }
  void Run() {
    for (;;) {
      std::function<void()> task;
      std::deque<std::function<void()>> abandoned;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return retired_ || !tasks_.empty(); });
        if (retired_) {
          abandoned.swap(tasks_);
        } else {
          task = std::move(tasks_.front()); tasks_.pop_front();
        }
      }
      if (!task) break;
      task();
    }
    handler_->OnClosed();
    handler_ = nullptr;
    std::lock_guard lock(mutex_);
    done_ = true;
    cv_.notify_all();
  }
  const ExtensionEndpoint endpoint_;
  const ViewAdoption adopt_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool started_ = false, retired_ = false, done_ = false;
  uint32_t slots_ = 0;
  uint64_t budget_ = 0, bytes_ = 0, generation_ = 0;
  uint64_t last_resource_ = 0, last_announcement_ = 0;
  Owner owner_;
  CefRefPtr<AlloyGuestStreamHandler> handler_;
  std::deque<std::function<void()>> tasks_;
  std::map<uint64_t, Pending> pending_;
  std::map<uint64_t, size_t> live_;
  std::map<uint64_t, Window> windows_;
};
class Client final : public AlloyGuestStream {
 public:
  Client(ExtensionEndpoint endpoint, ViewAdoption adopt)
      : state_(std::make_shared<State>(std::move(endpoint), std::move(adopt))) {}
  bool Start(CefRefPtr<AlloyGuestStreamHandler> handler, uint32_t slots, uint64_t budget) override {
    return state_->Start(std::move(handler), slots, budget);
  }
  bool Configure(int width, int height, double scale, int fps) override {
    if (width <= 0 || height <= 0 || !std::isfinite(scale) || scale <= 0 || fps < 1 || fps > 60)
      return false;
    std::string payload;
    if (!PackFields({std::to_string(width), std::to_string(height),
                     base::NumberToString(scale), std::to_string(fps)}, &payload)) return false;
    return state_->Call("configure", std::move(payload));
  }
  uint64_t SendPointer(const CefString& source, uint64_t document, uint64_t generation,
      uint64_t surface, int x, int y, const CefString& action, int button, int clicks,
      uint32_t modifiers, uint64_t capture) override {
    auto data = CefDictionaryValue::Create();
    data->SetInt("x", x); data->SetInt("y", y); data->SetString("action", action);
    data->SetInt("button", button); data->SetInt("clicks", clicks); data->SetDouble("modifiers", modifiers);
    return state_->Send(source, document, generation, surface, capture, "pointer", data);
  }
  uint64_t SendWheel(const CefString& source, uint64_t document, uint64_t generation,
      uint64_t surface, int x, int y, int dx, int dy, uint32_t modifiers, uint64_t capture) override {
    auto data = CefDictionaryValue::Create();
    data->SetInt("x", x); data->SetInt("y", y); data->SetInt("delta_x", dx);
    data->SetInt("delta_y", dy); data->SetDouble("modifiers", modifiers);
    return state_->Send(source, document, generation, surface, capture, "wheel", data);
  }
  uint64_t SendKey(const CefString& source, uint64_t document, uint64_t generation,
      const CefKeyEvent& event, uint64_t capture) override {
    auto data = CefDictionaryValue::Create();
    data->SetInt("type", event.type); data->SetInt("windows_key_code", event.windows_key_code);
    data->SetInt("native_key_code", event.native_key_code); data->SetDouble("modifiers", event.modifiers);
    data->SetInt("character", event.character); data->SetInt("unmodified_character", event.unmodified_character);
    data->SetBool("system", event.is_system_key); data->SetBool("editable", event.focus_on_editable_field);
    return state_->Send(source, document, generation, 0, capture, "key", data);
  }
  uint64_t SendTouch(const CefString& source, uint64_t document, uint64_t generation,
      uint64_t surface, const CefTouchEvent& event, uint64_t capture) override {
    auto data = CefDictionaryValue::Create();
    data->SetInt("id", event.id); data->SetInt("type", event.type); data->SetInt("pointer_type", event.pointer_type);
    data->SetDouble("x", event.x); data->SetDouble("y", event.y); data->SetDouble("radius_x", event.radius_x);
    data->SetDouble("radius_y", event.radius_y); data->SetDouble("rotation", event.rotation_angle);
    data->SetDouble("pressure", event.pressure); data->SetDouble("modifiers", event.modifiers);
    return state_->Send(source, document, generation, surface, capture, "touch", data);
  }
  uint64_t SendFocus(const CefString& source, uint64_t document, uint64_t generation,
      bool focused, uint64_t capture) override {
    auto data = CefDictionaryValue::Create(); data->SetBool("focused", focused);
    return state_->Send(source, document, generation, 0, capture, "focus", data);
  }
  uint64_t SendCaptureLost(const CefString& source, uint64_t document,
      uint64_t generation, uint64_t capture) override {
    return state_->Send(source, document, generation, 0, capture, "capture-lost", CefDictionaryValue::Create());
  }
  uint64_t SetComposition(const CefString& source, uint64_t document, uint64_t generation,
      const CefString& text, const std::vector<CefCompositionUnderline>& underlines,
      const CefRange& replacement, const CefRange& selection, uint64_t capture) override {
    if (!ValidUtf16(text)) return 0;
    auto data = CefDictionaryValue::Create(); data->SetString("text", text);
    data->SetList("replacement", RangeList(replacement)); data->SetList("selection", RangeList(selection));
    auto lines = CefListValue::Create();
    for (size_t i = 0; i < underlines.size(); ++i) {
      auto line = CefDictionaryValue::Create();
      line->SetList("range", RangeList(CefRange(underlines[i].range.from, underlines[i].range.to)));
      line->SetDouble("color", underlines[i].color); line->SetDouble("background", underlines[i].background_color);
      line->SetInt("style", underlines[i].style); line->SetBool("thick", underlines[i].thick);
      lines->SetDictionary(i, line);
    }
    data->SetList("underlines", lines);
    return state_->Send(source, document, generation, 0, capture, "ime-set", data);
  }
  uint64_t CommitText(const CefString& source, uint64_t document, uint64_t generation,
      const CefString& text, const CefRange& replacement, int relative, uint64_t capture) override {
    if (!ValidUtf16(text)) return 0;
    auto data = CefDictionaryValue::Create(); data->SetString("text", text);
    data->SetList("replacement", RangeList(replacement)); data->SetInt("relative_cursor", relative);
    return state_->Send(source, document, generation, 0, capture, "ime-commit", data);
  }
  uint64_t FinishComposition(const CefString& source, uint64_t document, uint64_t generation,
      bool keep, uint64_t capture) override {
    auto data = CefDictionaryValue::Create(); data->SetBool("keep_selection", keep);
    return state_->Send(source, document, generation, 0, capture, "ime-finish", data);
  }
  uint64_t CancelComposition(const CefString& source, uint64_t document, uint64_t generation,
      uint64_t capture) override {
    return state_->Send(source, document, generation, 0, capture, "ime-cancel", CefDictionaryValue::Create());
  }
 private:
  static CefRefPtr<CefListValue> RangeList(const CefRange& range) {
    auto result = CefListValue::Create();
    result->SetDouble(0, range.from == std::numeric_limits<uint32_t>::max() ? -1.0 : range.from);
    result->SetDouble(1, range.to == std::numeric_limits<uint32_t>::max() ? -1.0 : range.to);
    return result;
  }
  const std::shared_ptr<State> state_;
  IMPLEMENT_REFCOUNTING(Client);
};
}
CefRefPtr<AlloyGuestStream> MakeClient(ExtensionEndpoint endpoint, ViewAdoption adopt) {
  return new Client(std::move(endpoint), std::move(adopt));
}
}  // namespace alloy::guest::stream
