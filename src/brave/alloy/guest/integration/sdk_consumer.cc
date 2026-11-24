// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Synthetic app provider and consumer of the translated SDK, never a product
// logging or injection path. All browser actions use the ordinary typed APIs.
#include "include/alloy/alloy_guest_session.h"
#include "include/cef_api_hash.h"
#include "include/cef_parser.h"
#include "brave/alloy/guest/integration/sdk_files.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::mutex output;
void Event(const char* name, CefRefPtr<CefDictionaryValue> data = nullptr) {
  if (!data) data = CefDictionaryValue::Create();
  data->SetString("event", name);
  auto value = CefValue::Create(); value->SetDictionary(data);
  std::lock_guard lock(output);
  std::cout << "GUEST_INTEGRATION_EVENT " << CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString() << std::endl;
}
std::string Text(CefRefPtr<CefDictionaryValue> value, const char* key) {
  return value->GetType(key) == VTYPE_STRING ? value->GetString(key).ToString() : std::string();
}
uint64_t U(CefRefPtr<CefDictionaryValue> value, const char* key, uint64_t fallback = 0) {
  if (value->GetType(key) == VTYPE_STRING) {
    std::istringstream input(Text(value, key)); uint64_t result = 0;
    if (input >> result) return result;
  }
  if (value->GetType(key) == VTYPE_INT) return value->GetInt(key);
  if (value->GetType(key) == VTYPE_DOUBLE) return value->GetDouble(key);
  return fallback;
}
uint64_t Hash(const std::string& bytes) {
  uint64_t result = 1469598103934665603ULL;
  for (unsigned char byte : bytes) result = (result ^ byte) * 1099511628211ULL;
  return result;
}
int Edit(CefRefPtr<CefDictionaryValue> data, bool quick = false) {
  if (data->GetType("command") == VTYPE_INT) return data->GetInt("command");
  const auto command = Text(data, "command");
  if (quick) {
    if (command == "copy") return QM_EDITFLAG_CAN_COPY;
    if (command == "cut") return QM_EDITFLAG_CAN_CUT;
    if (command == "paste") return QM_EDITFLAG_CAN_PASTE;
    if (command == "more") return QM_EDITFLAG_CAN_ELLIPSIS;
    return -1;
  }
  if (command == "copy") return MENU_ID_COPY;
  if (command == "cut") return MENU_ID_CUT;
  if (command == "paste") return MENU_ID_PASTE;
  if (command == "paste-match-style") return MENU_ID_PASTE_MATCH_STYLE;
  if (command == "select-all") return MENU_ID_SELECT_ALL;
  if (command == "undo") return MENU_ID_UNDO;
  if (command == "redo") return MENU_ID_REDO;
  return -1;
}
class Reader final : public AlloyGuestDataHandler {
 public:
  explicit Reader(std::function<void(bool, uint64_t, CefRefPtr<CefBinaryValue>)> done)
      : done_(std::move(done)) {}
  void OnRead(bool success, uint64_t offset, CefRefPtr<CefBinaryValue> data) override {
    done_(success, offset, std::move(data));
  }
 private:
  const std::function<void(bool, uint64_t, CefRefPtr<CefBinaryValue>)> done_;
  IMPLEMENT_REFCOUNTING(Reader);
};
class Stream final : public AlloyGuestStreamHandler {
 public:
  std::function<void(uint64_t)> configured;
  std::function<void(uint64_t)> document_ready;
  void OnConfigured(uint64_t generation, int width, int height, double scale) override {
    configured(generation);
    auto event = CefDictionaryValue::Create(); event->SetString("generation", std::to_string(generation));
    event->SetInt("width", width); event->SetInt("height", height); event->SetDouble("scale", scale);
    Event("configured", event);
  }
  void OnFrame(CefRefPtr<AlloyGuestFrame> frame) override {
    if (frame->GetSurface() || frame->GetGeneration() == seen_) return;
    seen_ = frame->GetGeneration();
    document_ready(frame->GetDocumentId());
    uint8_t pixels[4] = {};
    const auto read = frame->ReadPixels(pixels, sizeof(pixels), 0);
    auto event = CefDictionaryValue::Create();
    event->SetString("document", std::to_string(frame->GetDocumentId()));
    event->SetString("generation", std::to_string(frame->GetGeneration()));
    event->SetInt("width", frame->GetWidth()); event->SetInt("height", frame->GetHeight());
    event->SetInt("pixel_bytes", read); Event("frame-consumed", event);
  }
  void OnInteraction(CefRefPtr<AlloyGuestInteraction> interaction) override {
    auto event = interaction->GetDetails();
    event->SetString("id", std::to_string(interaction->GetId()));
    event->SetString("document", std::to_string(interaction->GetDocumentId()));
    event->SetString("generation", std::to_string(interaction->GetGeneration()));
    event->SetString("kind", interaction->GetKind()); event->SetString("source", interaction->GetSource());
    event->SetString("capture_us", std::to_string(interaction->GetAppCaptureTimeUs()));
    event->SetString("dispatch_us", std::to_string(interaction->GetDispatchTimeUs()));
    event->SetString("outcome", interaction->GetOutcome()); Event("interaction", event);
  }
  void OnPopup(bool visible, const CefRect& bounds, uint64_t generation) override {
    auto event = CefDictionaryValue::Create(); event->SetBool("visible", visible);
    event->SetString("generation", std::to_string(generation));
    event->SetInt("x", bounds.x); event->SetInt("y", bounds.y);
    event->SetInt("width", bounds.width); event->SetInt("height", bounds.height); Event("popup", event);
  }
  void OnWindowOpen(CefRefPtr<AlloyGuestPopup> popup, const CefString&, const CefString&,
                    int, bool, const CefRect&) override { popup->Deny(); }
  void OnWindowRetired(uint64_t, const CefString&) override {}
  void OnCursor(int, CefRefPtr<CefBinaryValue>, int, int, int, int, double, uint64_t) override {}
  void OnImeBounds(const CefRange&, const std::vector<CefRect>&, uint64_t) override {}
  void OnUnavailable() override { Event("stream-unavailable"); }
  void OnClosed() override { Event("stream-closed"); }
 private:
  uint64_t seen_ = 0;
  IMPLEMENT_REFCOUNTING(Stream);
};
struct Write : public std::enable_shared_from_this<Write> {
  CefRefPtr<AlloyGuestClipboardRequest> request;
  std::map<std::string, std::string> formats;
  // Consumer::mutex_ publishes completion, acceptance intent and cancellation.
  // The callback sequence alone owns formats until this completed map exists.
  std::optional<std::map<std::string, std::string>> ready_formats;
  bool accept_requested = false;
  std::function<void(std::shared_ptr<Write>, bool)> done;
  // Synthetic app gate for deterministic acceptance-during-read coverage.
  std::function<void(std::function<void()>)> deliver_data;
  size_t index = 0;
  uint64_t offset = 0;
  void Next() {
    if (index == request->GetFormatCount()) { done(shared_from_this(), true); return; }
    auto data = request->GetData(index);
    if (!data || offset > data->GetSize()) { done(shared_from_this(), false); return; }
    const auto name = request->GetFormatName(index).ToString();
    formats.try_emplace(name);
    const auto count = std::min<uint64_t>(65536, data->GetSize() - offset);
    CefRefPtr<Reader> reader = new Reader(
        [self = shared_from_this(), size = data->GetSize(), count, name]
        (bool success, uint64_t offset, CefRefPtr<CefBinaryValue> bytes) {
          self->deliver_data([self, size, count, name, success, offset, bytes] {
            if (!success || offset != self->offset || (bytes ? bytes->GetSize() : 0) != count) {
              self->done(self, false); return;
            }
            std::string copied(count, '\0');
            if (bytes && bytes->GetData(copied.data(), copied.size(), 0) != copied.size()) {
              self->done(self, false); return;
            }
            self->formats[name].append(copied); self->offset += count;
            if (self->offset == size) { ++self->index; self->offset = 0; }
            self->Next();
          });
        });
    if (!data->Read(offset, count, reader)) done(shared_from_this(), false);
  }
};
class Consumer final : public AlloyGuestSessionHandler, public AlloyGuestViewHandler,
                       public AlloyGuestIntegrationHandler {
 public:
  explicit Consumer(bool attach) : attach_(attach) {}
  bool Start(const std::string& mode) {
    auto settings = AlloyCreateGuestSettings(mode, "keep", "unavailable", true, "");
    auto session = AlloyStartGuestSession(settings, this);
    if (!session) return false;
    std::lock_guard lock(mutex_); if (!session_) session_ = session;
    return true;
  }
  void OnPrepared(CefRefPtr<AlloyGuestSession> session, CefRefPtr<AlloyGuestProtection>,
                  CefRefPtr<AlloyGuestCapabilities> capabilities) override {
    { std::lock_guard lock(mutex_); session_ = session; }
    auto event = CefDictionaryValue::Create(); event->SetBool("integration", capabilities->IsAvailable("integration"));
    Event("prepared", event); session->Accept();
  }
  void OnStateChanged(CefRefPtr<AlloyGuestSession> session, const CefString& state) override {
    auto event = CefDictionaryValue::Create(); event->SetString("state", state); Event("state", event);
    if (state == "ACTIVE") session->CreateView(640, 480, this);
  }
  void OnReady(CefRefPtr<AlloyGuestView> view) override {
    auto stream = view->GetStream(); auto integration = view->GetIntegration();
    { std::lock_guard lock(mutex_); view_ = view; stream_ = stream; integration_ = integration; }
    CefRefPtr<Stream> observer = new Stream;
    auto self = CefRefPtr<Consumer>(this);
    observer->configured = [self](uint64_t generation) {
      std::lock_guard lock(self->mutex_); self->generation_ = generation;
    };
    observer->document_ready = [self](uint64_t document) {
      self->ObserveDocument(document);
    };
    const bool started = stream && stream->Start(observer, 3, 16 * 1024 * 1024);
    const bool attached = attach_ && integration && integration->Start(this);
    if (started) stream->Configure(640, 480, 1, 30);
    auto event = CefDictionaryValue::Create(); event->SetBool("stream", started); event->SetBool("attached", attached);
    Event("view-ready", event);
  }
  void OnTitleChanged(CefRefPtr<AlloyGuestView>, uint64_t document, const CefString& title) override {
    ObserveDocument(document);
    auto event = CefDictionaryValue::Create(); event->SetString("document", std::to_string(document));
    event->SetString("title", title); Event("title", event);
  }
  void OnNavigationComplete(CefRefPtr<AlloyGuestView>, uint64_t request,
                            uint64_t document, int status) override {
    // Outcomes describe requests. Only admitted titles and frames describe
    // the document available for interaction, including after an aborted load.
    auto event = CefDictionaryValue::Create(); event->SetString("document", std::to_string(document));
    event->SetString("request", std::to_string(request));
    event->SetInt("status", status); Event("load", event);
  }
  void OnClosed(CefRefPtr<AlloyGuestView>, const CefString& reason) override {
    auto event = CefDictionaryValue::Create(); event->SetString("reason", reason); Event("view-closed", event);
  }
  void OnClosed(CefRefPtr<AlloyGuestSession>, const CefString& reason) override {
    auto event = CefDictionaryValue::Create(); event->SetString("reason", reason); Event("closed", event);
    std::lock_guard lock(mutex_);
    session_ = nullptr; view_ = nullptr; stream_ = nullptr; integration_ = nullptr;
    closed_ = true; cv_.notify_all();
  }
  void OnClosed() override {
    std::lock_guard lock(mutex_); reads_.clear(); menus_.clear(); drags_.clear(); fullscreen_.clear(); writes_.clear(); write_data_.clear();
    Event("integration-closed");
  }
  void OnUnavailable() override { Event("integration-unavailable"); }
  void OnFileSelection(CefRefPtr<AlloyGuestFileSelection> request) override { files_.Select(request); }
  void OnDownloadProgress(const CefString& id, const CefString& state, int64_t received, int64_t total) override {
    auto event = CefDictionaryValue::Create(); event->SetString("download", id); event->SetString("state", state);
    event->SetString("received", std::to_string(received)); event->SetString("total", std::to_string(total));
    Event("download-state", event);
  }
  void OnRequestEnded(uint64_t id, const CefString& reason) override {
    files_.Ended(id);
    { std::lock_guard lock(mutex_);
      if (auto found = reads_.find(id); found != reads_.end()) retired_reads_[id] = found->second;
      if (auto found = menus_.find(id); found != menus_.end()) retired_menus_[id] = found->second;
      if (auto found = drags_.find(id); found != drags_.end()) retired_drags_[id] = found->second;
      if (auto found = fullscreen_.find(id); found != fullscreen_.end()) retired_fullscreen_[id] = found->second;
      reads_.erase(id); menus_.erase(id); drags_.erase(id); fullscreen_.erase(id); writes_.erase(id); write_data_.erase(id); }
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(id));
    event->SetString("reason", reason); Event("request-ended", event);
  }
  void OnClipboardRequest(CefRefPtr<AlloyGuestClipboardRequest> request) override {
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(request->GetId()));
    event->SetString("document", std::to_string(request->GetDocumentId()));
    event->SetString("origin", request->GetRequestingOrigin()); event->SetString("format", request->GetRequestedFormat());
    event->SetBool("write", request->IsWrite()); event->SetBool("permission", request->IsPermissionQuery());
    Event("clipboard-request", event);
    if (request->IsSequenceQuery()) {
      std::string sequence;
      { std::lock_guard lock(mutex_); std::ostringstream value;
        value << std::hex << std::setw(16) << std::setfill('0') << uint64_t{1}
              << std::setw(16) << sequence_; sequence = value.str(); }
      request->CompleteSequence(sequence); return;
    }
    if (request->IsPermissionQuery()) {
      bool allow = false; { std::lock_guard lock(mutex_); allow = permissions_; }
      request->CompletePermission(allow); return;
    }
    if (request->IsWrite()) {
      auto write = std::make_shared<Write>(); write->request = request;
      { std::lock_guard lock(mutex_); writes_[request->GetId()] = write; }
      auto self = CefRefPtr<Consumer>(this);
      write->done = [self](std::shared_ptr<Write> value, bool success) { self->WriteReady(value, success); };
      write->deliver_data = [self, id = request->GetId()](std::function<void()> deliver) {
        bool held = false;
        { std::lock_guard lock(self->mutex_);
          if (!self->writes_.contains(id)) return;
          held = self->hold_write_data_;
          if (held) self->write_data_[id] = std::move(deliver); }
        if (held) {
          auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(id));
          Event("write-data-held", event);
        } else deliver();
      };
      write->Next(); return;
    }
    const auto format = request->GetRequestedFormat().ToString();
    if (format == "@types") {
      std::vector<CefString> types;
      { std::lock_guard lock(mutex_); for (const auto& [type, bytes] : clipboard_) types.emplace_back(type); }
      request->CompleteTypes(types); return;
    }
    std::string bytes; bool held = false, denied = false;
    {
      std::lock_guard lock(mutex_); held = hold_reads_; denied = deny_reads_;
      if (held) reads_[request->GetId()] = request;
      if (auto found = clipboard_.find(format); found != clipboard_.end()) bytes = found->second;
    }
    if (held) { Event("read-held", event); return; }
    if (denied) { request->Deny(); return; }
    auto binary = bytes.empty() ? nullptr : CefBinaryValue::Create(bytes.data(), bytes.size());
    // Fixture HTML is UTF-8; CEF strings expose the native UTF-16 fragment extent.
    const uint32_t end = format == "text/html" ? CefString(bytes).length() : 0;
    request->CompleteRead(binary, "https://clipboard-source.test/", 0, end);
  }
  void WriteReady(std::shared_ptr<Write> write, bool success) {
    bool hold = false, fail = false;
    { std::lock_guard lock(mutex_);
      const auto found = writes_.find(write->request->GetId());
      if (found == writes_.end() || found->second != write) return;
      if (!success) writes_.erase(found);
      else {
        write->ready_formats = std::move(write->formats);
        hold = hold_writes_ && !write->accept_requested;
        fail = fail_writes_;
        if (fail) writes_.erase(found);
      }
    }
    if (!success) { write->request->Deny(); Event("write-aborted"); return; }
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(write->request->GetId()));
    Event("write-ready", event);
    if (fail) { write->request->Deny(); Event("sink-failed", event); return; }
    if (hold) return;
    StoreWrite(std::move(write));
  }
  void StoreWrite(std::shared_ptr<Write> write) {
    { std::lock_guard lock(mutex_);
      const auto found = writes_.find(write->request->GetId());
      if (found == writes_.end() || found->second != write) return;
      write->accept_requested = true;
      if (!write->ready_formats) return;
      clipboard_ = std::move(*write->ready_formats); ++sequence_; writes_.erase(found); }
    const bool queued = write->request->CompleteWrite("app-clipboard", 54321);
    auto event = CefDictionaryValue::Create(); event->SetBool("ok", queued);
    event->SetString("request", std::to_string(write->request->GetId())); Event("clipboard-stored", event);
  }
  void OnMenuRequest(CefRefPtr<AlloyGuestMenuRequest> request) override {
    { std::lock_guard lock(mutex_); menus_[request->GetId()] = request; }
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(request->GetId()));
    event->SetString("document", std::to_string(request->GetDocumentId()));
    event->SetInt("x", request->GetX()); event->SetInt("y", request->GetY());
    event->SetBool("quick", request->IsQuickMenu()); event->SetList("items", request->GetItems()); Event("menu", event);
  }
  void OnDragRequest(CefRefPtr<AlloyGuestDragRequest> request) override {
    { std::lock_guard lock(mutex_); drags_[request->GetId()] = request; }
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(request->GetId()));
    event->SetString("text", request->GetText()); event->SetString("html", request->GetHtml());
    event->SetString("link", request->GetLinkUrl()); event->SetInt("allowed", request->GetAllowedOperations());
    event->SetInt("files", request->GetFileCount());
    if (request->GetFileCount()) {
      auto file = request->GetFile(0);
      if (file) event->SetString("file_name", file->GetName());
    }
    event->SetString("file_bytes", std::to_string(request->GetFileData()->GetSize()));
    event->SetString("image_bytes", std::to_string(request->GetImagePng()->GetSize())); Event("drag-source", event);
  }
  void OnFullscreenRequest(CefRefPtr<AlloyGuestFullscreenRequest> request) override {
    { std::lock_guard lock(mutex_); fullscreen_[request->GetId()] = request; }
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(request->GetId()));
    event->SetString("origin", request->GetRequestingOrigin()); Event("fullscreen", event);
  }
  void OnFullscreenChanged(uint64_t document, uint64_t generation, bool fullscreen) override {
    CefRefPtr<AlloyGuestStream> stream;
    { std::lock_guard lock(mutex_); stream = stream_; }
    if (!fullscreen && stream) stream->Configure(640, 480, 1, 30);
    auto event = CefDictionaryValue::Create(); event->SetBool("fullscreen", fullscreen);
    event->SetString("document", std::to_string(document)); event->SetString("generation", std::to_string(generation));
    Event("fullscreen-changed", event);
  }
  void OnDragOperationChanged(int operation, uint64_t generation) override {
    auto event = CefDictionaryValue::Create(); event->SetInt("operation", operation);
    event->SetString("generation", std::to_string(generation)); Event("drag-effect-consumed", event);
  }
  bool Command(const std::string& line) {
    auto value = CefParseJSON(line, JSON_PARSER_RFC);
    auto data = value ? value->GetDictionary() : nullptr;
    if (!data) return false;
    const auto op = Text(data, "op");
    CefRefPtr<AlloyGuestSession> session; CefRefPtr<AlloyGuestView> view;
    CefRefPtr<AlloyGuestStream> stream; CefRefPtr<AlloyGuestIntegration> integration;
    uint64_t document = 0, generation = 0;
    { std::lock_guard lock(mutex_); session = session_; view = view_; stream = stream_; integration = integration_;
      document = U(data, "document", document_); generation = U(data, "generation", generation_); }
    const auto source = Text(data, "source").empty() ? "app" : Text(data, "source");
    const auto capture = U(data, "capture_us", 12345);
    uint64_t id = 0; bool queued = false;
    if (op == "navigate") id = view ? view->Navigate(Text(data, "url")) : 0;
    else if (op == "close") { if (session) session->Close(); queued = true; }
    else if (op == "terminate") { if (session) session->Terminate(); queued = true; }
    else if (op == "stop-provider") { if (integration) integration->Stop(); queued = true; }
    else if (op == "configure") {
      std::lock_guard lock(mutex_);
      permissions_ = data->GetBool("permissions"); hold_reads_ = data->GetBool("hold_reads");
      deny_reads_ = data->GetBool("deny_reads"); hold_writes_ = data->GetBool("hold_writes");
      fail_writes_ = data->GetBool("fail_writes"); queued = true;
      hold_write_data_ = data->GetBool("hold_write_data");
    } else if (op == "files-configure") {
      files_.Configure(data); queued = true;
    } else if (op == "files-get") {
      Event("files", files_.Snapshot()); queued = true;
    } else if (op == "files-release") {
      files_.Release(); queued = true;
    } else if (op == "clipboard-set") {
      auto formats = data->GetDictionary("formats"); std::vector<CefString> keys;
      if (!formats || !formats->GetKeys(keys)) return false;
      std::map<std::string, std::string> clipboard;
      for (const auto& key : keys) {
        auto binary = CefBase64Decode(formats->GetString(key));
        std::string bytes(binary ? binary->GetSize() : 0, '\0');
        if (binary) binary->GetData(bytes.data(), bytes.size(), 0);
        clipboard[key.ToString()] = std::move(bytes);
      }
      { std::lock_guard lock(mutex_); clipboard_ = std::move(clipboard); ++sequence_; }
      queued = !integration || integration->ClipboardChanged();
    } else if (op == "clipboard-get") {
      auto formats = CefDictionaryValue::Create(); auto hashes = CefDictionaryValue::Create();
      { std::lock_guard lock(mutex_); for (const auto& [name, bytes] : clipboard_) {
          hashes->SetString(name, std::to_string(Hash(bytes)));
          if (data->GetBool("data")) formats->SetString(name, CefBase64Encode(bytes.data(), bytes.size()));
        } }
      auto event = CefDictionaryValue::Create(); event->SetDictionary("formats", formats); event->SetDictionary("hashes", hashes);
      Event("clipboard-content", event); queued = true;
    } else if (op == "accept-write") {
      std::shared_ptr<Write> write;
       { std::lock_guard lock(mutex_); auto found = writes_.find(U(data, "request"));
         if (found != writes_.end()) write = found->second; }
      if (write) { StoreWrite(std::move(write)); queued = true; }
    } else if (op == "release-write-data") {
      std::function<void()> deliver;
      { std::lock_guard lock(mutex_); hold_write_data_ = false;
        auto found = write_data_.find(U(data, "request"));
        if (found != write_data_.end()) { deliver = std::move(found->second); write_data_.erase(found); } }
      if (deliver) { deliver(); queued = true; }
    } else if (op == "menu-select") {
      CefRefPtr<AlloyGuestMenuRequest> request;
      { std::lock_guard lock(mutex_); const auto number = U(data, "request");
        if (auto found = menus_.find(number); found != menus_.end()) request = found->second;
        else if (auto old = retired_menus_.find(number); old != retired_menus_.end()) request = old->second; }
      queued = request && request->Select(Edit(data, request->IsQuickMenu()), data->GetInt("flags"),
          data->GetBool("handled"), source, capture);
    } else if (op == "drag-finish") {
      CefRefPtr<AlloyGuestDragRequest> request;
      { std::lock_guard lock(mutex_); const auto number = U(data, "request");
        if (auto found = drags_.find(number); found != drags_.end()) request = found->second;
        else if (auto old = retired_drags_.find(number); old != retired_drags_.end()) request = old->second; }
      queued = request && request->Finish(data->GetInt("x"), data->GetInt("y"), data->GetInt("operation"), source, capture);
    } else if (op == "fullscreen-decide") {
      CefRefPtr<AlloyGuestFullscreenRequest> request;
      bool retired = false;
      { std::lock_guard lock(mutex_); const auto number = U(data, "request");
        if (auto found = fullscreen_.find(number); found != fullscreen_.end()) request = found->second;
        else if (auto old = retired_fullscreen_.find(number); old != retired_fullscreen_.end()) { request = old->second; retired = true; } }
      if (request && !retired && data->GetBool("allow") && stream) stream->Configure(960, 540, 1, 30);
      queued = request && request->Decide(data->GetBool("allow"), source, capture);
    } else if (op == "reply-read") {
      CefRefPtr<AlloyGuestClipboardRequest> request;
      { std::lock_guard lock(mutex_); const auto number = U(data, "request");
        if (auto found = reads_.find(number); found != reads_.end()) request = found->second;
        else if (auto old = retired_reads_.find(number); old != retired_reads_.end()) request = old->second; }
      const auto bytes = Text(data, "text");
      queued = request && request->CompleteRead(CefBinaryValue::Create(bytes.data(), bytes.size()), "", 0, 0);
    } else if (op == "edit") {
      id = integration ? integration->Edit(document, generation, Edit(data), source, capture) : 0;
    } else if (op == "focus") {
      id = stream ? stream->SendFocus(source, document, generation, data->GetBool("focused"), capture) : 0;
    } else if (op == "pointer") {
      id = stream ? stream->SendPointer(source, document, generation, U(data, "surface"), data->GetInt("x"), data->GetInt("y"),
          Text(data, "action"), data->GetInt("button"), data->GetInt("clicks"), U(data, "modifiers"), capture) : 0;
    } else if (op == "key") {
      CefKeyEvent key;
      const auto type = Text(data, "type");
      key.type = type == "up" ? KEYEVENT_KEYUP : type == "char" ? KEYEVENT_CHAR : KEYEVENT_RAWKEYDOWN;
      key.modifiers = U(data, "modifiers"); key.windows_key_code = data->GetInt("key");
      key.native_key_code = data->GetInt("native"); key.character = data->GetInt("character");
      key.unmodified_character = data->GetInt("character");
      id = stream ? stream->SendKey(source, document, generation, key, capture) : 0;
    } else if (op == "touch") {
      CefTouchEvent touch;
      touch.id = data->GetInt("contact");
      const auto type = Text(data, "type");
      touch.type = type == "press" ? CEF_TET_PRESSED : type == "release" ? CEF_TET_RELEASED
          : type == "cancel" ? CEF_TET_CANCELLED : CEF_TET_MOVED;
      touch.x = data->GetType("x") == VTYPE_INT ? data->GetInt("x") : data->GetDouble("x");
      touch.y = data->GetType("y") == VTYPE_INT ? data->GetInt("y") : data->GetDouble("y");
      touch.radius_x = touch.radius_y = 1;
      touch.pressure = touch.type == CEF_TET_RELEASED || touch.type == CEF_TET_CANCELLED ? 0 : 1;
      touch.pointer_type = CEF_POINTER_TYPE_TOUCH;
      id = stream ? stream->SendTouch(source, document, generation, U(data, "surface"), touch, capture) : 0;
    } else if (op == "download-control") {
      id = integration ? integration->DownloadControl(document, generation, Text(data, "download"),
          Text(data, "command"), source, capture) : 0;
    } else if (op == "drag-file-enter") {
      id = integration ? integration->DragEnterFiles(document, generation, data->GetInt("x"), data->GetInt("y"),
          data->GetInt("allowed"), data->GetInt("modifiers"), source, capture) : 0;
    } else if (op == "drag-enter") {
      auto drag = CefDragData::Create(); drag->SetFragmentText(Text(data, "text"));
      drag->SetFragmentHtml(Text(data, "html"));
      if (!Text(data, "link").empty()) drag->SetLinkURL(Text(data, "link"));
      id = integration ? integration->DragEnter(document, generation, drag, data->GetInt("x"), data->GetInt("y"),
          data->GetInt("allowed"), data->GetInt("modifiers"), source, capture) : 0;
    } else if (op == "drag-over") {
      id = integration ? integration->DragOver(document, generation, data->GetInt("x"), data->GetInt("y"),
          data->GetInt("allowed"), data->GetInt("modifiers"), source, capture) : 0;
    } else if (op == "drag-drop") {
      id = integration ? integration->DragDrop(document, generation, data->GetInt("x"), data->GetInt("y"),
          data->GetInt("modifiers"), source, capture) : 0;
    } else if (op == "drag-leave") {
      id = integration ? integration->DragLeave(document, generation, source, capture) : 0;
    } else if (op == "exit-fullscreen") {
      id = integration ? integration->ExitFullscreen(document, generation, source, capture) : 0;
    } else if (op == "resize") {
      queued = stream && stream->Configure(data->GetInt("width"), data->GetInt("height"), data->GetDouble("scale"), 30);
    } else return false;
    auto event = CefDictionaryValue::Create(); event->SetString("op", op);
    event->SetString("id", std::to_string(id)); event->SetBool("ok", queued || id != 0); Event("queued", event);
    return true;
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(20), [this] { return closed_; });
  }
 private:
  void ObserveDocument(uint64_t document) {
    std::lock_guard lock(mutex_);
    if (document > document_) document_ = document;
  }
  const bool attach_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool closed_ = false, permissions_ = false, hold_reads_ = false, deny_reads_ = false;
  bool hold_writes_ = false, fail_writes_ = false;
  bool hold_write_data_ = false;
  uint64_t document_ = 0, generation_ = 0, sequence_ = 1;
  SyntheticGuestFiles files_{[](const char* name, CefRefPtr<CefDictionaryValue> value) { Event(name, value); }};
  CefRefPtr<AlloyGuestSession> session_;
  CefRefPtr<AlloyGuestView> view_;
  CefRefPtr<AlloyGuestStream> stream_;
  CefRefPtr<AlloyGuestIntegration> integration_;
  std::map<std::string, std::string> clipboard_;
  std::map<uint64_t, CefRefPtr<AlloyGuestClipboardRequest>> reads_;
  std::map<uint64_t, std::shared_ptr<Write>> writes_;
  std::map<uint64_t, std::function<void()>> write_data_;
  std::map<uint64_t, CefRefPtr<AlloyGuestMenuRequest>> menus_;
  std::map<uint64_t, CefRefPtr<AlloyGuestDragRequest>> drags_;
  std::map<uint64_t, CefRefPtr<AlloyGuestFullscreenRequest>> fullscreen_;
  // Explicit synthetic app retention to exercise the real SDK objects after
  // retirement, not merely absence from the fixture's active-request maps.
  std::map<uint64_t, CefRefPtr<AlloyGuestClipboardRequest>> retired_reads_;
  std::map<uint64_t, CefRefPtr<AlloyGuestMenuRequest>> retired_menus_;
  std::map<uint64_t, CefRefPtr<AlloyGuestDragRequest>> retired_drags_;
  std::map<uint64_t, CefRefPtr<AlloyGuestFullscreenRequest>> retired_fullscreen_;
  IMPLEMENT_REFCOUNTING(Consumer);
};
}  // namespace

int main(int argc, char** argv) {
  const auto* hash = cef_api_hash(CEF_API_VERSION, 0);
  if (!hash || std::string(hash) != CEF_API_HASH_PLATFORM) return 2;
  CefRefPtr<Consumer> consumer = new Consumer(argc < 3 || std::string(argv[2]) != "missing");
  if (!consumer->Start(argc > 1 ? argv[1] : "memory")) return 2;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!consumer->Command(line)) Event("invalid-command");
  }
  consumer->Command("{\"op\":\"close\"}");
  if (consumer->Wait()) return 0;
  consumer->Command("{\"op\":\"terminate\"}");
  return consumer->Wait() ? 0 : 3;
}
