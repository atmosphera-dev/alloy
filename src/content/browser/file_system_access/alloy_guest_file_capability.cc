// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_file_capability.h"

#include <limits>
#include <map>
#include <set>
#include <utility>
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/unguessable_token.h"
#include "base/uuid.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/browser_context.h"
#include "storage/browser/file_system/external_mount_points.h"
#include "storage/browser/file_system/file_system_context.h"
#include "storage/browser/file_system/file_system_url.h"
#include "storage/common/file_system/file_system_mount_option.h"

namespace content {
namespace {
const int kRegistry = 0;
bool Number(const base::DictValue& value, const char* name, uint64_t* result) {
  const auto* text = value.FindString(name);
  return text && base::StringToUint64(*text, result);
}
bool ValidEntriesReply(const base::DictValue& details, const std::vector<std::string>& bytes) {
  const auto* status = details.FindString("status");
  if (!status || *status != "ok" || bytes.size() != 1) return false;
  auto body = base::JSONReader::ReadDict(bytes[0], base::JSON_PARSE_RFC);
  const auto* entries = body ? body->FindList("entries") : nullptr;
  if (!entries) return false;
  for (const auto& entry : *entries)
    if (!entry.is_dict() || !AlloyGuestFileMetadata::Parse(entry.GetDict())) return false;
  return true;
}
}  // namespace

class AlloyGuestFileRegistry final : public base::SupportsUserData::Data {
 public:
  static AlloyGuestFileRegistry* Existing(BrowserContext* context) {
    return context && !context->ShutdownStarted()
        ? static_cast<AlloyGuestFileRegistry*>(context->GetUserData(&kRegistry)) : nullptr;
  }
  static AlloyGuestFileRegistry* Get(BrowserContext* context) {
    if (!context || context->ShutdownStarted()) return nullptr;
    auto* registry = Existing(context);
    if (!registry) {
      auto created = std::make_unique<AlloyGuestFileRegistry>(); registry = created.get();
      context->SetUserData(&kRegistry, std::move(created));
    }
    return registry;
  }
  void Add(const base::FilePath& path, base::WeakPtr<AlloyGuestFileCapability> entry) {
    entries_.insert_or_assign(path, std::move(entry));
  }
  void Remove(const base::FilePath& path) {
    entries_.erase(path); pins_.erase(path);
  }
  void Pin(const base::FilePath& path, scoped_refptr<AlloyGuestFileCapability> cap) {
    pins_.insert_or_assign(path, std::move(cap));
  }
  void Unpin(const base::FilePath& path) { pins_.erase(path); }
  std::string UniqueId(const std::string& identity) {
    auto [found, inserted] = identities_.try_emplace(identity, base::Uuid::GenerateRandomV4());
    return found->second.AsLowercaseString();
  }
  bool RegisterIdentity(const AlloyGuestFileMetadata& metadata) {
    const auto identity = std::make_pair(metadata.ancestors, metadata.directory);
    std::vector<std::string> prefix;
    for (const auto& parent : metadata.ancestors) {
      auto found = lineages_.find(parent);
      if (found != lineages_.end() && found->second != std::make_pair(prefix, true)) return false;
      prefix.push_back(parent);
    }
    auto [found, added] = lineages_.try_emplace(metadata.identity, identity);
    if (found->second != identity) return false;
    prefix.clear();
    for (const auto& parent : metadata.ancestors) {
      lineages_.try_emplace(parent, std::make_pair(prefix, true)); prefix.push_back(parent);
    }
    return true;
  }
  scoped_refptr<AlloyGuestFileCapability> Find(const base::FilePath& path) {
    auto found = entries_.find(path);
    return found != entries_.end() && found->second
        ? base::WrapRefCounted(found->second.get()) : nullptr;
  }
  base::WeakPtr<AlloyGuestFileRegistry> GetWeakPtr() { return weak_.GetWeakPtr(); }
 private:
  std::map<base::FilePath, base::WeakPtr<AlloyGuestFileCapability>> entries_;
  // Only the chooser's asynchronous native grant phase needs a temporary pin.
  // Actual handles/tokens/blobs retain capabilities through native shared state.
  std::map<base::FilePath, scoped_refptr<AlloyGuestFileCapability>> pins_;
  std::map<std::string, base::Uuid> identities_;
  std::map<std::string, std::pair<std::vector<std::string>, bool>> lineages_;
  base::WeakPtrFactory<AlloyGuestFileRegistry> weak_{this};
};

bool AlloyGuestFileCapability::ValidName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
      name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos;
}
std::optional<AlloyGuestFileMetadata> AlloyGuestFileMetadata::Parse(const base::DictValue& value) {
  const auto* token = value.FindString("capability");
  const auto* identity = value.FindString("identity");
  const auto* name = value.FindString("name");
  const auto* mime = value.FindString("mime");
  const auto* modified = value.FindString("modified-ms");
  const auto* ancestors = value.FindList("ancestors");
  const auto directory = value.FindBool("directory"), readable = value.FindBool("readable"),
             writable = value.FindBool("writable");
  AlloyGuestFileMetadata result;
  if (!token || !identity || !name || !mime || !modified || !ancestors || !directory || !readable || !writable ||
      !base::UnguessableToken::DeserializeFromString(*token) ||
      !base::UnguessableToken::DeserializeFromString(*identity) ||
      !AlloyGuestFileCapability::ValidName(*name) || mime->find('\0') != std::string::npos ||
      !Number(value, "size", &result.size) ||
      result.size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      !base::StringToInt64(*modified, &result.modified_ms)) return std::nullopt;
  result.capability = *token; result.identity = *identity; result.name = *name; result.mime = *mime;
  result.directory = *directory; result.readable = *readable; result.writable = *writable;
  if (result.directory && result.size) return std::nullopt;
  std::set<std::string> seen{result.identity};
  for (const auto& ancestor : *ancestors) {
    if (!ancestor.is_string() || !base::UnguessableToken::DeserializeFromString(ancestor.GetString()) ||
        !seen.insert(ancestor.GetString()).second) return std::nullopt;
    result.ancestors.push_back(ancestor.GetString());
  }
  return result;
}
base::File::Info AlloyGuestFileMetadata::FileInfo() const {
  base::File::Info result;
  result.size = static_cast<int64_t>(size); result.is_directory = directory;
  result.last_modified = base::Time::FromMillisecondsSinceUnixEpoch(modified_ms);
  return result;
}
AlloyGuestFileCapability::AlloyGuestFileCapability(RenderFrameHost* original,
    AlloyGuestFileMetadata metadata)
    : base::RefCountedDeleteOnSequence<AlloyGuestFileCapability>(
          base::SequencedTaskRunner::GetCurrentDefault()), metadata_(std::move(metadata)),
      document_(original->GetWeakDocumentPtr()),
      contents_(WebContents::FromRenderFrameHost(original)->GetOutermostWebContents()->GetWeakPtr()) {}
AlloyGuestFileCapability::~AlloyGuestFileCapability() { ReleaseRegistration(); }

scoped_refptr<AlloyGuestFileCapability> AlloyGuestFileCapability::Create(
    RenderFrameHost* original, AlloyGuestFileMetadata metadata, base::RepeatingCallback<bool()> extra) {
  auto* bridge = AlloyGuestIntegration::Get(original);
  if (!original || !bridge) return nullptr;
  auto cap = base::AdoptRef(new AlloyGuestFileCapability(original, std::move(metadata)));
  auto* registry = AlloyGuestFileRegistry::Get(original->GetBrowserContext());
  if (!registry || !registry->RegisterIdentity(cap->metadata_)) { cap->Retire(); return nullptr; }
  cap->registry_ = registry->GetWeakPtr();
  cap->extra_ = std::move(extra);
  cap->admitted_ = bridge->CaptureLifetime(original, base::BindOnce(
      [](base::WeakPtr<AlloyGuestFileCapability> weak) {
        if (weak) { auto retained = base::WrapRefCounted(weak.get()); retained->Retire(); }
      }, cap->weak_.GetWeakPtr()));
  if (!cap->Admitted()) { cap->Retire(); return nullptr; }
  return cap;
}
bool AlloyGuestFileCapability::Admitted() const {
  return live_ && admitted_ && admitted_.Run() && (!extra_ || extra_.Run());
}
void AlloyGuestFileCapability::ReleaseRegistration() {
  if (released_) return;
  released_ = true;
  if (registry_) for (const auto& path : registered_paths_) registry_->Remove(path);
  if (!mount_.empty()) storage::ExternalMountPoints::GetSystemInstance()->RevokeFileSystem(mount_);
  if (auto* bridge = AlloyGuestIntegration::Get(contents_.get())) bridge->RetireFile(metadata_.capability);
}
void AlloyGuestFileCapability::Retire() {
  if (!live_) return;
  live_ = false;
  ReleaseRegistration();
  retired_.Notify();
}
base::CallbackListSubscription AlloyGuestFileCapability::OnRetired(base::OnceClosure callback) {
  if (!Admitted()) { std::move(callback).Run(); return {}; }
  return retired_.Add(std::move(callback));
}
std::optional<base::DictValue> AlloyGuestFileCapability::Body(const Result& result) {
  const auto* status = result ? result->details.FindString("status") : nullptr;
  if (!status || *status != "ok" || result->bytes.size() != 1) return std::nullopt;
  return base::JSONReader::ReadDict(result->bytes[0], base::JSON_PARSE_RFC);
}
std::optional<AlloyGuestFileCapability::Entries> AlloyGuestFileCapability::DecodeEntries(
    RenderFrameHost* original, const Result& result, base::RepeatingCallback<bool()> extra) {
  auto body = Body(result);
  const auto* entries = body ? body->FindList("entries") : nullptr;
  if (!entries) return std::nullopt;
  Entries output;
  for (const auto& entry : *entries) {
    auto metadata = entry.is_dict() ? AlloyGuestFileMetadata::Parse(entry.GetDict()) : std::nullopt;
    if (!metadata) return std::nullopt;
    auto cap = Create(original, std::move(*metadata), extra);
    if (!cap) return std::nullopt;
    output.push_back(std::move(cap));
  }
  return output;
}
void AlloyGuestFileCapability::Select(RenderFrameHost* original, base::DictValue details,
    EntriesReply reply, base::RepeatingCallback<bool()> extra) {
  auto* bridge = AlloyGuestIntegration::Get(original);
  if (!original || !bridge || (extra && !extra.Run())) { std::move(reply).Run(std::nullopt); return; }
  const auto document = original->GetWeakDocumentPtr();
  AlloyGuestIntegrationData data; data.details = std::move(details); data.admitted = extra;
  data.validate_reply = base::BindRepeating(&ValidEntriesReply);
  const auto* mode = data.details.FindString("mode");
  const auto kind = mode && *mode == "download" ? AlloyGuestIntegration::Operation::kDownload
                                               : AlloyGuestIntegration::Operation::kSelectFiles;
  bridge->Request(original, kind, std::move(data), base::BindOnce(
      [](WeakDocumentPtr document, base::RepeatingCallback<bool()> extra,
          EntriesReply reply, Result result) {
        auto* original = document.AsRenderFrameHostIfValid();
        // A chooser's visibility/callback lease ends with the presentation.
        // Selected objects retain their document lease, not that UI object.
        std::move(reply).Run(original && (!extra || extra.Run())
            ? DecodeEntries(original, result) : std::nullopt);
      }, document, std::move(extra), std::move(reply)));
}
void AlloyGuestFileCapability::Call(std::string operation, base::DictValue details,
    std::vector<std::string> bytes, Reply reply, base::RepeatingCallback<bool()> extra) {
  auto* bridge = AlloyGuestIntegration::Get(original());
  if (!Admitted() || !bridge || (extra && !extra.Run())) { std::move(reply).Run(std::nullopt); return; }
  AlloyGuestIntegration::Operation kind = AlloyGuestIntegration::Operation::kFileRead;
  if (operation == "truncate") kind = AlloyGuestIntegration::Operation::kFileTruncate;
  else if (operation == "close" || operation == "abort") kind = AlloyGuestIntegration::Operation::kFileClose;
  else if (operation == "write" || operation == "open-write" || operation == "remove" || operation == "child-entry" ||
           operation == "rename" || operation == "move" ||
           (operation == "permission" && details.FindBool("write").value_or(false)) ||
           details.FindBool("create").value_or(false))
    kind = AlloyGuestIntegration::Operation::kFileWrite;
  const bool writes = kind == AlloyGuestIntegration::Operation::kFileWrite ||
      kind == AlloyGuestIntegration::Operation::kFileTruncate ||
      kind == AlloyGuestIntegration::Operation::kFileClose;
  const bool reads = operation == "read" || operation == "entries" ||
      operation == "child-file" || operation == "child-directory" ||
      (operation == "permission" && !details.FindBool("write").value_or(false));
  if ((writes && !metadata_.writable) || (reads && !metadata_.readable)) {
    std::move(reply).Run(std::nullopt); return;
  }
  details.Set("interaction", operation == "open-write" || operation == "close" ||
      operation == "abort" || operation == "remove" || operation == "rename" || operation == "move" ||
      operation == "truncate" || operation == "permission");
  details.Set("operation", std::move(operation)); details.Set("capability", metadata_.capability);
  AlloyGuestIntegrationData data; data.details = std::move(details); data.bytes = std::move(bytes);
  const auto* requested = data.details.FindString("operation");
  if (requested && (*requested == "entries" || *requested == "child-entry" || *requested == "child-file" ||
                    *requested == "child-directory" || *requested == "open-write")) {
    data.validate_reply = base::BindRepeating([](const base::DictValue& details,
        const std::vector<std::string>& bytes) {
      const auto* status = details.FindString("status");
      return status && (*status == "ok" ? ValidEntriesReply(details, bytes) : bytes.empty());
    });
  }
  data.admitted = base::BindRepeating(
      [](base::WeakPtr<AlloyGuestFileCapability> cap, base::RepeatingCallback<bool()> extra) {
        return cap && cap->Admitted() && (!extra || extra.Run());
      }, weak_.GetWeakPtr(), extra);
  bridge->Request(original(), kind, std::move(data), base::BindOnce(
      [](scoped_refptr<AlloyGuestFileCapability> cap, base::RepeatingCallback<bool()> extra,
          Reply reply, Result result) {
        if (!cap->Admitted() || (extra && !extra.Run())) result.reset();
        std::move(reply).Run(std::move(result));
      }, base::WrapRefCounted(this), std::move(extra), std::move(reply)));
}
void AlloyGuestFileCapability::Stat(Reply reply, base::RepeatingCallback<bool()> extra) {
  Call("stat", {}, {}, base::BindOnce(
      [](scoped_refptr<AlloyGuestFileCapability> cap, Reply reply, Result result) {
        auto body = Body(result);
        if (body) {
          uint64_t size = 0; int64_t modified = 0;
          const auto* time = body->FindString("modified-ms");
          if (Number(*body, "size", &size) && size <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
              time && base::StringToInt64(*time, &modified)) {
            cap->metadata_.size = size; cap->metadata_.modified_ms = modified;
          } else { result.reset(); }
        }
        std::move(reply).Run(std::move(result));
      }, base::WrapRefCounted(this), std::move(reply)), std::move(extra));
}
void AlloyGuestFileCapability::Read(uint64_t offset, uint64_t length, int64_t expected_modified_ms,
    Reply reply, base::RepeatingCallback<bool()> extra) {
  if (!metadata_.readable || metadata_.directory || length > kChunk ||
      offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
      length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - offset) {
    std::move(reply).Run(std::nullopt); return;
  }
  base::DictValue details; details.Set("offset", std::to_string(offset));
  details.Set("length", std::to_string(length));
  details.Set("expected-modified-ms", std::to_string(expected_modified_ms));
  Call("read", std::move(details), {}, std::move(reply), std::move(extra));
}
std::optional<AlloyGuestFileCapability::Entries> AlloyGuestFileCapability::Children(const Result& result) {
  return Admitted() ? DecodeEntries(original(), result, extra_) : std::nullopt;
}
bool AlloyGuestFileCapability::RegisterURL(BrowserContext* context, storage::FileSystemContext* file_system) {
  if (!Admitted()) return false;
  if (!mount_.empty()) return true;
  auto* registry = AlloyGuestFileRegistry::Get(context);
  if (!registry) return false;
  const auto identity = base::UnguessableToken::Create().ToString();
  mount_ = "alloy-file-" + identity;
  const auto root = base::FilePath(FILE_PATH_LITERAL("/alloy-guest-files")).AppendASCII(identity);
  if (!storage::ExternalMountPoints::GetSystemInstance()->RegisterFileSystem(
          mount_, storage::kFileSystemTypeLocal, storage::FileSystemMountOption(), root)) {
    mount_.clear(); return false;
  }
  virtual_path_ = base::FilePath::FromUTF8Unsafe(mount_).Append(base::FilePath::FromUTF8Unsafe(metadata_.name));
  const auto url = file_system->CreateCrackedFileSystemURL(
      blink::StorageKey(), storage::kFileSystemTypeExternal, virtual_path_);
  if (!url.is_valid()) {
    storage::ExternalMountPoints::GetSystemInstance()->RevokeFileSystem(mount_);
    mount_.clear(); return false;
  }
  return RegisterAlias(context, url.path());
}
bool AlloyGuestFileCapability::RegisterAlias(BrowserContext* context, const base::FilePath& path) {
  if (!Admitted()) return false;
  auto* registry = AlloyGuestFileRegistry::Get(context);
  if (!registry) return false;
  registry_ = registry->GetWeakPtr(); registered_paths_.push_back(path);
  registry->Add(path, weak_.GetWeakPtr());
  return true;
}
void AlloyGuestFileCapability::PinSelection() {
  if (registry_ && !registered_paths_.empty())
    registry_->Pin(registered_paths_.front(), base::WrapRefCounted(this));
}
void AlloyGuestFileCapability::UnpinSelection() {
  if (registry_ && !registered_paths_.empty()) registry_->Unpin(registered_paths_.front());
}
storage::FileSystemURL AlloyGuestFileCapability::IdentityURL(storage::FileSystemContext* context) const {
  // Lock/unique-ID identity is separate from the per-selection authority URL.
  // No filesystem operation is ever issued against this synthetic local URL.
  auto path = base::FilePath(FILE_PATH_LITERAL("/alloy-guest-entry-identities"));
  for (const auto& ancestor : metadata_.ancestors) path = path.AppendASCII(ancestor);
  path = path.AppendASCII(metadata_.identity);
  return context->CreateCrackedFileSystemURL(blink::StorageKey(), storage::kFileSystemTypeLocal, path);
}
std::string AlloyGuestFileCapability::UniqueId() const {
  return Admitted() && registry_ ? registry_->UniqueId(metadata_.identity) : std::string();
}
scoped_refptr<AlloyGuestFileCapability> AlloyGuestFileCapability::FromURL(
    BrowserContext* context, const storage::FileSystemURL& url) {
  auto* registry = AlloyGuestFileRegistry::Existing(context);
  return registry ? registry->Find(url.path()) : nullptr;
}

void AlloyGuestIntegration::RequestFilePermission(RenderFrameHost* original,
    const base::FilePath& path, bool external, bool write, base::OnceCallback<void(bool)> callback) {
  if (!original || !original->IsActive()) { std::move(callback).Run(false); return; }
  auto* file_system = original->GetStoragePartition()->GetFileSystemContext();
  const auto url = file_system->CreateCrackedFileSystemURL(blink::StorageKey(),
      external ? storage::kFileSystemTypeExternal : storage::kFileSystemTypeLocal, path);
  auto cap = AlloyGuestFileCapability::FromURL(original->GetBrowserContext(), url);
  if (!cap || !cap->Admitted()) { std::move(callback).Run(false); return; }
  base::DictValue details; details.Set("write", write);
  cap->Call("permission", std::move(details), {}, base::BindOnce(
      [](base::OnceCallback<void(bool)> callback, AlloyGuestFileCapability::Result result) {
        auto body = AlloyGuestFileCapability::Body(result);
        std::move(callback).Run(body && body->FindBool("related").value_or(false));
      }, std::move(callback)));
}
}  // namespace content
