// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/integration/sdk_files.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
namespace {
std::string Hex(const std::string& bytes) {
  const char digits[] = "0123456789abcdef"; std::string result;
  for (unsigned char byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
std::string Unhex(const std::string& value) {
  std::string result;
  if (value.size() % 2) return {};
  for (size_t i = 0; i < value.size(); i += 2) {
    auto digit = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; };
    int a = digit(value[i]), b = digit(value[i + 1]);
    if (a < 0 || b < 0) return {};
    result += static_cast<char>((a << 4) | b);
  }
  return result;
}
struct Node {
  std::string identity = AlloyCreateGuestFileIdentity(), name, bytes;
  bool directory = false, transaction = false, ended = false, removed = false;
  int64_t modified = 1234;
  std::vector<std::string> ancestors;
  std::map<std::string, std::shared_ptr<Node>> children;
  std::shared_ptr<Node> target;
};
}  // namespace
class SyntheticGuestFiles::Impl : public std::enable_shared_from_this<Impl> {
 public:
  explicit Impl(Event event) : event_(std::move(event)) {
    root_ = Make("app-folder", true); source_ = Make("selected.txt", false); source_->bytes = "app-selected-bytes";
    root_->children[source_->name] = source_;
    source_->ancestors = {root_->identity};
    auto nested = Make("nested", true), child = Make("child.txt", false); child->bytes = "nested-app-bytes";
    nested_child_ = child;
    nested->children[child->name] = child; root_->children[nested->name] = nested;
    nested->ancestors = {root_->identity}; child->ancestors = {root_->identity, nested->identity};
    saved_ = Make("saved.txt", false); saved_->bytes = "old-save-data"; downloaded_ = Make("download.bin", false);
  }
  class File final : public AlloyGuestFile {
   public:
    File(std::shared_ptr<Impl> owner, std::shared_ptr<Node> node) : owner_(std::move(owner)), node_(std::move(node)) {}
    CefString GetIdentifier() override { owner_->Getter(); return node_->identity; }
    void GetAncestorIdentifiers(std::vector<CefString>& identifiers) override {
      owner_->Getter(); std::lock_guard lock(owner_->mutex_); identifiers.assign(node_->ancestors.begin(), node_->ancestors.end());
    }
    CefString GetName() override { owner_->Getter(); std::lock_guard lock(owner_->mutex_); return node_->name; }
    bool IsDirectory() override { owner_->Getter(); return node_->directory; }
    bool CanRead() override { owner_->Getter(); return true; }
    bool CanWrite() override { owner_->Getter(); return true; }
    uint64_t GetSize() override { owner_->Getter(); std::lock_guard lock(owner_->mutex_); return node_->bytes.size(); }
    int64_t GetModifiedTimeMs() override { owner_->Getter(); std::lock_guard lock(owner_->mutex_); return node_->modified; }
    CefString GetMimeType() override { owner_->Getter(); return node_->directory ? "" : "application/octet-stream"; }
    void OnOperation(CefRefPtr<AlloyGuestFileOperation> operation) override { owner_->Operation(node_, operation); }
   private:
    const std::shared_ptr<Impl> owner_;
    const std::shared_ptr<Node> node_;
    IMPLEMENT_REFCOUNTING(File);
  };
  class Read final : public AlloyGuestDataHandler {
   public:
    Read(std::shared_ptr<Impl> owner, std::shared_ptr<Node> node, CefRefPtr<AlloyGuestFileOperation> op)
        : owner_(std::move(owner)), node_(std::move(node)), op_(std::move(op)) {}
    void OnRead(bool success, uint64_t offset, CefRefPtr<CefBinaryValue> data) override {
      std::lock_guard lock(owner_->mutex_);
      const auto active = owner_->active_.find(op_->GetId());
      if (active == owner_->active_.end() || active->second.get() != op_.get()) return;
      if (!success || offset || !data || data->GetSize() != op_->GetLength() || node_->ended) { op_->Fail("failed"); return; }
      if (node_->removed || (node_->target && node_->target->removed)) { op_->Fail("not-found"); return; }
      std::string bytes(data->GetSize(), '\0'); data->GetData(bytes.data(), bytes.size(), 0);
      auto start = op_->GetOffset();
      if (start > node_->bytes.max_size() || bytes.size() > node_->bytes.max_size() - start) { op_->Fail("failed"); return; }
      if (node_->bytes.size() < start + bytes.size()) node_->bytes.resize(start + bytes.size());
      node_->bytes.replace(start, bytes.size(), bytes); ++node_->modified; op_->Complete();
    }
   private:
    const std::shared_ptr<Impl> owner_;
    const std::shared_ptr<Node> node_;
    const CefRefPtr<AlloyGuestFileOperation> op_;
    IMPLEMENT_REFCOUNTING(Read);
  };
  std::shared_ptr<Node> Make(std::string name, bool directory) {
    auto node = std::make_shared<Node>(); node->name = std::move(name); node->directory = directory;
    nodes_[node->identity] = node; return node;
  }
  CefRefPtr<AlloyGuestFile> Wrap(std::shared_ptr<Node> node) { return new File(shared_from_this(), std::move(node)); }
  void Select(CefRefPtr<AlloyGuestFileSelection> request) {
    std::lock_guard lock(mutex_);
    callback_thread_ = std::this_thread::get_id();
    std::string mode = request->GetMode();
    auto event = CefDictionaryValue::Create(); event->SetString("mode", mode);
    event->SetString("download", request->GetDownloadId());
    event->SetString("request", std::to_string(request->GetId())); event_("file-selection", event);
    if (mode == "download" && Option("deny-first-download") && !denied_download_) {
      denied_download_ = true; request->Deny(); return;
    }
    if (Option("deny-selection")) { request->Deny(); return; }
    if (Option("hold-selection")) { held_selections_[request->GetId()] = request; return; }
    auto node = source_;
    if (mode == "directory" || mode == "upload-directory") node = root_;
    else if (mode == "save") node = saved_;
    else if (mode == "download") node = downloaded_;
    std::vector<CefRefPtr<AlloyGuestFile>> selected{Wrap(node)};
    if (mode == "upload-multiple" && Option("upload-pair")) selected.push_back(Wrap(nested_child_));
    if (Option("async-metadata")) {
      std::thread([request, selected] { request->Complete(selected, "synthetic-app-files", 1); }).detach();
    } else request->Complete(selected, "synthetic-app-files", 1);
  }
  void Getter() {
    std::lock_guard lock(mutex_);
    if (callback_thread_ != std::this_thread::get_id()) event_("file-wrong-callback-thread", CefDictionaryValue::Create());
  }
  bool Option(const char* name) { return options_ && options_->GetBool(name); }
  void Configure(CefRefPtr<CefDictionaryValue> options) {
    std::lock_guard lock(mutex_); options_ = options->Copy(false);
    if (options->GetType("read-hex") == VTYPE_STRING) { source_->bytes = Unhex(options->GetString("read-hex")); ++source_->modified; }
    auto rename = [this](const std::shared_ptr<Node>& node, const std::string& name) {
      for (const auto& [id, weak] : nodes_) if (auto parent = weak.lock()) {
        for (auto it = parent->children.begin(); it != parent->children.end(); ++it) {
          if (it->second == node) { parent->children.erase(it); parent->children[name] = node; break; }
        }
      }
      node->name = name;
    };
    if (options->GetType("logical-name") == VTYPE_STRING) rename(source_, options->GetString("logical-name"));
    if (Option("same-name")) rename(nested_child_, source_->name);
  }
  void Operation(const std::shared_ptr<Node>& node, CefRefPtr<AlloyGuestFileOperation> op, bool released = false) {
    std::lock_guard lock(mutex_); std::string kind = op->GetKind();
    if (!released) {
      callback_thread_ = std::this_thread::get_id();
      active_[op->GetId()] = op;
    } else {
      const auto active = active_.find(op->GetId());
      if (active == active_.end() || active->second.get() != op.get()) return;
    }
    auto event = CefDictionaryValue::Create(); event->SetString("request", std::to_string(op->GetId()));
    event->SetString("kind", kind); event->SetString("name", node->name);
    event->SetString("offset", std::to_string(op->GetOffset())); event->SetString("length", std::to_string(op->GetLength()));
    event_("file-operation", event);
    if (!released && ((kind == "write" && Option("hold-write")) || (kind == "read" && Option("hold-read")) ||
        (kind == "close" && Option("hold-close")))) { held_[op->GetId()] = {node, op}; return; }
    if (node->ended) { op->Fail("aborted"); return; }
    if (node->removed || (node->target && node->target->removed)) { op->Fail("not-found"); return; }
    if (kind == "permission") { op->CompleteRelation(!Option("deny-permission"), {}); return; }
    if (kind == "stat") {
      if (Option("async-metadata")) std::thread([op] { op->Complete(); }).detach();
      else op->Complete();
      return;
    }
    if (kind == "read") {
      if (op->GetExpectedModifiedTimeMs() != node->modified) { op->Fail("failed"); return; }
      const auto bytes = op->GetOffset() > node->bytes.size() ? "" : node->bytes.substr(op->GetOffset(), op->GetLength());
      op->CompleteRead(bytes.empty() ? nullptr : CefBinaryValue::Create(bytes.data(), bytes.size())); return;
    }
    if (kind == "open-write") {
      auto transaction = Make(node->name, false); transaction->transaction = true; transaction->target = node;
      if (op->GetKeepExistingData()) transaction->bytes = node->bytes;
      op->CompleteEntries({Wrap(transaction)}); return;
    }
    if (kind == "write") {
      auto data = op->GetData();
      if (!node->transaction || !data || !data->Read(0, data->GetSize(), new Read(shared_from_this(), node, op))) op->Fail("failed");
      return;
    }
    if (kind == "truncate") {
      if (!node->transaction || op->GetLength() > node->bytes.max_size()) { op->Fail("failed"); return; }
      node->bytes.resize(op->GetLength()); ++node->modified; op->Complete(); return;
    }
    if (kind == "close") {
      if (!node->transaction || Option("fail-close")) { op->Fail("failed"); return; }
      node->target->bytes = node->bytes; ++node->target->modified;
      auto receipt = CefDictionaryValue::Create(); receipt->SetString("name", node->target->name);
      receipt->SetString("hex", Hex(node->target->bytes)); event_("file-committed", receipt);
      op->Complete(); node->ended = true; return;
    }
    if (kind == "abort") { op->Complete(); node->ended = true; return; }
    if (kind == "entries") {
      if (!node->directory) { op->Fail("not-directory"); return; }
      std::vector<CefRefPtr<AlloyGuestFile>> entries;
      for (const auto& [name, child] : node->children) entries.push_back(Wrap(child));
      op->CompleteEntries(entries); return;
    }
    if (kind == "child-entry" || kind == "child-file" || kind == "child-directory") {
      if (!node->directory) { op->Fail("not-directory"); return; }
      std::string name = op->GetName(); auto found = node->children.find(name);
      if (found == node->children.end() && op->GetCreate()) {
        auto child = Make(name, kind == "child-directory"); child->ancestors = node->ancestors;
        child->ancestors.push_back(node->identity); node->children[name] = child; found = node->children.find(name);
      }
      if (found == node->children.end()) { op->Fail("not-found"); return; }
      if (kind != "child-entry" && found->second->directory != (kind == "child-directory")) { op->Fail(found->second->directory ? "not-file" : "not-directory"); return; }
      op->CompleteEntries({Wrap(found->second)}); return;
    }
    if (kind == "resolve" || kind == "compare") {
      auto other = op->GetDestination(); std::vector<CefString> path; std::set<std::string> seen;
      std::string identity = other ? other->GetIdentifier().ToString() : "";
      bool related = kind == "compare" ? identity == node->identity : FindPath(node, identity, &path, &seen);
      op->CompleteRelation(related, path); return;
    }
    if (kind == "remove") {
      std::string name = op->GetName(); auto removed = node;
      if (!name.empty()) { auto found = node->children.find(name); if (found == node->children.end()) { op->Fail("not-found"); return; } removed = found->second; }
      if (!op->GetRecursive() && !removed->children.empty()) { op->Fail("not-empty"); return; }
      std::vector<std::shared_ptr<Node>> subtree{removed};
      while (!subtree.empty()) {
        auto current = std::move(subtree.back()); subtree.pop_back();
        current->removed = true;
        for (const auto& [child_name, child] : current->children) subtree.push_back(child);
      }
      if (!name.empty()) node->children.erase(name);
      else for (auto& [id, weak] : nodes_) if (auto parent = weak.lock()) {
        for (auto it = parent->children.begin(); it != parent->children.end();) {
          if (it->second == removed) it = parent->children.erase(it); else ++it;
        }
      }
      op->Complete(); return;
    }
    op->Fail("failed");
  }
  bool FindPath(const std::shared_ptr<Node>& node, const std::string& id, std::vector<CefString>* path, std::set<std::string>* seen) {
    if (node->identity == id) return true;
    if (!seen->insert(node->identity).second) return false;
    for (const auto& [name, child] : node->children) { path->push_back(name); if (FindPath(child, id, path, seen)) return true; path->pop_back(); }
    return false;
  }
  void Ended(uint64_t id) {
    std::lock_guard lock(mutex_);
    active_.erase(id); held_.erase(id); held_selections_.erase(id);
  }
  void Release() {
    decltype(held_) held; { std::lock_guard lock(mutex_); held.swap(held_); }
    for (auto& [id, item] : held) Operation(item.first, item.second, true);
  }
  CefRefPtr<CefDictionaryValue> Snapshot() {
    std::lock_guard lock(mutex_); auto value = CefDictionaryValue::Create();
    value->SetString("source", Hex(source_->bytes)); value->SetString("saved", Hex(saved_->bytes));
    value->SetString("downloaded", Hex(downloaded_->bytes)); value->SetInt("held", held_.size());
    uint64_t staged = 0;
    for (const auto& [id, weak] : nodes_) if (auto node = weak.lock())
      if (node->transaction && !node->ended && node->target == downloaded_)
        staged = std::max<uint64_t>(staged, node->bytes.size());
    value->SetString("download-staged-bytes", std::to_string(staged));
    return value;
  }
 private:
  const Event event_;
  std::recursive_mutex mutex_;
  std::shared_ptr<Node> root_, source_, saved_, downloaded_, nested_child_;
  std::map<std::string, std::weak_ptr<Node>> nodes_;
  // Retirement and replay/async byte acceptance linearize on this same map
  // under mutex_. Extracting a held operation never detaches its cancellation.
  std::map<uint64_t, CefRefPtr<AlloyGuestFileOperation>> active_;
  std::map<uint64_t, std::pair<std::shared_ptr<Node>, CefRefPtr<AlloyGuestFileOperation>>> held_;
  std::map<uint64_t, CefRefPtr<AlloyGuestFileSelection>> held_selections_;
  CefRefPtr<CefDictionaryValue> options_;
  std::thread::id callback_thread_;
  bool denied_download_ = false;
};
SyntheticGuestFiles::SyntheticGuestFiles(Event event) : impl_(std::make_shared<Impl>(std::move(event))) {}
SyntheticGuestFiles::~SyntheticGuestFiles() = default;
void SyntheticGuestFiles::Select(CefRefPtr<AlloyGuestFileSelection> request) { impl_->Select(request); }
void SyntheticGuestFiles::Configure(CefRefPtr<CefDictionaryValue> options) { impl_->Configure(options); }
void SyntheticGuestFiles::Ended(uint64_t id) { impl_->Ended(id); }
void SyntheticGuestFiles::Release() { impl_->Release(); }
CefRefPtr<CefDictionaryValue> SyntheticGuestFiles::Snapshot() { return impl_->Snapshot(); }
