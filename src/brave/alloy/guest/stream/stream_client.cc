// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/stream/stream_client.h"

#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include "base/strings/string_number_conversions.h"
#include "brave/alloy/guest/stream/lease.h"

namespace alloy::guest::stream {
namespace {
bool Older(const Owner& value, const Owner& current) {
  return value.document < current.document ||
      (value.document == current.document && value.generation < current.generation);
}
class State final : public ExtensionSink, public std::enable_shared_from_this<State> {
 public:
  explicit State(ExtensionEndpoint endpoint) : endpoint_(std::move(endpoint)) {}
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
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (Older(it->second.info.owner, owner)) {
          release.push_back(it->second.info);
          it = pending_.erase(it);
        } else { ++it; }
      }
    }
    for (auto info : release) Released(info);
  }
  void Retire() override {
    std::map<uint64_t, Pending> pending;
    {
      std::lock_guard lock(mutex_);
      if (retired_) return;
      retired_ = true;
      pending.swap(pending_);
      cv_.notify_one();
    }
  }
  void Wait() override {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return done_; });
  }
 private:
  struct Pending {
    FrameInfo info;
    std::string description;
    bool announced = false;
    base::ScopedFD handle;
  };
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
};
class Client final : public AlloyGuestStream {
 public:
  explicit Client(ExtensionEndpoint endpoint) : state_(std::make_shared<State>(std::move(endpoint))) {}
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
 private:
  const std::shared_ptr<State> state_;
  IMPLEMENT_REFCOUNTING(Client);
};
}
CefRefPtr<AlloyGuestStream> MakeClient(ExtensionEndpoint endpoint) {
  return new Client(std::move(endpoint));
}
}  // namespace alloy::guest::stream
