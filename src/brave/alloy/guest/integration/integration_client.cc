// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/integration_client.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <set>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "brave/alloy/guest/stream/input.h"
#include "base/unguessable_token.h"
#include "base/strings/string_number_conversions.h"
#include "brave/alloy/guest/integration/clipboard.h"
#include "cef/include/cef_api_hash.h"
#include "cef/libcef/common/api_version_util.h"

namespace alloy::guest::integration {
namespace {
// Chunk size is derived from the existing message envelope, not a content-size
// quota. One acknowledged chunk at a time gives the integration lane credit.
constexpr size_t kChunk = kMaxControlPayload / 2;
using Post = std::function<bool(std::function<void()>)>;
struct Read;
struct Request : public std::enable_shared_from_this<Request> {
  Owner owner;
  uint64_t id = 0;
  std::string kind;
  CefRefPtr<CefDictionaryValue> details;
  std::vector<uint64_t> sizes;
  ExtensionEndpoint endpoint;
  Post post;
  std::atomic<bool> active{true}, replied{false}, announced{false}, file_reply_queued{false};
  std::mutex mutex;
  CefRefPtr<AlloyGuestFile> file, destination;
  std::vector<CefRefPtr<AlloyGuestFile>> files;
  std::function<CefRefPtr<CefListValue>(const std::vector<CefRefPtr<AlloyGuestFile>>&)> register_files;
  std::function<void()> rollback_files;
  std::vector<std::weak_ptr<Read>> reads;
  std::function<bool(std::shared_ptr<Read>)> track_read;
  std::function<void(Read*)> retire_read;
  bool Call(std::string operation, std::string payload, ExtensionCompletion reply) {
    return active && endpoint.owned_call(Feature::kIntegration, owner,
        std::move(operation), std::move(payload), std::move(reply));
  }
  void End();
  bool Respond(std::string metadata, std::vector<std::string> bytes,
                 std::string source = "app-integration", uint64_t capture = 0, bool reserved = false);
  bool IsUiReply() const {
    return announced && (kind == "clipboard-write" || kind == "menu" ||
                         kind == "drag-source" || kind == "fullscreen" ||
                         kind == "select-files" || kind == "download" ||
                         (kind.starts_with("file-") &&
                          details->GetBool("interaction")));
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
                        std::string source, uint64_t capture, bool reserved) {
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
  if (!active || (reserved ? !file_reply_queued.load() : replied.exchange(true))) return false;
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
  size_t GetFileCount() override { return request_->files.size(); }
  CefRefPtr<AlloyGuestFile> GetFile(size_t index) override {
    return index < request_->files.size() ? request_->files[index] : nullptr;
  }
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
  size_t GetFileCount() override { return request_->files.size(); }
  CefRefPtr<AlloyGuestFile> GetFile(size_t index) override {
    return index < request_->files.size() ? request_->files[index] : nullptr;
  }
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

bool FileName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
      name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos;
}
CefRefPtr<CefDictionaryValue> FileMetadata(CefRefPtr<AlloyGuestFile> file) {
  if (!file) return nullptr;
  const auto name = file->GetName().ToString();
  const auto identity = file->GetIdentifier().ToString();
  const auto size = file->GetSize();
  if (!FileName(name) || !base::UnguessableToken::DeserializeFromString(identity) ||
      size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return nullptr;
  auto value = CefDictionaryValue::Create();
  value->SetString("name", name); value->SetString("size", std::to_string(size));
  value->SetString("identity", identity);
  std::vector<CefString> ancestry; file->GetAncestorIdentifiers(ancestry);
  auto ancestors = CefListValue::Create(); ancestors->SetSize(ancestry.size());
  std::set<std::string> seen{identity};
  for (size_t index = 0; index < ancestry.size(); ++index) {
    const auto id = ancestry[index].ToString();
    if (!base::UnguessableToken::DeserializeFromString(id) || !seen.insert(id).second) return nullptr;
    ancestors->SetString(index, id);
  }
  value->SetList("ancestors", ancestors);
  value->SetString("modified-ms", std::to_string(file->GetModifiedTimeMs()));
  value->SetString("mime", file->GetMimeType()); value->SetBool("directory", file->IsDirectory());
  value->SetBool("readable", file->CanRead()); value->SetBool("writable", file->CanWrite());
  return value;
}
bool FileResponse(const std::shared_ptr<Request>& request,
    CefRefPtr<CefDictionaryValue> details, std::vector<std::string> bytes,
    const std::string& source = "app-file", uint64_t capture = 0, bool reserved = false) {
  return request->Respond(stream::WriteDetails(details), std::move(bytes), source, capture, reserved);
}
bool FileMetadataReply(const std::shared_ptr<Request>& request, std::function<bool()> reply) {
  bool expected = false;
  if (!request->active || !request->replied.compare_exchange_strong(expected, true)) return false;
  request->file_reply_queued = true;
  const bool queued = request->post([request, reply = std::move(reply)] {
    const bool sent = request->active && reply();
    request->file_reply_queued = false;
    if (!sent && request->active) { request->replied = false; request->Deny(); }
  });
  if (!queued) { request->file_reply_queued = false; request->replied = false; }
  return queued;
}
bool FileEntries(const std::shared_ptr<Request>& request,
    const std::vector<CefRefPtr<AlloyGuestFile>>& files, const std::string& source, uint64_t capture) {
  if (!request->active || !request->file_reply_queued || !request->register_files) return false;
  auto entries = request->register_files(files);
  if (!entries) return false;
  auto body = CefDictionaryValue::Create(); body->SetList("entries", entries);
  auto details = CefDictionaryValue::Create(); details->SetString("status", "ok");
  const bool queued = FileResponse(request, details, {stream::WriteDetails(body)}, source, capture, true);
  if (!queued && request->rollback_files) request->rollback_files();
  return queued;
}
class FileSelection final : public AlloyGuestFileSelection {
 public:
  explicit FileSelection(std::shared_ptr<Request> request) : request_(std::move(request)) {}
  ~FileSelection() override { request_->Abandon(); }
  uint64_t GetId() override { return request_->id; }
  uint64_t GetDocumentId() override { return request_->owner.document; }
  CefString GetMode() override { return String(request_->details, "mode"); }
  CefString GetRequestingOrigin() override { return String(request_->details, "requesting-origin"); }
  CefString GetTitle() override { return String(request_->details, "title"); }
  CefString GetSuggestedName() override { return String(request_->details, "suggested-name"); }
  CefString GetSourceUrl() override { return String(request_->details, "source-url"); }
  CefString GetDownloadId() override { return String(request_->details, "download-id"); }
  size_t GetAcceptTypeCount() override {
    auto values = request_->details->GetList("accept"); return values ? values->GetSize() : 0;
  }
  CefString GetAcceptType(size_t index) override {
    auto values = request_->details->GetList("accept");
    return values && index < values->GetSize() ? values->GetString(index) : CefString();
  }
  bool Complete(const std::vector<CefRefPtr<AlloyGuestFile>>& files,
      const CefString& source, uint64_t capture) override {
    if (source.empty()) return false;
    return FileMetadataReply(request_, [request = request_, files, source = source.ToString(), capture] {
      return FileEntries(request, files, source, capture);
    });
  }
  void Deny() override { request_->Deny(); }
 private:
  const std::shared_ptr<Request> request_;
  IMPLEMENT_REFCOUNTING(FileSelection);
};
class FileOperation final : public AlloyGuestFileOperation {
 public:
  explicit FileOperation(std::shared_ptr<Request> request) : request_(std::move(request)) {}
  ~FileOperation() override { request_->Abandon(); }
  uint64_t GetId() override { return request_->id; }
  uint64_t GetDocumentId() override { return request_->owner.document; }
  CefString GetKind() override { return String(request_->details, "operation"); }
  uint64_t GetOffset() override { return Unsigned("offset"); }
  uint64_t GetLength() override { return Unsigned("length"); }
  int64_t GetExpectedModifiedTimeMs() override {
    int64_t value = 0;
    base::StringToInt64(String(request_->details, "expected-modified-ms"), &value); return value;
  }
  CefString GetName() override { return String(request_->details, "name"); }
  bool GetCreate() override { return request_->details->GetBool("create"); }
  bool GetRecursive() override { return request_->details->GetBool("recursive"); }
  bool GetKeepExistingData() override { return request_->details->GetBool("keep-existing"); }
  bool GetExclusiveWrite() override { return request_->details->GetBool("exclusive"); }
  bool GetWritePermission() override { return request_->details->GetBool("write"); }
  CefRefPtr<AlloyGuestFile> GetDestination() override { return request_->destination; }
  CefRefPtr<AlloyGuestData> GetData() override {
    return request_->sizes.size() == 1 ? CefRefPtr<AlloyGuestData>(new Data(request_, 0)) : nullptr;
  }
  bool Complete() override {
    const auto kind = GetKind();
    if (kind != "stat" && kind != "write" && kind != "truncate" && kind != "close" &&
        kind != "abort" && kind != "remove" && kind != "rename" && kind != "move") return false;
    auto details = CefDictionaryValue::Create(); details->SetString("status", "ok");
    if (kind != "stat") return FileResponse(request_, details, {});
    return FileMetadataReply(request_, [request = request_, details] {
      auto metadata = FileMetadata(request->file);
      return metadata && FileResponse(request, details, {stream::WriteDetails(metadata)}, "app-file", 0, true);
    });
  }
  bool CompleteRead(CefRefPtr<CefBinaryValue> value) override {
    if (GetKind() != "read" || (value && value->GetSize() > GetLength())) return false;
    std::string bytes(value ? value->GetSize() : 0, '\0');
    if (value && value->GetData(bytes.data(), bytes.size(), 0) != bytes.size()) return false;
    auto details = CefDictionaryValue::Create(); details->SetString("status", "ok");
    return FileResponse(request_, details, {std::move(bytes)});
  }
  bool CompleteEntries(const std::vector<CefRefPtr<AlloyGuestFile>>& files) override {
    const auto kind = GetKind();
    if (kind != "entries" && kind != "child-entry" && kind != "child-file" && kind != "child-directory" &&
        kind != "open-write") return false;
    return FileMetadataReply(request_, [request = request_, files] { return FileEntries(request, files, "app-file", 0); });
  }
  bool CompleteRelation(bool related, const std::vector<CefString>& path) override {
    if (GetKind() != "permission" && GetKind() != "compare" && GetKind() != "resolve") return false;
    auto body = CefDictionaryValue::Create(); body->SetBool("related", related);
    auto values = CefListValue::Create(); values->SetSize(path.size());
    for (size_t index = 0; index < path.size(); ++index) {
      if (!FileName(path[index].ToString())) return false;
      values->SetString(index, path[index]);
    }
    body->SetList("path", values);
    auto details = CefDictionaryValue::Create();
    details->SetString("status", GetKind() == "permission" && !related ? "denied" : "ok");
    return FileResponse(request_, details, {stream::WriteDetails(body)});
  }
  bool Fail(const CefString& error) override {
    const std::string value = error;
    if (value != "not-found" && value != "denied" && value != "exists" &&
        value != "not-directory" && value != "not-file" && value != "not-empty" &&
        value != "aborted" && value != "failed") return false;
    auto details = CefDictionaryValue::Create(); details->SetString("status", value);
    return FileResponse(request_, details, {});
  }
 private:
  uint64_t Unsigned(const char* name) const {
    uint64_t value = 0; ParseUnsigned(String(request_->details, name), &value); return value;
  }
  const std::shared_ptr<Request> request_;
  IMPLEMENT_REFCOUNTING(FileOperation);
};

class FilesContext final : public ExtensionClientState {
 public:
  struct Record { Owner owner; CefRefPtr<AlloyGuestFile> file; };
  static bool SameDocument(Owner first, Owner second) {
    return first.view == second.view && first.document == second.document;
  }
  CefRefPtr<AlloyGuestFile> Find(const std::string& token, std::optional<Owner> owner = {}) {
    std::lock_guard lock(mutex_);
    auto found = files_.find(token);
    return found != files_.end() && (!owner || SameDocument(found->second.owner, *owner))
        ? found->second.file : nullptr;
  }
  void Add(std::string token, Owner owner, CefRefPtr<AlloyGuestFile> file) {
    std::lock_guard lock(mutex_); files_.emplace(std::move(token), Record{owner, std::move(file)});
  }
  std::vector<CefRefPtr<AlloyGuestFile>> Remove(const std::string& token, std::optional<Owner> owner = {}) {
    std::vector<CefRefPtr<AlloyGuestFile>> removed;
    std::lock_guard lock(mutex_);
    auto found = files_.find(token);
    if (found != files_.end() && (!owner || SameDocument(found->second.owner, *owner))) {
      removed.push_back(std::move(found->second.file)); files_.erase(found);
    }
    return removed;
  }
  std::vector<CefRefPtr<AlloyGuestFile>> RetireView(uint64_t view, std::optional<uint64_t> keep_document = {}) {
    std::vector<CefRefPtr<AlloyGuestFile>> removed;
    std::lock_guard lock(mutex_);
    for (auto it = files_.begin(); it != files_.end();) {
      if (it->second.owner.view == view && (!keep_document || it->second.owner.document != *keep_document)) {
        removed.push_back(std::move(it->second.file)); it = files_.erase(it);
      } else { ++it; }
    }
    return removed;
  }
 private:
  std::mutex mutex_;
  std::map<std::string, Record> files_;
};

std::shared_ptr<FilesContext> FileContext(const ExtensionEndpoint& endpoint) {
  auto shared = endpoint.shared_state ? endpoint.shared_state(Feature::kIntegration,
      [] { return std::make_shared<FilesContext>(); }) : nullptr;
  return shared ? std::static_pointer_cast<FilesContext>(std::move(shared)) : std::make_shared<FilesContext>();
}

class State final : public ExtensionSink, public std::enable_shared_from_this<State> {
 public:
  explicit State(ExtensionEndpoint endpoint) : endpoint_(std::move(endpoint)), files_(FileContext(endpoint_)) {}
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
  bool PostTask(std::function<void()> task) {
    std::lock_guard lock(mutex_);
    if (finishing_ || done_) return false;
    tasks_.push_back(std::move(task));
    cv_.notify_one();
    return true;
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
    if (operation == "download-state") {
      auto details = stream::ParseDetails(payload);
      int64_t received = 0, total = 0;
      const auto id = String(details, "download-id"), state = String(details, "state");
      if (!base::UnguessableToken::DeserializeFromString(id) ||
          (state != "preparing" && state != "active" && state != "paused" && state != "complete" && state != "cancelled" && state != "interrupted") ||
          !base::StringToInt64(String(details, "received"), &received) ||
          !base::StringToInt64(String(details, "total"), &total)) { Unavailable(); return; }
      PostTask([this, id, state, received, total] { handler_->OnDownloadProgress(id, state, received, total); });
      return;
    }
    if (operation == "file-retired") {
      auto details = stream::ParseDetails(payload);
      const auto token = String(details, "capability");
      if (!base::UnguessableToken::DeserializeFromString(token)) { Unavailable(); return; }
      auto removed = files_->Remove(token, owner);
      PostTask([removed = std::move(removed)] {});
      return;
    }
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
      auto self = weak.lock(); return self && self->PostTask(std::move(task));
    };
    request->track_read = [weak = weak_from_this()](std::shared_ptr<Read> read) {
      auto self = weak.lock(); return self && self->TrackRead(std::move(read));
    };
    request->retire_read = [weak = weak_from_this()](Read* read) {
      if (auto self = weak.lock()) self->RetireRead(read);
    };
    request->register_files = [weak = weak_from_this(), owner, id](
        const std::vector<CefRefPtr<AlloyGuestFile>>& files) {
      auto self = weak.lock();
      return self ? self->RegisterFiles(owner, id, files) : CefRefPtr<CefListValue>();
    };
    request->rollback_files = [weak = weak_from_this(), id] {
      if (auto self = weak.lock()) self->DropIssuedFiles(id, false);
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
        request->kind != "menu" && request->kind != "drag-source" && request->kind != "fullscreen" &&
        request->kind != "select-files" && request->kind != "download" &&
        request->kind != "file-read" && request->kind != "file-write" &&
        request->kind != "file-truncate" && request->kind != "file-close") {
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
      std::unique_lock lock(mutex_);
      if (retiring_ || owner != owner_ || requests_.contains(id)) return;
      if (request->details->HasKey("file-capabilities")) {
        auto tokens = request->details->GetList("file-capabilities");
        if (!tokens || (request->kind != "clipboard-write" && request->kind != "drag-source")) {
          lock.unlock(); request->Deny(); return;
        }
        for (size_t index = 0; index < tokens->GetSize(); ++index) {
          auto file = tokens->GetType(index) == VTYPE_STRING ? files_->Find(tokens->GetString(index), owner) : nullptr;
          if (!file) { lock.unlock(); request->Deny(); return; }
          request->files.push_back(std::move(file));
        }
      }
      if (request->kind.starts_with("file-")) {
        request->file = files_->Find(String(request->details, "capability"), owner);
        if (!request->file) {
          lock.unlock(); request->Deny(); return;
        }
        const auto destination = String(request->details, "destination");
        if (!destination.empty()) {
          request->destination = files_->Find(destination);
          if (!request->destination) {
            lock.unlock(); request->Deny(); return;
          }
        }
      }
      requests_.emplace(id, request);
      tasks_.push_back([this, request] {
        if (!request->active) return;
        request->announced = true;
        if (request->kind == "select-files" || request->kind == "download")
          handler_->OnFileSelection(new FileSelection(request));
        else if (request->kind.starts_with("file-")) request->file->OnOperation(new FileOperation(request));
        else if (request->kind == "menu") handler_->OnMenuRequest(new MenuRequest(request));
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
    auto removed_files = files_->RetireView(owner.view, owner.document);
    PostTask([removed_files = std::move(removed_files)] {});
    for (auto& request : ended) { DropIssuedFiles(request->id, false); request->End(); }
  }
  void ViewRetired(uint64_t) override {}
  void Retire() override {
    std::map<uint64_t, std::shared_ptr<Request>> pending;
    uint64_t view = 0;
    std::vector<std::shared_ptr<Read>> reads;
    {
      std::lock_guard lock(mutex_);
      if (retiring_) return;
      retiring_ = true;
      pending.swap(requests_);
      view = owner_.view; file_issuances_.clear();
      for (const auto& [id, request] : pending) QueueEnded(request, "retired");
      for (const auto& [key, read] : reads_) reads.push_back(read);
    }
    auto removed_files = files_->RetireView(view);
    PostTask([removed_files = std::move(removed_files)] {});
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
  static bool SameDocument(Owner first, Owner second) {
    return first.view == second.view && first.document == second.document;
  }
  CefRefPtr<CefListValue> RegisterFiles(Owner owner, uint64_t id,
      const std::vector<CefRefPtr<AlloyGuestFile>>& files) {
    std::vector<CefRefPtr<CefDictionaryValue>> metadata;
    for (const auto& file : files) {
      auto details = FileMetadata(file);
      if (!details) return nullptr;
      metadata.push_back(details);
    }
    auto result = CefListValue::Create(); result->SetSize(files.size());
    std::lock_guard lock(mutex_);
    auto request = requests_.find(id);
    if (retiring_ || request == requests_.end() || !request->second->active ||
        (request->second->replied && !request->second->file_reply_queued) || !SameDocument(owner, owner_)) return nullptr;
    for (size_t index = 0; index < files.size(); ++index) {
      const auto token = base::UnguessableToken::Create().ToString();
      metadata[index]->SetString("capability", token);
      files_->Add(token, owner, files[index]);
      file_issuances_[id].push_back(token);
      result->SetDictionary(index, metadata[index]);
    }
    return result;
  }
  void DropIssuedFiles(uint64_t id, bool keep) {
    std::vector<std::string> tokens;
    {
      std::lock_guard lock(mutex_);
      auto found = file_issuances_.find(id);
      if (found == file_issuances_.end()) return;
      if (!keep) tokens = std::move(found->second);
      file_issuances_.erase(found);
    }
    for (const auto& token : tokens) {
      auto removed = files_->Remove(token);
      PostTask([removed = std::move(removed)] {});
    }
  }
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
      QueueEnded(request, reason);
    }
    DropIssuedFiles(id, reason == "completed");
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
  const std::shared_ptr<FilesContext> files_;
  std::mutex mutex_;
  std::condition_variable cv_;
  Owner owner_;
  bool started_ = false, retiring_ = false, retired_ = false, finishing_ = false, done_ = false;
  CefRefPtr<AlloyGuestIntegrationHandler> handler_;
  std::map<uint64_t, std::shared_ptr<Request>> requests_;
  std::map<uint64_t, std::vector<std::string>> file_issuances_;
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
  uint64_t DragEnterFiles(uint64_t document, uint64_t generation, int x, int y, int allowed,
      int modifiers, const CefString& source, uint64_t capture) override {
    auto data = Point(x, y, modifiers); data->SetInt("allowed", allowed);
    return state_->Send(document, generation, source, capture, "drag-enter-files", data);
  }
  uint64_t DownloadControl(uint64_t document, uint64_t generation, const CefString& identity,
      const CefString& operation, const CefString& source, uint64_t capture) override {
    const auto command = operation.ToString();
    if (!base::UnguessableToken::DeserializeFromString(identity.ToString()) ||
        (command != "cancel" && command != "pause" && command != "resume")) return 0;
    auto data = CefDictionaryValue::Create(); data->SetString("download-id", identity); data->SetString("operation", command);
    return state_->Send(document, generation, source, capture, "download-control", data);
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

CefString AlloyCreateGuestFileIdentity() {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  return base::UnguessableToken::Create().ToString();
}
