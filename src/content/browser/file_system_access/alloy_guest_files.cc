// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_files.h"
#include "content/browser/file_system_access/alloy_guest_file_binding.h"

#include <set>
#include <utility>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/process/alloy_guest_runtime.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "content/browser/file_system_access/alloy_guest_file_streams.h"
#include "content/browser/file_system_access/alloy_guest_file_writer.h"
#include "content/browser/file_system_access/file_system_access_error.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/mime_util.h"
#include "storage/browser/file_system/file_system_context.h"
#include "third_party/blink/public/mojom/blob/serialized_blob.mojom.h"

namespace content {
namespace {
using Cap = AlloyGuestFileCapability;
using Manager = FileSystemAccessManagerImpl;
using State = Manager::SharedHandleState;
using Context = Manager::BindingContext;
using Status = blink::mojom::FileSystemAccessStatus;
using Permission = blink::mojom::PermissionStatus;
using Error = blink::mojom::FileSystemAccessErrorPtr;
using Lock = FileSystemAccessLockManager::LockHandle;
using Mode = blink::mojom::FileSystemAccessPermissionMode;
using Done = base::OnceCallback<void(Error)>;
Error Failed(Status status = Status::kOperationAborted) {
  return file_system_access_error::FromStatus(status);
}
bool DirectChild(const Cap& parent, const Cap& child) {
  auto expected = parent.metadata().ancestors;
  expected.push_back(parent.metadata().identity);
  return child.metadata().ancestors == expected;
}
scoped_refptr<Cap> Lookup(const Context& context, const storage::FileSystemURL& url) {
  auto* frame = context.document.AsRenderFrameHostIfValid();
  return frame ? Cap::FromURL(frame->GetBrowserContext(), url) : nullptr;
}
State WithCapability(const Context& context, const storage::FileSystemURL& url, const State& state) {
  State result(state);
  // A child inherits native grants, not its parent's selected-object identity.
  auto registered = Lookup(context, url);
  if (registered) result.guest_file = std::move(registered);
  if (result.guest_file) {
    auto recipient = AlloyGuestFileBinding::Create(context.document.AsRenderFrameHostIfValid(),
        result.guest_file, result.guest_binding);
    if (recipient) result.guest_binding = std::move(recipient);
  }
  return result;
}

// Shared native authorization and capability ownership for file and directory
// adapters. Native grants remain authoritative; this only replaces their I/O.
template <class Native>
class GuestHandle : public Native {
 public:
  template <typename... Args>
  GuestHandle(const Context& context, const storage::FileSystemURL& url,
      const State& state, Args&&... args)
      : Native(std::forward<Args>(args)..., WithCapability(context, url, state)),
        cap_(this->handle_state().guest_file),
        receiver_binding_(this->handle_state().guest_binding) {}
  bool OwnsContext() const {
    return cap_ && cap_->Admitted() && receiver_binding_ && receiver_binding_->Admitted() &&
        this->manager()->IsBindingContextValid(this->context());
  }
  void GetPermissionStatus(Mode mode, typename Native::GetPermissionStatusCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Permission::DENIED); return; }
    Native::GetPermissionStatus(mode, std::move(callback));
  }
  void RequestPermission(Mode mode, typename Native::RequestPermissionCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Failed(), Permission::DENIED); return; }
    Native::RequestPermission(mode, std::move(callback));
  }
  void Transfer(mojo::PendingReceiver<blink::mojom::FileSystemAccessTransferToken> receiver) override {
    if (OwnsContext()) Native::Transfer(std::move(receiver));
  }
  void Move(mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> destination,
      const std::string& name, typename Native::MoveCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Failed()); return; }
    Native::Move(std::move(destination), name, std::move(callback));
  }
  void Rename(const std::string& name, typename Native::RenameCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Failed()); return; }
    Native::Rename(name, std::move(callback));
  }
  base::RepeatingCallback<bool()> PermissionCheck(bool write) {
    return base::BindRepeating([](base::WeakPtr<Manager> manager, Context context,
        State state, scoped_refptr<AlloyGuestFileBinding> receiver, bool write) {
      if (!manager || !manager->IsBindingContextValid(context) || !state.guest_file ||
          !state.guest_file->Admitted() || !receiver || !receiver->Admitted()) return false;
      if (write) {
        if (context.storage_key.IsThirdPartyContext() || !state.guest_file->metadata().writable ||
            state.write_grant->GetStatus() != Permission::GRANTED) return false;
        return Manager::GetEffectiveWritePermissionMode() == Mode::kWrite ||
            state.read_grant->GetStatus() == Permission::GRANTED;
      }
      return state.guest_file->metadata().readable && state.read_grant->GetStatus() == Permission::GRANTED;
    }, this->manager()->AsWeakPtr(), this->context(), this->handle_state(), receiver_binding_, write);
  }
  void RemoveAllowed(bool recursive, std::string name, Done callback) {
    if (!PermissionCheck(true).Run()) { std::move(callback).Run(Failed(Status::kPermissionDenied)); return; }
    if (!name.empty()) {
      base::DictValue details; details.Set("name", name);
      cap_->Call("child-entry", std::move(details), {}, base::BindOnce(
          [](base::WeakPtr<GuestHandle> self, std::string name, bool recursive, Done callback, Cap::Result result) {
            if (!self) return;
            auto children = self->cap_->Children(result);
            if (!children || children->size() != 1 || (*children)[0]->metadata().name != name ||
                !DirectChild(*self->cap_, *(*children)[0])) {
              auto error = AlloyGuestFileError(result);
              std::move(callback).Run(error->status == Status::kOk ? Failed() : std::move(error)); return;
            }
            self->RemoveCap((*children)[0], recursive, false, std::move(callback));
          }, weak_.GetWeakPtr(), name, recursive, std::move(callback)), PermissionCheck(true));
      return;
    }
    RemoveCap(cap_, recursive, true, std::move(callback));
  }
  void RemoveCap(scoped_refptr<Cap> target, bool recursive, bool own, Done callback) {
    this->manager()->TakeLock(this->context(), target->IdentityURL(this->file_system_context()),
        this->manager()->GetExclusiveLockType(), base::BindOnce(
        [](base::WeakPtr<GuestHandle> self, scoped_refptr<Cap> target, bool recursive, bool own, Done callback,
            scoped_refptr<Lock> lock) {
          if (!self) return;
          if (!lock) { std::move(callback).Run(Failed(Status::kNoModificationAllowedError)); return; }
          base::DictValue details; details.Set("recursive", recursive);
          target->Call("remove", std::move(details), {}, base::BindOnce(
              [](base::WeakPtr<GuestHandle> self, scoped_refptr<Lock> lock, bool own,
                  Done callback, Cap::Result result) {
                auto error = AlloyGuestFileError(result);
                if (self && error->status == Status::kOk) {
                  if (own) {
                    self->MaybeNotifyEntryModified(self->url());
                    self->cap_->Retire();
                  }
                }
                std::move(callback).Run(std::move(error));
              }, self, std::move(lock), own, std::move(callback)), self->PermissionCheck(true));
        }, weak_.GetWeakPtr(), target, recursive, own, std::move(callback)));
  }
  void Unique(base::OnceCallback<void(Error, const std::string&)> callback) {
    if (!OwnsContext()) { std::move(callback).Run(Failed(), {}); return; }
    const auto id = cap_->UniqueId();
    std::move(callback).Run(id.empty() ? Failed() : file_system_access_error::Ok(), id);
  }
 protected:
  base::WeakPtr<FileSystemAccessHandleBase> AsWeakPtr() override { return weak_.GetWeakPtr(); }
  scoped_refptr<Cap> cap_;
  scoped_refptr<AlloyGuestFileBinding> receiver_binding_;
  base::WeakPtrFactory<GuestHandle> weak_{this};
};

class GuestFile final : public GuestHandle<FileSystemAccessFileHandleImpl> {
 public:
  GuestFile(Manager* manager, const Context& context, const storage::FileSystemURL& url,
      const std::string& name, const State& state)
      : GuestHandle(context, url, state, manager, context, url, name) {}
  void AsBlob(AsBlobCallback callback) override {
    auto check = CheckReadAccess();
    if (!check.has_value()) { std::move(callback).Run(std::move(check.error()), {}, nullptr); return; }
    if (!cap_ || !PermissionCheck(false).Run()) { std::move(callback).Run(Failed(), {}, nullptr); return; }
    cap_->Stat(base::BindOnce([](base::WeakPtr<GuestFile> self, AsBlobCallback callback, Cap::Result result) {
      if (!self) return;
      auto error = AlloyGuestFileError(result);
      if (error->status != Status::kOk || !Cap::Body(result)) {
        std::move(callback).Run(error->status == Status::kOk ? Failed() : std::move(error), {}, nullptr); return;
      }
      const auto metadata = self->cap_->metadata();
      auto blob = blink::mojom::SerializedBlob::New();
      blob->uuid = base::Uuid::GenerateRandomV4().AsLowercaseString();
      blob->content_type = metadata.mime;
      if (blob->content_type.empty()) net::GetWellKnownMimeTypeFromFile(
          base::FilePath::FromUTF8Unsafe(metadata.name), &blob->content_type);
      blob->size = metadata.size;
      CreateAlloyGuestFileBlob(base::WrapRefCounted(self->manager()->blob_context()), self->cap_,
          metadata.size, metadata.modified_ms, blob->uuid, blob->content_type,
          blob->blob.InitWithNewPipeAndPassReceiver(), self->PermissionCheck(false), self->receiver_binding_);
      std::move(callback).Run(file_system_access_error::Ok(), metadata.FileInfo(), std::move(blob));
    }, file_weak_.GetWeakPtr(), std::move(callback)), PermissionCheck(false));
  }
  void CreateFileWriter(bool keep, bool auto_close,
      blink::mojom::FileSystemAccessWritableFileStreamLockMode mode,
      CreateFileWriterCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Failed(), mojo::NullRemote()); return; }
    // Keep the native permission continuation, then take its ordinary writer
    // lock without creating a local swap file.
    RunWithPermission(Manager::GetEffectiveWritePermissionMode(),
        base::BindOnce(&GuestFile::WriterAllowed, file_weak_.GetWeakPtr(), keep, auto_close, mode),
        base::BindOnce([](Error error, CreateFileWriterCallback callback) {
          std::move(callback).Run(std::move(error), mojo::NullRemote());
        }), std::move(callback));
  }
  void Remove(RemoveCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Failed()); return; }
    RunWithPermission(Manager::GetEffectiveWritePermissionMode(),
        base::BindOnce([](base::WeakPtr<GuestFile> self, RemoveCallback callback) {
          if (self) self->RemoveAllowed(false, {}, std::move(callback));
        }, file_weak_.GetWeakPtr()),
        base::BindOnce([](Error error, RemoveCallback callback) { std::move(callback).Run(std::move(error)); }),
        std::move(callback));
  }
  void IsSameEntry(mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> token,
      IsSameEntryCallback callback) override {
    if (!OwnsContext()) { std::move(callback).Run(Failed(), false); return; }
    manager()->ResolveTransferToken(std::move(token), base::BindOnce(
        [](base::WeakPtr<GuestFile> self, IsSameEntryCallback callback, FileSystemAccessTransferTokenImpl* token) {
          if (!self) return;
          if (!self->OwnsContext()) { std::move(callback).Run(Failed(), false); return; }
          auto other = token && token->guest_binding() && token->guest_binding()->Admitted() &&
              token->origin() == self->context().storage_key.origin()
              ? Lookup(self->context(), token->url()) : nullptr;
          std::move(callback).Run(file_system_access_error::Ok(), other && other->Admitted() &&
              !other->metadata().directory && other->metadata().identity == self->cap_->metadata().identity);
        }, file_weak_.GetWeakPtr(), std::move(callback)));
  }
  void GetUniqueId(GetUniqueIdCallback callback) override { Unique(std::move(callback)); }
  void GetCloudIdentifiers(GetCloudIdentifiersCallback callback) override {
    std::move(callback).Run(OwnsContext() ? file_system_access_error::Ok() : Failed(), {});
  }
 private:
  void WriterAllowed(bool keep, bool auto_close, blink::mojom::FileSystemAccessWritableFileStreamLockMode mode,
      CreateFileWriterCallback callback) {
    if (!cap_ || !PermissionCheck(true).Run()) { std::move(callback).Run(Failed(), mojo::NullRemote()); return; }
    auto type = mode == blink::mojom::FileSystemAccessWritableFileStreamLockMode::kExclusive
        ? manager()->GetExclusiveLockType() : manager()->GetWFSSiloedLockType();
    manager()->TakeLock(context(), cap_->IdentityURL(file_system_context()), type, base::BindOnce(
        [](base::WeakPtr<GuestFile> self, bool keep, bool auto_close, bool exclusive,
            CreateFileWriterCallback callback, scoped_refptr<Lock> lock) {
          if (!self) return;
          if (!lock) { std::move(callback).Run(Failed(Status::kNoModificationAllowedError), mojo::NullRemote()); return; }
          base::DictValue details; details.Set("keep-existing", keep); details.Set("exclusive", exclusive);
          self->cap_->Call("open-write", std::move(details), {}, base::BindOnce(
              [](base::WeakPtr<GuestFile> self, bool auto_close, scoped_refptr<Lock> lock,
                  CreateFileWriterCallback callback, Cap::Result result) {
                if (!self) return;
                auto entries = self->cap_->Children(result);
                if (!entries || entries->size() != 1 || (*entries)[0]->metadata().directory ||
                    !(*entries)[0]->metadata().writable) {
                  auto error = AlloyGuestFileError(result);
                  std::move(callback).Run(error->status == Status::kOk ? Failed() : std::move(error), mojo::NullRemote());
                  return;
                }
                auto writer = CreateAlloyGuestFileWriter((*entries)[0], self->PermissionCheck(true),
                    std::move(lock), auto_close, self->receiver_binding_);
                const bool bound = writer.is_valid();
                std::move(callback).Run(bound ? file_system_access_error::Ok() : Failed(), std::move(writer));
              }, self, auto_close, std::move(lock), std::move(callback)), self->PermissionCheck(true));
        }, file_weak_.GetWeakPtr(), keep, auto_close,
           mode == blink::mojom::FileSystemAccessWritableFileStreamLockMode::kExclusive, std::move(callback)));
  }
  base::WeakPtrFactory<GuestFile> file_weak_{this};
};

class GuestDirectory final : public GuestHandle<FileSystemAccessDirectoryHandleImpl> {
 public:
  GuestDirectory(Manager* manager, const Context& context, const storage::FileSystemURL& url, const State& state)
      : GuestHandle(context, url, state, manager, context, url) {}
  void GetFile(const std::string& name, bool create, GetFileCallback callback) override {
    Child(name, false, create, base::BindOnce([](GetFileCallback callback,
        Error error, blink::mojom::FileSystemAccessEntryPtr entry) {
      std::move(callback).Run(std::move(error), entry ? std::move(entry->entry_handle->get_file()) : mojo::NullRemote());
    }, std::move(callback)));
  }
  void GetDirectory(const std::string& name, bool create, GetDirectoryCallback callback) override {
    Child(name, true, create, base::BindOnce([](GetDirectoryCallback callback,
        Error error, blink::mojom::FileSystemAccessEntryPtr entry) {
      std::move(callback).Run(std::move(error), entry ? std::move(entry->entry_handle->get_directory()) : mojo::NullRemote());
    }, std::move(callback)));
  }
  void GetEntries(mojo::PendingRemote<blink::mojom::FileSystemAccessDirectoryEntriesListener> pending) override {
    mojo::Remote<blink::mojom::FileSystemAccessDirectoryEntriesListener> listener(std::move(pending));
    if (!cap_ || !PermissionCheck(false).Run()) { listener->DidReadDirectory(Failed(Status::kPermissionDenied), {}, false); return; }
    cap_->Call("entries", {}, {}, base::BindOnce([](base::WeakPtr<GuestDirectory> self,
        mojo::Remote<blink::mojom::FileSystemAccessDirectoryEntriesListener> listener, Cap::Result result) {
      if (!self) return;
      auto entries = self->cap_->Children(result);
      if (!entries) { listener->DidReadDirectory(AlloyGuestFileError(result), {}, false); return; }
      std::set<std::string> names;
      std::vector<blink::mojom::FileSystemAccessEntryPtr> output;
      for (auto& child : *entries) {
        if (!names.insert(child->metadata().name).second) {
          listener->DidReadDirectory(Failed(Status::kOperationFailed), {}, false); return;
        }
        auto entry = self->Entry(child);
        if (!entry) { listener->DidReadDirectory(Failed(), {}, false); return; }
        output.push_back(std::move(entry));
      }
      listener->DidReadDirectory(file_system_access_error::Ok(), std::move(output), false);
    }, directory_weak_.GetWeakPtr(), std::move(listener)), PermissionCheck(false));
  }
  void Remove(bool recursive, RemoveCallback callback) override { RemoveChecked(recursive, {}, std::move(callback)); }
  void RemoveEntry(const std::string& name, bool recursive, RemoveEntryCallback callback) override {
    if (!SafeName(name)) { std::move(callback).Run(Failed(Status::kInvalidArgument)); return; }
    RemoveChecked(recursive, name, std::move(callback));
  }
  void Resolve(mojo::PendingRemote<blink::mojom::FileSystemAccessTransferToken> token,
      ResolveCallback callback) override {
    if (!cap_ || !PermissionCheck(false).Run()) { std::move(callback).Run(Failed(), std::nullopt); return; }
    manager()->ResolveTransferToken(std::move(token), base::BindOnce(
        [](base::WeakPtr<GuestDirectory> self, ResolveCallback callback, FileSystemAccessTransferTokenImpl* token) {
          if (!self) return;
          auto other = token && token->guest_binding() && token->guest_binding()->Admitted() &&
              token->origin() == self->context().storage_key.origin()
              ? Lookup(self->context(), token->url()) : nullptr;
          if (!other || !other->Admitted()) { std::move(callback).Run(file_system_access_error::Ok(), std::nullopt); return; }
          base::DictValue details; details.Set("destination", other->metadata().capability);
          auto admitted = base::BindRepeating([](base::RepeatingCallback<bool()> self,
              scoped_refptr<AlloyGuestFileBinding> other) { return self.Run() && other->Admitted(); },
              self->PermissionCheck(false), token->guest_binding());
          self->cap_->Call("resolve", std::move(details), {}, base::BindOnce(
              [](ResolveCallback callback, scoped_refptr<Cap> other, Cap::Result result) {
                auto body = Cap::Body(result);
                auto error = AlloyGuestFileError(result);
                const auto* values = body ? body->FindList("path") : nullptr;
                if (!body || !body->FindBool("related").value_or(false)) {
                  std::move(callback).Run(std::move(error), std::nullopt); return;
                }
                std::vector<std::string> path;
                if (!values || !other->Admitted()) { std::move(callback).Run(Failed(), std::nullopt); return; }
                for (const auto& value : *values) {
                  if (!value.is_string() || !Cap::ValidName(value.GetString())) {
                    std::move(callback).Run(Failed(), std::nullopt); return;
                  }
                  path.push_back(value.GetString());
                }
                std::move(callback).Run(file_system_access_error::Ok(), path);
              }, std::move(callback), other), std::move(admitted));
        }, directory_weak_.GetWeakPtr(), std::move(callback)));
  }
  void GetUniqueId(GetUniqueIdCallback callback) override { Unique(std::move(callback)); }
  void GetCloudIdentifiers(GetCloudIdentifiersCallback callback) override {
    std::move(callback).Run(OwnsContext() ? file_system_access_error::Ok() : Failed(), {});
  }
 private:
  using ChildReply = base::OnceCallback<void(Error, blink::mojom::FileSystemAccessEntryPtr)>;
  bool SafeName(const std::string& name) {
    return Cap::ValidName(name) && manager()->IsSafePathComponent(url().type(), name);
  }
  blink::mojom::FileSystemAccessEntryPtr Entry(scoped_refptr<Cap> child) {
    if (!DirectChild(*cap_, *child) || !SafeName(child->metadata().name)) return nullptr;
    auto* frame = context().document.AsRenderFrameHostIfValid();
    if (!frame || !child->RegisterURL(frame->GetBrowserContext(), file_system_context())) return nullptr;
    const auto child_url = file_system_context()->CreateCrackedFileSystemURL(context().storage_key,
        storage::kFileSystemTypeExternal, child->virtual_path());
    State state(handle_state()); state.guest_file = child;
    if (child->metadata().directory) return blink::mojom::FileSystemAccessEntry::New(
        blink::mojom::FileSystemAccessHandle::NewDirectory(manager()->CreateDirectoryHandle(context(), child_url, state)),
        child->metadata().name);
    return blink::mojom::FileSystemAccessEntry::New(blink::mojom::FileSystemAccessHandle::NewFile(
        manager()->CreateFileHandle(context(), child_url, child->metadata().name, state)), child->metadata().name);
  }
  void Child(const std::string& name, bool directory, bool create, ChildReply callback) {
    if (!SafeName(name)) { std::move(callback).Run(Failed(Status::kInvalidArgument), nullptr); return; }
    if (!cap_ || !PermissionCheck(false).Run()) { std::move(callback).Run(Failed(Status::kPermissionDenied), nullptr); return; }
    if (create) {
      RunWithPermission(Manager::GetEffectiveWritePermissionMode(),
          base::BindOnce(&GuestDirectory::ChildAllowed, directory_weak_.GetWeakPtr(), name, directory, create),
          base::BindOnce([](Error error, ChildReply callback) { std::move(callback).Run(std::move(error), nullptr); }),
          std::move(callback));
    } else ChildAllowed(name, directory, create, std::move(callback));
  }
  void ChildAllowed(std::string name, bool directory, bool create, ChildReply callback) {
    base::DictValue details; details.Set("name", name); details.Set("create", create);
    cap_->Call(directory ? "child-directory" : "child-file", std::move(details), {}, base::BindOnce(
        [](base::WeakPtr<GuestDirectory> self, std::string name, bool directory, ChildReply callback, Cap::Result result) {
          if (!self) return;
          auto children = self->cap_->Children(result);
          if (!children || children->size() != 1 || (*children)[0]->metadata().directory != directory ||
              (*children)[0]->metadata().name != name) {
            auto error = AlloyGuestFileError(result);
            std::move(callback).Run(error->status == Status::kOk ? Failed() : std::move(error), nullptr); return;
          }
          auto entry = self->Entry((*children)[0]);
          const bool present = !!entry;
          std::move(callback).Run(present ? file_system_access_error::Ok() : Failed(), std::move(entry));
        }, directory_weak_.GetWeakPtr(), name, directory, std::move(callback)), PermissionCheck(create));
  }
  void RemoveChecked(bool recursive, std::string name, Done callback) {
    if (!OwnsContext()) { std::move(callback).Run(Failed()); return; }
    RunWithPermission(Manager::GetEffectiveWritePermissionMode(),
        base::BindOnce(&GuestDirectory::RemoveAllowed, weak_.GetWeakPtr(), recursive, std::move(name)),
        base::BindOnce([](Error error, Done callback) { std::move(callback).Run(std::move(error)); }), std::move(callback));
  }
  base::WeakPtrFactory<GuestDirectory> directory_weak_{this};
};

class Picker final : public base::RefCounted<Picker> {
 public:
  Picker(RenderFrameHost* frame, FileSystemChooser::Options options,
      FileSystemChooser::ResultCallback callback, FileSystemChooser::ScopedObjects objects,
      std::unique_ptr<WebContentsBasedCanceller> canceller)
      : document_(frame->GetWeakDocumentPtr()), options_(options), callback_(std::move(callback)),
        objects_(std::move(objects)), canceller_(std::move(canceller)) {}
  void Start() {
    self_ = this;
    canceller_->SetCancelCallback(base::BindOnce(&Picker::Cancel, weak_.GetWeakPtr()));
    base::DictValue details;
    std::string mode = "open";
    if (options_.type() == ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE) mode = "open-multiple";
    else if (options_.type() == ui::SelectFileDialog::SELECT_FOLDER) mode = "directory";
    else if (options_.type() == ui::SelectFileDialog::SELECT_SAVEAS_FILE) mode = "save";
    details.Set("mode", mode); details.Set("title", base::UTF16ToUTF8(options_.title()));
    details.Set("suggested-name", options_.default_path().BaseName().AsUTF8Unsafe());
    base::ListValue accept;
    for (const auto& group : options_.file_type_info().extensions)
      for (const auto& extension : group) accept.Append("." + extension);
    details.Set("accept", std::move(accept));
    auto admitted = base::BindRepeating([](base::WeakPtr<Picker> self) { return self && self->callback_; }, weak_.GetWeakPtr());
    Cap::Select(document_.AsRenderFrameHostIfValid(), std::move(details),
        base::BindOnce(&Picker::Selected, weak_.GetWeakPtr()), std::move(admitted));
  }
 private:
  friend class base::RefCounted<Picker>;
  ~Picker() = default;
  void Cancel() { Complete(Failed(), {}); }
  void Selected(std::optional<Cap::Entries> entries) {
    auto* original = document_.AsRenderFrameHostIfValid();
    if (!callback_) return;
    if (!original || !entries || entries->empty() ||
        (options_.type() != ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE && entries->size() != 1)) { Cancel(); return; }
    std::vector<PathInfo> paths;
    auto* context = original->GetStoragePartition()->GetFileSystemContext();
    for (auto& cap : *entries) {
      if (cap->metadata().directory != (options_.type() == ui::SelectFileDialog::SELECT_FOLDER) ||
          !cap->RegisterURL(original->GetBrowserContext(), context)) { Cancel(); return; }
      cap->PinSelection();
      paths.emplace_back(PathType::kExternal, cap->virtual_path(), cap->metadata().name);
    }
    Complete(file_system_access_error::Ok(), std::move(paths));
  }
  void Complete(Error error, std::vector<PathInfo> paths) {
    if (!callback_) return;
    auto retained = base::WrapRefCounted(this);
    auto callback = std::move(callback_);
    canceller_.reset(); self_.reset();
    std::move(callback).Run(std::move(error), std::move(paths));
  }
  WeakDocumentPtr document_;
  const FileSystemChooser::Options options_;
  FileSystemChooser::ResultCallback callback_;
  FileSystemChooser::ScopedObjects objects_;
  std::unique_ptr<WebContentsBasedCanceller> canceller_;
  scoped_refptr<Picker> self_;
  base::WeakPtrFactory<Picker> weak_{this};
};
}  // namespace

bool UsesAlloyGuestExternalFiles(const storage::FileSystemURL& url) {
  return alloy_guest_is_runtime() && url.type() != storage::kFileSystemTypeTemporary &&
      url.type() != storage::kFileSystemTypeTest;
}
std::unique_ptr<FileSystemAccessFileHandleImpl> CreateAlloyGuestFileHandle(Manager* manager,
    const Context& context, const storage::FileSystemURL& url, const std::string& name, const State& state) {
  return std::make_unique<GuestFile>(manager, context, url, name, state);
}
std::unique_ptr<FileSystemAccessDirectoryHandleImpl> CreateAlloyGuestDirectoryHandle(Manager* manager,
    const Context& context, const storage::FileSystemURL& url, const State& state) {
  return std::make_unique<GuestDirectory>(manager, context, url, state);
}
void ShowAlloyGuestFilePicker(RenderFrameHost* original, const FileSystemChooser::Options& options,
    FileSystemChooser::ResultCallback callback, FileSystemChooser::ScopedObjects objects,
    std::unique_ptr<WebContentsBasedCanceller> canceller) {
  base::MakeRefCounted<Picker>(original, options, std::move(callback), std::move(objects), std::move(canceller))->Start();
}
base::ScopedClosureRunner HoldAlloyGuestFileSelection(RenderFrameHost* original,
    storage::FileSystemContext* context, const std::vector<PathInfo>& paths) {
  Cap::Entries entries;
  if (original) for (const auto& path : paths) {
    auto url = context->CreateCrackedFileSystemURL(blink::StorageKey(),
        path.type == PathType::kExternal ? storage::kFileSystemTypeExternal : storage::kFileSystemTypeLocal, path.path);
    auto cap = Cap::FromURL(original->GetBrowserContext(), url);
    if (cap) entries.push_back(std::move(cap));
  }
  return base::ScopedClosureRunner(base::BindOnce([](Cap::Entries entries) {
    for (auto& entry : entries) entry->UnpinSelection();
  }, std::move(entries)));
}
void TruncateAlloyGuestSaveFile(Manager* manager, const Context& context, const storage::FileSystemURL& url,
    base::OnceCallback<void(base::File::Error)> callback) {
  auto cap = Lookup(context, url);
  if (!cap || !cap->Admitted() || !cap->metadata().writable) {
    std::move(callback).Run(base::File::FILE_ERROR_ABORT); return;
  }
  base::DictValue details; details.Set("keep-existing", false); details.Set("exclusive", true);
  cap->Call("open-write", std::move(details), {}, base::BindOnce(
      [](scoped_refptr<Cap> cap, base::OnceCallback<void(base::File::Error)> callback, Cap::Result result) {
        auto entries = cap->Children(result);
        if (!entries || entries->size() != 1 || (*entries)[0]->metadata().directory) {
          std::move(callback).Run(base::File::FILE_ERROR_FAILED); return;
        }
        auto transaction = (*entries)[0];
        transaction->Call("close", {}, {}, base::BindOnce(
            [](scoped_refptr<Cap> transaction, base::OnceCallback<void(base::File::Error)> callback, Cap::Result result) {
              auto error = AlloyGuestFileError(result);
              transaction->Retire();
              std::move(callback).Run(error->status == Status::kOk ? base::File::FILE_OK : error->file_error);
            }, transaction, std::move(callback)));
      }, cap, std::move(callback)));
}
}  // namespace content
