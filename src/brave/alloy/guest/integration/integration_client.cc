// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/integration_client.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "brave/alloy/guest/stream/input.h"
#include "base/unguessable_token.h"
#include "brave/alloy/guest/integration/clipboard.h"

namespace alloy::guest::integration {
namespace {
// Chunk size is derived from the existing message envelope, not a content-size
// quota. One acknowledged chunk at a time gives the integration lane credit.
constexpr size_t kChunk = kMaxControlPayload / 2;
using Post = std::function<void(std::function<void()>)>;
struct Read;
struct Request : public std::enable_shared_from_this<Request> {
  Owner owner;
  uint64_t id = 0;
  std::string kind;
  CefRefPtr<CefDictionaryValue> details;
  std::vector<uint64_t> sizes;
  ExtensionEndpoint endpoint;
  Post post;
  std::atomic<bool> active{true}, replied{false}, announced{false};
  std::mutex mutex;
  std::vector<std::weak_ptr<Read>> reads;
  std::function<bool(std::shared_ptr<Read>)> track_read;
  std::function<void(Read*)> retire_read;
  bool Call(std::string operation, std::string payload, ExtensionCompletion reply) {
    return active && endpoint.owned_call(Feature::kIntegration, owner,
        std::move(operation), std::move(payload), std::move(reply));
  }
  void End();
  bool Respond(std::string metadata, std::vector<std::string> bytes,
                 std::string source = "app-integration", uint64_t capture = 0);
  bool IsUiReply() const {
    return announced && (kind == "clipboard-write" || kind == "menu" ||
                         kind == "drag-source" || kind == "fullscreen");
  }
  std::optional<stream::InteractionData> UiReply(bool allow, std::string metadata,
                                                 std::string source, uint64_t capture) const {
    uint64_t generation = 0;
    if (details->GetType("generation") != VTYPE_STRING ||
        !ParseUnsigned(details->GetString("generation").ToString(), &generation) ||
        !generation || source.empty()) return std::nullopt;
    auto decision = CefDictionaryValue::Create();
    decision->SetString("request", std::to_string(id));
    decision->SetBool("allow", allow);
    decision->SetString("reply", metadata);
    stream::InteractionData action{0, owner.document, generation, 0, capture, 0,
        std::move(source), kind == "clipboard-write" ? "clipboard-write-outcome"
                                                     : kind + "-decision",
        {}, stream::WriteDetails(decision)};
    auto maximum_id = action;
    maximum_id.id = std::numeric_limits<uint64_t>::max();
    std::string payload, envelope;
    if (!stream::FitsInteraction(maximum_id) || !stream::PackInteraction(maximum_id, &payload) ||
        !PackExtension({Feature::kIntegration, std::numeric_limits<uint64_t>::max(),
                        "complete-ui", std::move(payload)}, &envelope)) return std::nullopt;
    return action;
  }
  bool FinishReply(bool allow, std::string metadata,
                       std::string source, uint64_t capture) {
    if (IsUiReply()) {
      auto action = UiReply(allow, std::move(metadata), std::move(source), capture);
      return action && active && endpoint.interact(Feature::kIntegration, owner,
          "complete-ui", std::move(*action)) != 0;
    }
    std::string payload;
    return PackFields({std::to_string(id), allow ? "1" : "0", std::move(metadata)}, &payload) &&
        Call("complete", std::move(payload), [](ProviderResult, uint64_t, std::string) {});
  }
  void Deny() {
    if (!active || replied.exchange(true)) return;
    FinishReply(false, "{}", "app-integration", 0);
  }
  void Abandon() {
    if (!active || replied.exchange(true)) return;
    std::string payload;
    if (PackFields({std::to_string(id), "0", "{}"}, &payload))
      Call("complete", std::move(payload), [](ProviderResult, uint64_t, std::string) {});
    End();
  }
};

struct Read : public std::enable_shared_from_this<Read> {
  std::shared_ptr<Request> request;
  uint64_t index = 0, offset = 0, length = 0;
  std::string bytes;
  CefRefPtr<AlloyGuestDataHandler> handler;
  std::mutex mutex;
  bool done = false;
  bool requested = false;
  void Finish(bool success) {
    CefRefPtr<AlloyGuestDataHandler> complete;
    CefRefPtr<CefBinaryValue> result;
    {
      std::lock_guard lock(mutex);
      if (done) return;
      done = true;
      complete = std::move(handler);
      if (success && !bytes.empty()) result = CefBinaryValue::Create(bytes.data(), bytes.size());
      bytes.clear();
    }
    request->post([self = shared_from_this(), complete, result, success, offset = offset] {
      const bool admitted = success && self->request->active;
      complete->OnRead(admitted, offset, admitted ? result : nullptr);
      self->request->retire_read(self.get());
    });
  }
  void Next() {
    uint64_t received = 0, count = 0;
    bool empty_request = false;
    {
      std::lock_guard lock(mutex);
      if (done) return;
      received = bytes.size();
      count = std::min<uint64_t>(length - received, kChunk);
      empty_request = !length && !requested;
      requested = true;
    }
    if (!request->active) { Finish(false); return; }
    if (!count && !empty_request) { Finish(true); return; }
    std::string payload;
    PackFields({std::to_string(request->id), std::to_string(index),
                  std::to_string(offset + received), std::to_string(count)}, &payload);
    if (!request->Call("read", std::move(payload),
        [self = shared_from_this(), count](ProviderResult result, uint64_t,
                                           std::string bytes) {
          if (result != ProviderResult::kOk || bytes.size() != count ||
              !self->request->active) { self->Finish(false); return; }
          {
            std::lock_guard lock(self->mutex);
            if (self->done) return;
            self->bytes.append(bytes);
          }
          self->Next();
        })) Finish(false);
  }
};

void Request::End() {
  active = false;
  std::vector<std::weak_ptr<Read>> cancelled;
  { std::lock_guard lock(mutex); cancelled.swap(reads); }
  for (const auto& weak : cancelled)
    if (auto read = weak.lock()) read->Finish(false);
}

struct Upload : public std::enable_shared_from_this<Upload> {
  std::shared_ptr<Request> request;
  std::string metadata;
  std::vector<std::string> bytes;
  size_t index = 0, offset = 0;
  std::string source;
  uint64_t capture = 0;
  void Failed() {
    request->FinishReply(false, "{}", source, capture);
  }
  void Next() {
    if (!request->active) return;
    while (index < bytes.size() && offset == bytes[index].size()) { ++index; offset = 0; }
    std::string payload;
    if (index == bytes.size()) {
      if (!request->FinishReply(true, metadata, source, capture)) Failed();
      return;
    }
    const size_t count = std::min(kChunk, bytes[index].size() - offset);
    PackFields({std::to_string(request->id), std::to_string(index),
                  std::to_string(offset), bytes[index].substr(offset, count)}, &payload);
    if (!request->Call("append", std::move(payload),
        [self = shared_from_this(), count](ProviderResult result, uint64_t, std::string) {
          if (result != ProviderResult::kOk) { self->Failed(); return; }
          self->offset += count;
          self->Next();
        })) Failed();
  }
};

bool Request::Respond(std::string metadata, std::vector<std::string> bytes,
                        std::string source, uint64_t capture) {
  if (!active || metadata.empty()) return false;
  // Validate the final action before consuming the single-use reply or sending
  // its upload preamble. An unqueueable action must leave the request usable
  // for a corrected reply or cancellation, not strand native UI indefinitely.
  if (IsUiReply() && !UiReply(true, metadata, source, capture)) return false;
  auto upload = std::make_shared<Upload>();
  upload->request = shared_from_this();
  upload->metadata = std::move(metadata);
  upload->bytes = std::move(bytes);
  upload->source = std::move(source);
  upload->capture = capture;
  std::vector<std::string> extents;
  for (const auto& blob : upload->bytes) extents.push_back(std::to_string(blob.size()));
  std::string lengths, payload;
  if (!PackFields(extents, &lengths) || !PackFields(
       {std::to_string(id), std::to_string(extents.size()), lengths}, &payload)) return false;
  if (!active || replied.exchange(true)) return false;
  const bool queued = Call("respond", std::move(payload),
       [upload](ProviderResult result, uint64_t, std::string) {
         if (result != ProviderResult::kOk) { upload->Failed(); return; }
         upload->Next();
       });
  if (!queued) replied = false;
  return queued;
}

class Data final : public AlloyGuestData {
 public:
  Data(std::shared_ptr<Request> request, size_t index)
      : request_(std::move(request)), index_(index) {}
  uint64_t GetSize() override { return request_->sizes[index_]; }
  bool Read(uint64_t offset, uint64_t size,
              CefRefPtr<AlloyGuestDataHandler> handler) override {
    if (!handler || !request_->active || offset > GetSize() ||
        size > GetSize() - offset || size > std::string().max_size()) return false;
    auto read = std::make_shared<struct Read>();
    read->request = request_;
    read->index = index_; read->offset = offset; read->length = size;
    read->handler = std::move(handler);
    if (!request_->track_read(read)) return false;
    bool cancelled = false;
    {
      std::lock_guard lock(request_->mutex);
      cancelled = !request_->active;
      if (!cancelled) {
        std::erase_if(request_->reads, [](const auto& value) { return value.expired(); });
        request_->reads.push_back(read);
      }
    }
    if (cancelled) read->Finish(false); else read->Next();
    return true;
  }
 private:
  const std::shared_ptr<Request> request_;
  const size_t index_;
  IMPLEMENT_REFCOUNTING(Data);
};

std::string String(CefRefPtr<CefDictionaryValue> value, const char* key) {
  return value && value->GetType(key) == VTYPE_STRING ? value->GetString(key).ToString()
                                                     : std::string();
}
class ClipboardRequest final : public AlloyGuestClipboardRequest {
 public:
  explicit ClipboardRequest(std::shared_ptr<Request> request) : request_(std::move(request)) {}
  ~ClipboardRequest() override { request_->Abandon(); }
  uint64_t GetId() override { return request_->id; }
  uint64_t GetDocumentId() override { return request_->owner.document; }
  CefString GetRequestingOrigin() override { return String(request_->details, "requesting-origin"); }
  CefString GetEmbeddingOrigin() override { return String(request_->details, "embedding-origin"); }
  bool IsRead() override { return request_->kind == "clipboard-read"; }
  bool IsWrite() override { return request_->kind == "clipboard-write"; }
  bool IsSequenceQuery() override { return request_->kind == "clipboard-sequence"; }
  bool IsPermissionQuery() override { return request_->kind == "clipboard-permission"; }
  bool NeedsReadPermission() override {
    return request_->details->GetType("read") == VTYPE_BOOL && request_->details->GetBool("read");
  }
  bool HasUserGesture() override {
    return request_->details->GetType("gesture") == VTYPE_BOOL && request_->details->GetBool("gesture");
  }
  int GetBuffer() override { return String(request_->details, "buffer") == "selection" ? 1 : 0; }
  CefString GetRequestedFormat() override { return String(request_->details, "format"); }
  size_t GetFormatCount() override { return IsWrite() ? request_->sizes.size() : 0; }
  CefString GetFormatName(size_t index) override {
    auto formats = request_->details->GetList("formats");
    return formats && index < formats->GetSize() ? formats->GetString(index) : CefString();
  }
  CefRefPtr<AlloyGuestData> GetData(size_t index) override {
    return index < GetFormatCount() ? CefRefPtr<AlloyGuestData>(new Data(request_, index)) : nullptr;
  }
  CefString GetSourceUrl() override { return String(request_->details, "source-url"); }
  int GetPrivacyFlags() override {
    return request_->details->GetType("privacy") == VTYPE_INT ? request_->details->GetInt("privacy") : 0;
  }
  bool CompleteTypes(const std::vector<CefString>& formats) override {
    if (!IsRead() || GetRequestedFormat() != "@types") return false;
    auto details = CefDictionaryValue::Create();
    auto types = CefListValue::Create();
    types->SetSize(formats.size());
    for (size_t i = 0; i < formats.size(); ++i) types->SetString(i, formats[i]);
    details->SetList("formats", types);
    return request_->Respond(stream::WriteDetails(details), {});
  }
  bool CompleteRead(CefRefPtr<CefBinaryValue> data, const CefString& source,
                      uint32_t start, uint32_t end) override {
    if (!IsRead() || GetRequestedFormat() == "@types" || start > end) return false;
    std::string bytes(data ? data->GetSize() : 0, '\0');
    if (data && data->GetData(bytes.data(), bytes.size(), 0) != bytes.size()) return false;
    auto details = CefDictionaryValue::Create();
    details->SetString("source-url", source);
    details->SetString("fragment-start", std::to_string(start));
    details->SetString("fragment-end", std::to_string(end));
    return request_->Respond(stream::WriteDetails(details), {std::move(bytes)});
  }
  bool CompleteWrite(const CefString& source, uint64_t capture) override {
    return IsWrite() && !source.empty() && request_->Respond("{}", {}, source.ToString(), capture);
  }
  bool CompleteSequence(const CefString& sequence) override {
    if (!IsSequenceQuery() ||
        !base::UnguessableToken::DeserializeFromString(sequence.ToString())) return false;
    auto details = CefDictionaryValue::Create();
    details->SetString("sequence", sequence);
    return request_->Respond(stream::WriteDetails(details), {});
  }
  bool CompletePermission(bool allow) override {
    if (!IsPermissionQuery()) return false;
    auto details = CefDictionaryValue::Create();
    details->SetBool("allow", allow);
    return request_->Respond(stream::WriteDetails(details), {});
  }
  void Deny() override { request_->Deny(); }
 private:
  const std::shared_ptr<Request> request_;
  IMPLEMENT_REFCOUNTING(ClipboardRequest);
};

uint64_t Generation(const std::shared_ptr<Request>& request) {
  uint64_t value = 0;
  ParseUnsigned(String(request->details, "generation"), &value);
  return value;
}
int Number(const std::shared_ptr<Request>& request, const char* name) {
  return request->details->GetType(name) == VTYPE_INT ? request->details->GetInt(name) : 0;
}
class MenuRequest final : public AlloyGuestMenuRequest {
 public:
  explicit MenuRequest(std::shared_ptr<Request> request) : request_(std::move(request)) {}
  ~MenuRequest() override { request_->Abandon(); }
  uint64_t GetId() override { return request_->id; }
  uint64_t GetDocumentId() override { return request_->owner.document; }
  uint64_t GetGeneration() override { return Generation(request_); }
  int GetX() override { return Number(request_, "x"); }
  int GetY() override { return Number(request_, "y"); }
  bool IsQuickMenu() override { return request_->details->GetBool("quick"); }
  CefRefPtr<CefListValue> GetItems() override {
    auto items = request_->details->GetList("items");
    return items ? items->Copy() : nullptr;
  }
  bool Select(int command, int flags, bool handled, const CefString& source, uint64_t capture) override {
    if (source.empty() || flags < 0) return false;
    auto reply = CefDictionaryValue::Create();
    reply->SetInt("command", command); reply->SetInt("flags", flags);
    reply->SetBool("handled", handled);
    return request_->Respond(stream::WriteDetails(reply), {}, source.ToString(), capture);
  }
  bool Cancel(const CefString& source, uint64_t capture) override { return Select(-1, 0, false, source, capture); }
 private:
  const std::shared_ptr<Request> request_;
  IMPLEMENT_REFCOUNTING(MenuRequest);
};
class FullscreenRequest final : public AlloyGuestFullscreenRequest {
 public:
  explicit FullscreenRequest(std::shared_ptr<Request> request) : request_(std::move(request)) {}
  ~FullscreenRequest() override { request_->Abandon(); }
  uint64_t GetId() override { return request_->id; }
  uint64_t GetDocumentId() override { return request_->owner.document; }
  uint64_t GetGeneration() override { return Generation(request_); }
  CefString GetRequestingOrigin() override { return String(request_->details, "requesting-origin"); }
  bool Decide(bool allow, const CefString& source, uint64_t capture) override {
    if (source.empty()) return false;
    auto reply = CefDictionaryValue::Create(); reply->SetBool("allow", allow);
    return request_->Respond(stream::WriteDetails(reply), {}, source.ToString(), capture);
  }
 private:
  const std::shared_ptr<Request> request_;
  IMPLEMENT_REFCOUNTING(FullscreenRequest);
};
class DragRequest final : public AlloyGuestDragRequest {
 public:
  explicit DragRequest(std::shared_ptr<Request> request) : request_(std::move(request)) {}
  ~DragRequest() override { request_->Abandon(); }
  uint64_t GetId() override { return request_->id; }
  uint64_t GetDocumentId() override { return request_->owner.document; }
  uint64_t GetGeneration() override { return Generation(request_); }
  int GetAllowedOperations() override { return Number(request_, "allowed"); }
  int GetStartX() override { return Number(request_, "x"); }
  int GetStartY() override { return Number(request_, "y"); }
  CefString GetText() override { return String(request_->details, "text"); }
  CefString GetHtml() override { return String(request_->details, "html"); }
  CefString GetBaseUrl() override { return String(request_->details, "base"); }
  CefString GetLinkUrl() override { return String(request_->details, "link"); }
  CefString GetLinkTitle() override { return String(request_->details, "title"); }
  CefString GetFileName() override { return String(request_->details, "file-name"); }
  CefRefPtr<AlloyGuestData> GetFileData() override { return new Data(request_, 0); }
  CefRefPtr<AlloyGuestData> GetImagePng() override { return new Data(request_, 1); }
  int GetHotspotX() override { return Number(request_, "hotspot-x"); }
  int GetHotspotY() override { return Number(request_, "hotspot-y"); }
  bool Finish(int x, int y, int operation, const CefString& source, uint64_t capture) override {
    if (source.empty()) return false;
    auto reply = CefDictionaryValue::Create();
    reply->SetInt("x", x); reply->SetInt("y", y); reply->SetInt("operation", operation);
    return request_->Respond(stream::WriteDetails(reply), {}, source.ToString(), capture);
  }
 private:
  const std::shared_ptr<Request> request_;
  IMPLEMENT_REFCOUNTING(DragRequest);
};

class State final : public ExtensionSink, public std::enable_shared_from_this<State> {
 public:
  explicit State(ExtensionEndpoint endpoint) : endpoint_(std::move(endpoint)) {}
  bool Start(CefRefPtr<AlloyGuestIntegrationHandler> handler) {
    if (!handler) return false;
    {
      std::lock_guard lock(mutex_);
      if (started_ || retiring_) return false;
      started_ = true;
      handler_ = std::move(handler);
    }
    if (!endpoint_.attach(Feature::kIntegration, shared_from_this())) {
      std::lock_guard lock(mutex_);
      retiring_ = retired_ = done_ = true;
      handler_ = nullptr;
      return false;
    }
    std::thread([self = shared_from_this()] { self->Run(); }).detach();
    if (Call("start")) return true;
    Retire();
    return false;
  }
  bool Call(std::string operation) {
    { std::lock_guard lock(mutex_); if (!started_ || retiring_) return false; }
    return endpoint_.call(Feature::kIntegration, std::move(operation), {},
        [weak = weak_from_this()](ProviderResult result, uint64_t, std::string) {
          if (result != ProviderResult::kOk && result != ProviderResult::kCancelled)
            if (auto self = weak.lock()) self->Unavailable();
        });
  }
  uint64_t Send(uint64_t document, uint64_t generation, const CefString& source,
      uint64_t capture, std::string kind, CefRefPtr<CefDictionaryValue> details) {
    if (!document || !generation || source.empty() || !details) return 0;
    const auto json = stream::WriteDetails(details);
    if (json.empty()) return 0;
    std::lock_guard lock(mutex_);
    if (!started_ || retiring_) return 0;
    Owner original = owner_; original.document = document;
    stream::InteractionData action{0, document, generation, 0, capture, 0,
                                    source.ToString(), std::move(kind), {}, json};
    return endpoint_.interact(Feature::kIntegration, original, "ui", std::move(action));
  }
  void PostTask(std::function<void()> task) {
    std::lock_guard lock(mutex_);
    if (finishing_ || done_) return;
    tasks_.push_back(std::move(task));
    cv_.notify_one();
  }
  bool TrackRead(std::shared_ptr<Read> read) {
    std::lock_guard lock(mutex_);
    if (retiring_ || !read->request->active) return false;
    reads_.emplace(read.get(), std::move(read));
    return true;
  }
  void RetireRead(Read* read) {
    std::lock_guard lock(mutex_);
    reads_.erase(read); cv_.notify_all();
  }
  void Event(Owner owner, std::string operation, std::string payload) override {
    if (operation == "fullscreen-changed" || operation == "drag-cursor") {
      auto details = stream::ParseDetails(payload);
      uint64_t generation = 0;
      if (!details || !ParseUnsigned(String(details, "generation"), &generation) || !generation ||
          (operation == "fullscreen-changed" && details->GetType("fullscreen") != VTYPE_BOOL) ||
          (operation == "drag-cursor" && details->GetType("operation") != VTYPE_INT)) {
        Unavailable(); return;
      }
      std::lock_guard lock(mutex_);
      if (retiring_ || owner != owner_) return;
      tasks_.push_back([this, owner, details, generation, operation] {
        { std::lock_guard lock(mutex_); if (retiring_ || owner != owner_) return; }
        if (operation == "fullscreen-changed")
          handler_->OnFullscreenChanged(owner.document, generation, details->GetBool("fullscreen"));
        else handler_->OnDragOperationChanged(details->GetInt("operation"), generation);
      });
      cv_.notify_one();
      return;
    }
    std::vector<std::string> fields;
    uint64_t id = 0, count = 0;
    if (operation == "request-ended") {
      if (!UnpackFields(payload, 2, &fields) || !ParseUnsigned(fields[0], &id) ||
          (fields[1] != "completed" && fields[1] != "retired")) { Unavailable(); return; }
      EndRequest(owner, id, fields[1]);
      return;
    }
    if (operation != "request" || !UnpackFields(payload, 5, &fields) ||
        !ParseUnsigned(fields[0], &id) || !id || !ParseUnsigned(fields[3], &count) ||
        count > fields[4].size() / 4) { Unavailable(); return; }
    auto request = std::make_shared<Request>();
    request->owner = owner; request->id = id; request->kind = fields[1];
    request->details = stream::ParseDetails(fields[2]);
    request->endpoint = endpoint_;
    request->post = [weak = weak_from_this()](std::function<void()> task) {
      if (auto self = weak.lock()) self->PostTask(std::move(task));
    };
    request->track_read = [weak = weak_from_this()](std::shared_ptr<Read> read) {
      auto self = weak.lock(); return self && self->TrackRead(std::move(read));
    };
    request->retire_read = [weak = weak_from_this()](Read* read) {
      if (auto self = weak.lock()) self->RetireRead(read);
    };
    std::vector<std::string> lengths;
    if (!request->details || !UnpackFields(fields[4], static_cast<size_t>(count), &lengths)) {
      Unavailable(); return;
    }
    for (const auto& size : lengths) {
      uint64_t value = 0;
      if (!ParseUnsigned(size, &value)) { Unavailable(); return; }
      request->sizes.push_back(value);
    }
    if (request->kind != "clipboard-read" && request->kind != "clipboard-write" &&
        request->kind != "clipboard-sequence" && request->kind != "clipboard-permission" &&
        request->kind != "menu" && request->kind != "drag-source" && request->kind != "fullscreen") {
      request->Deny(); return;
    }
    if ((request->kind == "drag-source" && request->sizes.size() != 2) ||
        (request->kind == "menu" && request->details->GetType("items") != VTYPE_LIST) ||
        !Generation(request)) { request->Deny(); return; }
    if (request->kind == "clipboard-write") {
      auto formats = request->details->GetList("formats");
      if (!formats || formats->GetSize() != count) { request->Deny(); return; }
      for (size_t index = 0; index < formats->GetSize(); ++index)
        if (formats->GetType(index) != VTYPE_STRING) { request->Deny(); return; }
    }
    {
      std::lock_guard lock(mutex_);
      if (retiring_ || owner != owner_ || requests_.contains(id)) return;
      requests_.emplace(id, request);
      tasks_.push_back([this, request] {
        if (!request->active) return;
        request->announced = true;
        if (request->kind == "menu") handler_->OnMenuRequest(new MenuRequest(request));
        else if (request->kind == "fullscreen") handler_->OnFullscreenRequest(new FullscreenRequest(request));
        else if (request->kind == "drag-source") handler_->OnDragRequest(new DragRequest(request));
        else handler_->OnClipboardRequest(new ClipboardRequest(request));
      });
      cv_.notify_one();
    }
  }
  void Resource(stream::Resource) override { Unavailable(); }
  void OwnerChanged(Owner owner) override {
    std::vector<std::shared_ptr<Request>> ended;
    {
      std::lock_guard lock(mutex_);
      owner_ = owner;
      for (auto found = requests_.begin(); found != requests_.end();) {
        if (found->second->owner != owner) {
          QueueEnded(found->second, "retired");
          ended.push_back(found->second); found = requests_.erase(found);
        } else { ++found; }
      }
    }
    for (auto& request : ended) request->End();
  }
  void ViewRetired(uint64_t) override {}
  void Retire() override {
    std::map<uint64_t, std::shared_ptr<Request>> pending;
    std::vector<std::shared_ptr<Read>> reads;
    {
      std::lock_guard lock(mutex_);
      if (retiring_) return;
      retiring_ = true;
      pending.swap(requests_);
      for (const auto& [id, request] : pending) QueueEnded(request, "retired");
      for (const auto& [key, read] : reads_) reads.push_back(read);
    }
    for (auto& [id, request] : pending) request->End();
    for (auto& read : reads) read->Finish(false);
    {
      std::lock_guard lock(mutex_);
      retired_ = true;
      cv_.notify_one();
    }
  }
  void Wait() override {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return done_; });
  }
 private:
  void Unavailable() {
    PostTask([this] { handler_->OnUnavailable(); });
  }
  // Owning mutex held: enqueue retirement before releasing the request from
  // the registry. Native callbacks and Stop can race without overtaking it.
  void QueueEnded(std::shared_ptr<Request> request, std::string reason) {
    request->active = false;
    tasks_.push_back([this, request, reason = std::move(reason)] {
      if (request->announced) handler_->OnRequestEnded(request->id, reason);
    });
    cv_.notify_one();
  }
  void EndRequest(Owner owner, uint64_t id, std::string reason) {
    std::shared_ptr<Request> request;
    {
      std::lock_guard lock(mutex_);
      auto found = requests_.find(id);
      if (found == requests_.end() || found->second->owner != owner) return;
      request = std::move(found->second);
      requests_.erase(found);
      QueueEnded(request, std::move(reason));
    }
    request->End();
  }
  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !tasks_.empty() || (retired_ && reads_.empty()); });
        if (tasks_.empty()) { finishing_ = true; break; }
        task = std::move(tasks_.front()); tasks_.pop_front();
      }
      task();
    }
    handler_->OnClosed();
    {
      std::lock_guard lock(mutex_);
      handler_ = nullptr; done_ = true; cv_.notify_all();
    }
  }
  const ExtensionEndpoint endpoint_;
  std::mutex mutex_;
  std::condition_variable cv_;
  Owner owner_;
  bool started_ = false, retiring_ = false, retired_ = false, finishing_ = false, done_ = false;
  CefRefPtr<AlloyGuestIntegrationHandler> handler_;
  std::map<uint64_t, std::shared_ptr<Request>> requests_;
  std::map<Read*, std::shared_ptr<Read>> reads_;
  std::deque<std::function<void()>> tasks_;
};

class Client final : public AlloyGuestIntegration {
 public:
  explicit Client(ExtensionEndpoint endpoint) : state_(std::make_shared<State>(std::move(endpoint))) {}
  bool Start(CefRefPtr<AlloyGuestIntegrationHandler> handler) override { return state_->Start(std::move(handler)); }
  void Stop() override { state_->Call("stop"); state_->Retire(); }
  bool ClipboardChanged() override { return state_->Call("clipboard-changed"); }
  uint64_t Edit(uint64_t document, uint64_t generation, int command,
      const CefString& source, uint64_t capture) override {
    if (!IsEditCommand(command)) return 0;
    auto data = CefDictionaryValue::Create(); data->SetInt("command", command);
    return state_->Send(document, generation, source, capture, "edit", data);
  }
  uint64_t DragEnter(uint64_t document, uint64_t generation, CefRefPtr<CefDragData> drag,
      int x, int y, int allowed, int modifiers, const CefString& source, uint64_t capture) override {
    if (!drag) return 0;
    std::vector<CefString> paths;
    if (drag->GetFilePaths(paths) && !paths.empty()) return 0;
    auto data = Point(x, y, modifiers); data->SetInt("allowed", allowed);
    data->SetString("text", drag->GetFragmentText()); data->SetString("html", drag->GetFragmentHtml());
    data->SetString("base", drag->GetFragmentBaseURL()); data->SetString("link", drag->GetLinkURL());
    data->SetString("title", drag->GetLinkTitle());
    return state_->Send(document, generation, source, capture, "drag-enter", data);
  }
  uint64_t DragOver(uint64_t document, uint64_t generation, int x, int y, int allowed,
      int modifiers, const CefString& source, uint64_t capture) override {
    auto data = Point(x, y, modifiers); data->SetInt("allowed", allowed);
    return state_->Send(document, generation, source, capture, "drag-over", data);
  }
  uint64_t DragDrop(uint64_t document, uint64_t generation, int x, int y, int modifiers,
      const CefString& source, uint64_t capture) override {
    return state_->Send(document, generation, source, capture, "drag-drop", Point(x, y, modifiers));
  }
  uint64_t DragLeave(uint64_t document, uint64_t generation, const CefString& source,
      uint64_t capture) override {
    return state_->Send(document, generation, source, capture, "drag-leave", CefDictionaryValue::Create());
  }
  uint64_t ExitFullscreen(uint64_t document, uint64_t generation, const CefString& source,
      uint64_t capture) override {
    return state_->Send(document, generation, source, capture, "exit-fullscreen", CefDictionaryValue::Create());
  }
 private:
  static CefRefPtr<CefDictionaryValue> Point(int x, int y, int modifiers) {
    auto data = CefDictionaryValue::Create(); data->SetInt("x", x);
    data->SetInt("y", y); data->SetInt("modifiers", modifiers); return data;
  }
  const std::shared_ptr<State> state_;
  IMPLEMENT_REFCOUNTING(Client);
};
}  // namespace

CefRefPtr<AlloyGuestIntegration> MakeClient(ExtensionEndpoint endpoint) {
  return new Client(std::move(endpoint));
}
}  // namespace alloy::guest::integration
