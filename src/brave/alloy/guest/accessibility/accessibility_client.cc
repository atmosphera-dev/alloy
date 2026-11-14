// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/accessibility/accessibility_client.h"
#include <condition_variable>
#include <optional>
#include <mutex>
#include <thread>
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "brave/alloy/guest/accessibility/model.h"
#include "brave/alloy/guest/accessibility/projection.h"
#include "brave/alloy/guest/accessibility/ax_actions.h"
#include "brave/alloy/guest/stream/input.h"
namespace alloy::guest::accessibility {
namespace {
class State final : public ExtensionSink, public std::enable_shared_from_this<State> {
 public:
  explicit State(ExtensionEndpoint endpoint) : endpoint_(std::move(endpoint)) {}
  bool Start(CefRefPtr<AlloyGuestAccessibilityHandler> handler) {
    {
      std::lock_guard lock(mutex_);
      if (!handler || started_ || retired_) return false;
      started_ = true; handler_ = std::move(handler);
    }
    if (!endpoint_.attach(Feature::kAccessibility, shared_from_this())) {
      std::lock_guard lock(mutex_);
      retired_ = done_ = true; handler_ = nullptr; return false;
    }
    std::thread([self = shared_from_this()] { self->Run(); }).detach();
    if (!Call("start")) { Retire(); return false; }
    return true;
  }
  bool Call(std::string operation) {
    {
      std::lock_guard lock(mutex_);
      if (!started_ || retired_) return false;
    }
    std::weak_ptr<State> weak = shared_from_this();
    return endpoint_.call(Feature::kAccessibility, std::move(operation), {},
        [weak](ProviderResult result, uint64_t, std::string) {
      if (result != ProviderResult::kOk && result != ProviderResult::kCancelled)
        if (auto state = weak.lock()) state->Unavailable();
    });
  }
  uint64_t Send(const CefString& source, uint64_t document, uint64_t geometry,
                const CefString& tree, uint64_t tree_generation, int node,
                uint64_t node_generation, const CefString& action,
                const CefString& value, int anchor, int focus, uint64_t capture) {
    const std::string kind = action.ToString();
    if (source.empty() || !geometry || tree.empty() || !tree_generation || node == 0 || !node_generation ||
        !IsActionKind(kind) ||
        !stream::ValidUtf16(value) || (kind == "selection" && (anchor < 0 || focus < 0))) return 0;
    auto details = CefDictionaryValue::Create();
    details->SetString("tree", tree); details->SetString("tree_generation", std::to_string(tree_generation));
    details->SetInt("node", node); details->SetString("node_generation", std::to_string(node_generation));
    details->SetString("action", action); details->SetString("value", value);
    details->SetInt("anchor", anchor); details->SetInt("focus", focus);
    stream::InteractionData input{0, document, geometry, 0, capture, 0,
        source.ToString(), "ax", {}, stream::WriteDetails(details)};
    std::lock_guard lock(mutex_);
    if (!started_ || retired_ || !projection_.ready()) return 0;
    Owner original = owner_; original.document = document;
    return endpoint_.interact(Feature::kAccessibility, original, "action", std::move(input));
  }
  void Event(Owner owner, std::string operation, std::string payload) override {
    if (operation == "unavailable") { Unavailable(); return; }
    if (operation != "update-part") { Unavailable(); return; }
    std::vector<std::string> fields;
    uint64_t sequence = 0, total = 0, offset = 0;
    bool invalid = !UnpackFields(payload, 4, &fields) ||
        !ParseUnsigned(fields[0], &sequence) || !sequence ||
        !ParseUnsigned(fields[1], &total) || !total ||
        !ParseUnsigned(fields[2], &offset);
    UpdateParts::Result assembled = UpdateParts::Result::kResync;
    {
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_) return;
      if (!invalid) assembled = parts_.Add(sequence, total, offset, fields[3], &payload);
      if (assembled == UpdateParts::Result::kResync) { parts_.Clear(); }
    }
    if (assembled == UpdateParts::Result::kPending) return;
    if (assembled == UpdateParts::Result::kResync) { RequestResync(); return; }
    ReceiveUpdate(owner, std::move(payload), sequence);
  }
  void ReceiveUpdate(Owner owner, std::string payload, uint64_t sequence) {
    auto data = base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
    uint64_t declared_sequence = 0;
    const auto* declared = data ? data->FindString("sequence") : nullptr;
    if (!declared || !ParseUnsigned(*declared, &declared_sequence) || sequence != declared_sequence) {
      RequestResync(); return;
    }
    Projection::Result result;
    {
      std::lock_guard lock(mutex_);
      if (retired_ || owner != owner_) return;
      result = projection_.Receive(*data);
      cv_.notify_one();
    }
    if (result == Projection::Result::kResync && !Call("resync")) Unavailable();
  }
  void Resource(stream::Resource) override { Unavailable(); }
  void OwnerChanged(Owner owner) override {
    std::lock_guard lock(mutex_);
    owner_ = owner; projection_.Clear(); parts_.Clear();
  }
  void ViewRetired(uint64_t) override {}
  void Retire() override {
    std::lock_guard lock(mutex_);
    retired_ = true; projection_.Clear(); parts_.Clear(); cv_.notify_all();
  }
  void Wait() override {
    std::unique_lock lock(mutex_); cv_.wait(lock, [this] { return done_; });
  }
 private:
  void RequestResync() {
    {
      std::lock_guard lock(mutex_);
      if (retired_ || !projection_.RequestResync()) return;
    }
    if (!Call("resync")) Unavailable();
  }
  void Unavailable() {
    std::lock_guard lock(mutex_);
    if (!retired_) { projection_.Fail(); parts_.Clear(); cv_.notify_one(); }
  }
  void Run() {
    for (;;) {
      std::optional<base::DictValue> delivery;
      Owner owner;
      bool unavailable = false;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return retired_ || projection_.pending() || projection_.unavailable(); });
        if (retired_) break;
        owner = owner_;
        unavailable = projection_.TakeUnavailable();
        if (!unavailable) delivery = projection_.Take();
      }
      if (unavailable) handler_->OnUnavailable();
      else if (delivery) {
        auto json = base::WriteJson(*delivery);
        auto copied = json ? stream::ParseDetails(*json) : nullptr;
        { std::lock_guard lock(mutex_); if (retired_ || owner != owner_) continue; }
        if (copied) handler_->OnUpdate(copied);
        else Unavailable();
      }
    }
    handler_->OnClosed(); handler_ = nullptr;
    std::lock_guard lock(mutex_); done_ = true; cv_.notify_all();
  }
  const ExtensionEndpoint endpoint_;
  std::mutex mutex_;
  std::condition_variable cv_;
  Projection projection_;
  UpdateParts parts_;
  Owner owner_;
  bool started_ = false, retired_ = false, done_ = false;
  CefRefPtr<AlloyGuestAccessibilityHandler> handler_;
};
class Client final : public AlloyGuestAccessibility {
 public:
  explicit Client(ExtensionEndpoint endpoint) : state_(std::make_shared<State>(std::move(endpoint))) {}
  bool Start(CefRefPtr<AlloyGuestAccessibilityHandler> handler) override { return state_->Start(std::move(handler)); }
  bool Resync() override { return state_->Call("resync"); }
  uint64_t SendAction(const CefString& source, uint64_t document, uint64_t geometry,
      const CefString& tree, uint64_t tree_generation, int node, uint64_t node_generation,
      const CefString& action, const CefString& value, int anchor, int focus, uint64_t capture) override {
    return state_->Send(source, document, geometry, tree, tree_generation, node,
                        node_generation, action, value, anchor, focus, capture);
  }
 private:
  const std::shared_ptr<State> state_;
  IMPLEMENT_REFCOUNTING(Client);
};
}
CefRefPtr<AlloyGuestAccessibility> MakeClient(ExtensionEndpoint endpoint) {
  return new Client(std::move(endpoint));
}
}  // namespace alloy::guest::accessibility
