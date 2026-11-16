// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_uploads.h"
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/task/bind_post_task.h"
#include "base/unguessable_token.h"
#include "base/uuid.h"
#include "components/file_access/scoped_file_access_delegate.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/browser/file_system_access/file_system_access_manager_impl.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/child_process_id.h"
#include "net/base/mime_util.h"
#include "storage/browser/blob/blob_data_builder.h"
#include "storage/browser/blob/blob_impl.h"
#include "storage/browser/blob/blob_storage_context.h"
#include "storage/browser/blob/shareable_file_reference.h"
#include "storage/browser/file_system/file_system_context.h"

namespace content {
namespace {
using Cap = AlloyGuestFileCapability;
class Snapshot;
const int kSnapshots = 0;
class SnapshotRegistry final : public base::SupportsUserData::Data {
 public:
  static SnapshotRegistry* Existing(WebContents* contents) {
    return static_cast<SnapshotRegistry*>(contents->GetUserData(&kSnapshots));
  }
  static SnapshotRegistry* Get(WebContents* contents) {
    auto* registry = Existing(contents);
    if (!registry) {
      auto value = std::make_unique<SnapshotRegistry>(); registry = value.get();
      contents->SetUserData(&kSnapshots, std::move(value));
    }
    return registry;
  }
  void Add(const base::FilePath& path, base::WeakPtr<Snapshot> snapshot) {
    std::erase_if(entries_, [](const auto& value) { return !value.second; });
    entries_.insert_or_assign(path, std::move(snapshot));
  }
  void Remove(const base::FilePath& path) { entries_.erase(path); }
  base::WeakPtr<Snapshot> Find(const base::FilePath& path) {
    auto found = entries_.find(path); return found == entries_.end() ? nullptr : found->second;
  }
  base::WeakPtr<SnapshotRegistry> GetWeakPtr() { return weak_.GetWeakPtr(); }
 private:
  std::map<base::FilePath, base::WeakPtr<Snapshot>> entries_;
  base::WeakPtrFactory<SnapshotRegistry> weak_{this};
};
// Disk-path consumers need filename aliases. Only this worker owns mutable
// backing and filesystem cleanup; the UI never blocks waiting for app bytes.
class Backing final : public base::RefCountedDeleteOnSequence<Backing> {
 public:
  explicit Backing(scoped_refptr<base::SequencedTaskRunner> worker)
      : base::RefCountedDeleteOnSequence<Backing>(std::move(worker)) {}
  bool Init() {
    const char* candidates[] = {std::getenv("XDG_RUNTIME_DIR"), std::getenv("TMPDIR"), "/dev/shm"};
    for (const char* path : candidates) {
      if (!path || !*path) continue;
      base::ScopedFD parent(open(path, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      struct statfs fs;
      if (!parent.is_valid() || fstatfs(parent.get(), &fs) != 0 || fs.f_type != TMPFS_MAGIC) continue;
      const auto name = "alloy-guest-files-" + base::UnguessableToken::Create().ToString();
      if (mkdirat(parent.get(), name.c_str(), 0700) != 0) continue;
      base::ScopedFD directory(openat(parent.get(), name.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      struct stat info;
      if (!directory.is_valid() || fstat(directory.get(), &info) != 0 || !S_ISDIR(info.st_mode) ||
          info.st_uid != geteuid() || (info.st_mode & 0777) != 0700) {
        unlinkat(parent.get(), name.c_str(), AT_REMOVEDIR); continue;
      }
      // The path-only consumer follows a pinned RAM parent, not a mutable
      // environment pathname. The random owned root is never reused.
      auto alias = base::FilePath("/proc").AppendASCII(std::to_string(getpid())).AppendASCII("fd")
          .AppendASCII(std::to_string(parent.get())).AppendASCII(name);
      if (!root_.Set(alias)) { unlinkat(parent.get(), name.c_str(), AT_REMOVEDIR); continue; }
      parent_ = std::move(parent); directory_ = std::move(directory); root_info_ = info; return true;
    }
    return false;
  }
  std::optional<size_t> Add(const base::FilePath& relative) {
    if (relative.IsAbsolute() || relative.ReferencesParent() || relative.empty()) return std::nullopt;
    const auto path = root_.GetPath().Append(relative);
    if (!base::CreateDirectory(path.DirName())) return std::nullopt;
    base::ScopedFD fd(memfd_create("alloy-selected-file", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    struct statfs fs;
    if (!fd.is_valid() || fstatfs(fd.get(), &fs) != 0 || fs.f_type != TMPFS_MAGIC) return std::nullopt;
    files_.push_back(std::move(fd)); return files_.size() - 1;
  }
  bool Write(size_t index, uint64_t offset, std::string bytes) {
    if (index >= files_.size()) return false;
    size_t copied = 0;
    while (copied < bytes.size()) {
      const auto result = HANDLE_EINTR(pwrite(files_[index].get(), bytes.data() + copied,
          bytes.size() - copied, offset + copied));
      if (result <= 0) return false;
      copied += result;
    }
    return true;
  }
  std::optional<base::FilePath> Seal(size_t index, const base::FilePath& relative, uint64_t size) {
    if (index >= files_.size() || HANDLE_EINTR(ftruncate(files_[index].get(), size)) != 0 ||
        fcntl(files_[index].get(), F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
      return std::nullopt;
    const auto target = base::FilePath("/proc").AppendASCII(std::to_string(getpid())).AppendASCII("fd")
        .AppendASCII(std::to_string(files_[index].get()));
    const auto alias = root_.GetPath().Append(relative);
    if (!base::CreateSymbolicLink(target, alias)) return std::nullopt;
    return alias;
  }
  const base::FilePath& root() const { return root_.GetPath(); }
  bool Verify(size_t index, const base::FilePath& alias) const {
    std::lock_guard lock(mutex_);
    constexpr int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (revoked_ || index >= files_.size() || !root_.GetPath().IsParent(alias)) return false;
    const int actual_seals = fcntl(files_[index].get(), F_GET_SEALS);
    if (actual_seals < 0 || (actual_seals & seals) != seals) return false;
    struct stat root, pinned, expected, observed;
    if (lstat(root_.GetPath().value().c_str(), &root) != 0 || !S_ISDIR(root.st_mode) ||
        fstat(directory_.get(), &pinned) != 0 || pinned.st_dev != root.st_dev || pinned.st_ino != root.st_ino ||
        root.st_dev != root_info_.st_dev || root.st_ino != root_info_.st_ino ||
        root.st_uid != geteuid() || (root.st_mode & 0777) != 0700) return false;
    for (auto path = alias.DirName(); path != root_.GetPath(); path = path.DirName()) {
      struct stat directory;
      if (!root_.GetPath().IsParent(path) || lstat(path.value().c_str(), &directory) != 0 ||
          !S_ISDIR(directory.st_mode) || directory.st_uid != geteuid()) return false;
    }
    base::ScopedFD fd(open(alias.value().c_str(), O_RDONLY | O_CLOEXEC));
    struct statfs fs;
    return fd.is_valid() && fstat(files_[index].get(), &expected) == 0 && fstat(fd.get(), &observed) == 0 &&
        S_ISREG(observed.st_mode) && expected.st_dev == observed.st_dev && expected.st_ino == observed.st_ino &&
        fstatfs(fd.get(), &fs) == 0 && fs.f_type == TMPFS_MAGIC;
  }
  void Revoke() {
    std::lock_guard lock(mutex_); revoked_ = true;
    // On failure keep both the owned tree and sealed backing for ScopedTempDir's
    // destruction-time cleanup, rather than losing the root's cleanup owner.
    if (!root_.IsValid() || root_.Delete()) files_.clear();
  }
 private:
  friend class base::RefCountedDeleteOnSequence<Backing>;
  friend class base::DeleteHelper<Backing>;
  ~Backing() = default;
  base::ScopedFD parent_, directory_;
  base::ScopedTempDir root_;
  std::vector<base::ScopedFD> files_;
  struct stat root_info_ = {};
  mutable std::mutex mutex_;
  bool revoked_ = false;
};

void BindSnapshotBlob(scoped_refptr<ChromeBlobStorageContext> context,
    base::FilePath path, std::string uuid, std::string mime, uint64_t size,
    file_access::ScopedFileAccessDelegate::RequestFilesAccessIOCallback file_access,
    base::OnceClosure consumer, mojo::PendingReceiver<blink::mojom::Blob> receiver) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  auto reference = storage::ShareableFileReference::GetOrCreate(path,
      storage::ShareableFileReference::DONT_DELETE_ON_FINAL_RELEASE,
      GetIOThreadTaskRunner({}).get());
  reference->AddFinalReleaseCallback(base::BindOnce(
      [](base::OnceClosure consumer, const base::FilePath&) { std::move(consumer).Run(); },
      std::move(consumer)));
  auto builder = std::make_unique<storage::BlobDataBuilder>(uuid);
  if (size) builder->AppendFile(path, 0, size, base::Time(), std::move(file_access));
  builder->set_content_type(mime);
  storage::BlobImpl::Create(context->context()->AddFinishedBlob(std::move(builder)), std::move(receiver));
}

class Snapshot final : public base::RefCounted<Snapshot> {
 public:
  Snapshot(RenderFrameHost* frame, Cap::Entries entries, bool directory, AlloyGuestUploadsReply reply,
      base::RepeatingCallback<bool()> extra)
      : document_(frame->GetWeakDocumentPtr()), directory_(directory), reply_(std::move(reply)), extra_(std::move(extra)),
        worker_(base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock(), base::TaskPriority::USER_VISIBLE,
            base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
        backing_(base::MakeRefCounted<Backing>(worker_)) {
    for (auto& cap : entries) queue_.push_back({cap, base::FilePath::FromUTF8Unsafe(cap->metadata().name), {}});
  }
  void Start() {
    self_ = this;
    auto* frame = document_.AsRenderFrameHostIfValid();
    if (frame) registry_ = SnapshotRegistry::Get(
        WebContents::FromRenderFrameHost(frame)->GetOutermostWebContents())->GetWeakPtr();
    auto* bridge = AlloyGuestIntegration::Get(frame);
    if (!bridge) { Finish(false); return; }
    admitted_ = bridge->CaptureLifetime(frame, base::BindOnce(&Snapshot::Retire, weak_.GetWeakPtr()));
    if (!Admitted() || (directory_ && (queue_.size() != 1 || !queue_.front().cap->metadata().directory))) {
      Finish(false); return;
    }
    worker_->PostTaskAndReplyWithResult(FROM_HERE, base::BindOnce(&Backing::Init, backing_),
        base::BindOnce([](base::WeakPtr<Snapshot> self, bool success) {
          if (!self) return;
          if (!success) self->Finish(false); else self->Next();
        }, weak_.GetWeakPtr()));
  }
  std::optional<AlloyGuestUploadProof> Proof(RenderFrameHost* original, const base::FilePath& path) {
    const auto found = aliases_.find(path);
    if (!Admitted() || original != document_.AsRenderFrameHostIfValid() || found == aliases_.end()) return std::nullopt;
    AlloyGuestUploadProof proof;
    proof.admitted = base::BindRepeating([](base::WeakPtr<Snapshot> self) {
      return self && self->Admitted();
    }, weak_.GetWeakPtr());
    proof.verify = base::BindRepeating([](scoped_refptr<Backing> backing, size_t index,
        base::FilePath path, std::shared_ptr<std::atomic<bool>> live) {
      return live->load() && backing->Verify(index, path) && live->load();
    }, backing_, found->second.index, path, live_flag_);
    return proof;
  }
  base::OnceClosure AcquireConsumer(RenderFrameHost* original, const base::FilePath& path) {
    if (!finished_ || !Proof(original, path)) return {};
    return base::BindPostTask(base::SequencedTaskRunner::GetCurrentDefault(),
        base::BindOnce([](scoped_refptr<Snapshot>) {}, base::WrapRefCounted(this)));
  }
  blink::mojom::SerializedBlobPtr Blob(RenderFrameHost* original, const base::FilePath& path) {
    auto proof = Proof(original, path);
    auto consumer = AcquireConsumer(original, path);
    if (!proof || !consumer) return nullptr;
    file_access::ScopedFileAccessDelegate::RequestFilesAccessIOCallback access_callback;
    if (file_access::ScopedFileAccessDelegate::HasInstance()) {
      const auto& destination = original->GetLastCommittedURL();
      if (!destination.is_valid()) return nullptr;
      access_callback = file_access::ScopedFileAccessDelegate::Get()->CreateFileAccessCallback(destination);
    }
    auto* manager = static_cast<FileSystemAccessManagerImpl*>(
        original->GetStoragePartition()->GetFileSystemAccessEntryFactory());
    const auto& alias = aliases_.at(path);
    auto blob = blink::mojom::SerializedBlob::New();
    blob->uuid = base::Uuid::GenerateRandomV4().AsLowercaseString();
    blob->content_type = alias.mime; blob->size = alias.size;
    // The pending native Blob owns the handoff even if the receiving message
    // is discarded before registration finishes. No UI thread waits for I/O.
    worker_->PostTaskAndReplyWithResult(FROM_HERE, proof->verify, base::BindOnce(
        [](base::WeakPtr<Snapshot> self, scoped_refptr<ChromeBlobStorageContext> context,
            base::FilePath path, std::string uuid, std::string mime, uint64_t size,
            file_access::ScopedFileAccessDelegate::RequestFilesAccessIOCallback file_access,
            base::OnceClosure consumer, mojo::PendingReceiver<blink::mojom::Blob> receiver, bool verified) {
          if (!self || !verified || !self->Admitted()) return;
          GetIOThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce(&BindSnapshotBlob,
              std::move(context), std::move(path), std::move(uuid), std::move(mime), size,
              std::move(file_access), std::move(consumer), std::move(receiver)));
        }, weak_.GetWeakPtr(), base::WrapRefCounted(manager->blob_context()), path,
           blob->uuid, blob->content_type, blob->size, std::move(access_callback), std::move(consumer),
           blob->blob.InitWithNewPipeAndPassReceiver()));
    return blob;
  }
 private:
  friend class base::RefCounted<Snapshot>;
  ~Snapshot() { RevokeBacking(); }
  struct Pending { scoped_refptr<Cap> cap; base::FilePath relative; std::set<std::string> ancestors; };
  struct Alias { size_t index; uint64_t size; std::string mime; };
  bool Admitted() const { return live_ && admitted_ && admitted_.Run() && (!extra_ || extra_.Run()); }
  void Finish(bool success) {
    auto retained = base::WrapRefCounted(this);
    success = success && Admitted();
    if (success) {
      extra_.Reset();  // Consumer leases retain document life, not a read deadline/receiver.
      finished_ = true;
      for (auto& file : result_.files) {
        file.native_file_lifetime = base::MakeRefCounted<base::RefCountedData<base::ScopedClosureRunner>>(
            base::ScopedClosureRunner(AcquireConsumer(document_.AsRenderFrameHostIfValid(), file.path)));
      }
    }
    if (reply_) {
      auto reply = std::move(reply_);
      if (success) {
        if (directory_) result_.directory = backing_->root();
        std::move(reply).Run(std::move(result_));
      } else std::move(reply).Run(std::nullopt);
    }
    if (!success) Retire();
    else self_.reset();  // Only native handoff/Blob owners retain completed snapshots.
  }
  void Retire() {
    if (!live_) return;
    auto retained = base::WrapRefCounted(this);
    RevokeBacking(); self_.reset();
  }
  void RevokeBacking() {
    if (!live_) return;
    live_ = false; live_flag_->store(false);
    if (registry_) for (const auto& [path, index] : aliases_) registry_->Remove(path);
    weak_.InvalidateWeakPtrs();
    if (reply_) std::move(reply_).Run(std::nullopt);
    if (backing_) worker_->PostTask(FROM_HERE, base::BindOnce(&Backing::Revoke, backing_));
    queue_.clear(); current_.reset(); retained_.clear(); backing_.reset();
  }
  void Next() {
    if (!Admitted()) { Finish(false); return; }
    if (queue_.empty()) { Finish(true); return; }
    current_ = std::move(queue_.front()); queue_.pop_front();
    auto& selected = *current_;
    if (!selected.cap->Admitted() || !selected.cap->metadata().readable) { Finish(false); return; }
    retained_.push_back(selected.cap);
    if (selected.cap->metadata().directory) {
      if (!directory_ || !selected.ancestors.insert(selected.cap->metadata().identity).second) { Finish(false); return; }
      selected.cap->Call("entries", {}, {}, base::BindOnce(
          [](base::WeakPtr<Snapshot> self, Cap::Result result) {
            if (!self) return;
            if (!self->Admitted()) { self->Finish(false); return; }
            auto children = self->current_->cap->Children(result);
            if (!children) { self->Finish(false); return; }
            std::set<std::string> names;
            for (auto& cap : *children) {
              if (!names.insert(cap->metadata().name).second) { self->Finish(false); return; }
              self->queue_.push_back({cap, self->current_->relative.Append(
                  base::FilePath::FromUTF8Unsafe(cap->metadata().name)), self->current_->ancestors});
            }
            self->Next();
          }, weak_.GetWeakPtr()), base::BindRepeating([](base::WeakPtr<Snapshot> self) {
            return self && self->Admitted();
          }, weak_.GetWeakPtr()));
      return;
    }
    selected.cap->Stat(base::BindOnce([](base::WeakPtr<Snapshot> self, Cap::Result result) {
      if (!self) return;
      if (!Cap::Body(result) || !self->Admitted()) { self->Finish(false); return; }
      self->offset_ = 0;
      self->size_ = self->current_->cap->metadata().size;
      self->modified_ = self->current_->cap->metadata().modified_ms;
      self->alias_ = base::FilePath::FromUTF8Unsafe(base::UnguessableToken::Create().ToString());
      // Private backing names are identities, not app names. Retain recognized
      // suffixes for existing native content classifiers without imposing a
      // host-filesystem basename limit on the app's logical name.
      const auto logical_name = base::FilePath::FromUTF8Unsafe(self->current_->cap->metadata().name);
      std::string mime;
      if (net::GetWellKnownMimeTypeFromFile(logical_name, &mime)) self->alias_ = self->alias_.AddExtension(logical_name.FinalExtension());
      self->worker_->PostTaskAndReplyWithResult(FROM_HERE,
          base::BindOnce(&Backing::Add, self->backing_, self->alias_),
          base::BindOnce([](base::WeakPtr<Snapshot> self, std::optional<size_t> index) {
            if (!self) return;
            if (!index || !self->Admitted()) { self->Finish(false); return; }
            self->index_ = *index; self->Read();
          }, self));
    }, weak_.GetWeakPtr()), base::BindRepeating([](base::WeakPtr<Snapshot> self) {
      return self && self->Admitted();
    }, weak_.GetWeakPtr()));
  }
  void Read() {
    if (!Admitted()) { Finish(false); return; }
    if (offset_ == size_) {
      worker_->PostTaskAndReplyWithResult(FROM_HERE,
          base::BindOnce(&Backing::Seal, backing_, index_, alias_, size_),
          base::BindOnce([](base::WeakPtr<Snapshot> self, std::optional<base::FilePath> path) {
            if (!self) return;
            auto* frame = self->document_.AsRenderFrameHostIfValid();
            if (!path || !self->Admitted() || !frame || !self->current_->cap->RegisterAlias(frame->GetBrowserContext(), *path)) {
              self->Finish(false); return;
            }
            self->result_.files.emplace_back(*path, base::FilePath::FromUTF8Unsafe(self->current_->cap->metadata().name));
            if (self->directory_) {
              auto& directories = self->result_.relative_directories[*path];
              for (const auto& name : self->current_->relative.DirName().GetComponents())
                directories.push_back(base::FilePath(name).AsUTF16Unsafe());
            }
             std::string mime = self->current_->cap->metadata().mime;
             if (mime.empty()) net::GetWellKnownMimeTypeFromFile(
                 base::FilePath::FromUTF8Unsafe(self->current_->cap->metadata().name), &mime);
             self->aliases_.emplace(*path, Alias{self->index_, self->size_, std::move(mime)});
            if (self->registry_) self->registry_->Add(*path, self->weak_.GetWeakPtr());
            self->Next();
          }, weak_.GetWeakPtr()));
      return;
    }
    const auto count = std::min<uint64_t>(Cap::kChunk, size_ - offset_);
    current_->cap->Read(offset_, count, modified_, base::BindOnce(
        [](base::WeakPtr<Snapshot> self, uint64_t count, Cap::Result result) {
          if (!self) return;
          const auto* status = result ? result->details.FindString("status") : nullptr;
          if (!self->Admitted() || !status || *status != "ok" || result->bytes.size() != 1 || result->bytes[0].size() != count) {
            self->Finish(false); return;
          }
          self->worker_->PostTaskAndReplyWithResult(FROM_HERE,
              base::BindOnce(&Backing::Write, self->backing_, self->index_, self->offset_, std::move(result->bytes[0])),
              base::BindOnce([](base::WeakPtr<Snapshot> self, uint64_t count, bool success) {
                if (!self) return;
                if (!success) { self->Finish(false); return; }
                self->offset_ += count; self->Read();
              }, self, count));
        }, weak_.GetWeakPtr(), count), base::BindRepeating([](base::WeakPtr<Snapshot> self) {
          return self && self->Admitted();
        }, weak_.GetWeakPtr()));
  }
  WeakDocumentPtr document_;
  const bool directory_;
  AlloyGuestUploadsReply reply_;
  base::RepeatingCallback<bool()> extra_;
  const scoped_refptr<base::SequencedTaskRunner> worker_;
  scoped_refptr<Backing> backing_;
  scoped_refptr<Snapshot> self_;
  base::RepeatingCallback<bool()> admitted_;
  std::deque<Pending> queue_;
  std::optional<Pending> current_;
  Cap::Entries retained_;
  AlloyGuestUploadFiles result_;
  uint64_t offset_ = 0, size_ = 0;
  size_t index_ = 0;
  int64_t modified_ = 0;
  bool live_ = true, finished_ = false;
  base::FilePath alias_;
  std::map<base::FilePath, Alias> aliases_;
  base::WeakPtr<SnapshotRegistry> registry_;
  std::shared_ptr<std::atomic<bool>> live_flag_ = std::make_shared<std::atomic<bool>>(true);
  base::WeakPtrFactory<Snapshot> weak_{this};
};
}  // namespace

std::optional<AlloyGuestUploadProof> GetAlloyGuestUploadProof(RenderFrameHost* original, const base::FilePath& path) {
  if (!original) return std::nullopt;
  auto* contents = WebContents::FromRenderFrameHost(original)->GetOutermostWebContents();
  auto* registry = SnapshotRegistry::Existing(contents);
  auto snapshot = registry ? registry->Find(path) : nullptr;
  return snapshot ? snapshot->Proof(original, path) : std::nullopt;
}

base::OnceClosure AcquireAlloyGuestUploadConsumer(RenderFrameHost* original, const base::FilePath& path) {
  if (!original) return {};
  auto* contents = WebContents::FromRenderFrameHost(original)->GetOutermostWebContents();
  auto* registry = SnapshotRegistry::Existing(contents);
  auto snapshot = registry ? registry->Find(path) : nullptr;
  return snapshot ? snapshot->AcquireConsumer(original, path) : base::OnceClosure();
}

blink::mojom::SerializedBlobPtr CreateAlloyGuestUploadBlob(RenderFrameHost* original, const base::FilePath& path) {
  if (!original) return nullptr;
  auto* registry = SnapshotRegistry::Existing(WebContents::FromRenderFrameHost(original)->GetOutermostWebContents());
  auto snapshot = registry ? registry->Find(path) : nullptr;
  return snapshot ? snapshot->Blob(original, path) : nullptr;
}

blink::mojom::SerializedBlobPtr CreateAlloyGuestUploadBlobForChild(
    FileSystemAccessManagerImpl* manager, int renderer_id, const base::FilePath& path) {
  auto* process = RenderProcessHost::FromID(ChildProcessId::FromUnsafeValue(renderer_id));
  if (!process) return nullptr;
  const auto url = manager->context()->CreateCrackedFileSystemURL(
      blink::StorageKey(), storage::kFileSystemTypeLocal, path);
  auto cap = Cap::FromURL(process->GetBrowserContext(), url);
  auto* original = cap && cap->Admitted() ? cap->original() : nullptr;
  if (!original || original->GetProcess() != process ||
      original->GetStoragePartition()->GetFileSystemAccessEntryFactory() != manager) return nullptr;
  return CreateAlloyGuestUploadBlob(original, path);
}

void SnapshotAlloyGuestFiles(RenderFrameHost* original, Cap::Entries entries, bool directory,
    AlloyGuestUploadsReply reply, base::RepeatingCallback<bool()> admitted) {
  if (!original) { std::move(reply).Run(std::nullopt); return; }
  if (entries.empty() && !directory) {
    std::move(reply).Run(!admitted || admitted.Run() ? std::make_optional<AlloyGuestUploadFiles>() : std::nullopt);
    return;
  }
  base::MakeRefCounted<Snapshot>(original, std::move(entries), directory, std::move(reply), std::move(admitted))->Start();
}
void SelectAlloyGuestUploadFiles(RenderFrameHost* original, const blink::mojom::FileChooserParams& params,
    scoped_refptr<FileSelectListener> listener) {
  using Mode = blink::mojom::FileChooserParams::Mode;
  if (!original || !original->IsActive() || params.open_writable || params.mode == Mode::kSave) {
    listener->FileSelectionCanceled(); return;
  }
  const bool directory = params.mode == Mode::kUploadFolder || params.mode == Mode::kOpenDirectory;
  base::DictValue details; details.Set("mode", directory ? "upload-directory" :
      params.mode == Mode::kOpenMultiple ? "upload-multiple" : "upload");
  details.Set("title", base::UTF16ToUTF8(params.title));
  base::ListValue accept; for (const auto& type : params.accept_types) accept.Append(base::UTF16ToUTF8(type));
  details.Set("accept", std::move(accept));
  Cap::Select(original, std::move(details), base::BindOnce(
      [](WeakDocumentPtr document, Mode mode, bool directory, scoped_refptr<FileSelectListener> listener,
          std::optional<Cap::Entries> entries) {
        if (!entries || entries->empty() || (mode != Mode::kOpenMultiple && entries->size() != 1)) {
          listener->FileSelectionCanceled(); return;
        }
        SnapshotAlloyGuestFiles(document.AsRenderFrameHostIfValid(), std::move(*entries), directory,
            base::BindOnce([](WeakDocumentPtr document, Mode mode, scoped_refptr<FileSelectListener> listener, std::optional<AlloyGuestUploadFiles> snapshot) {
              auto* original = document.AsRenderFrameHostIfValid();
              if (!snapshot || !original) { listener->FileSelectionCanceled(); return; }
              std::vector<blink::mojom::FileChooserFileInfoPtr> files;
              for (const auto& file : snapshot->files) {
                auto found = snapshot->relative_directories.find(file.path);
                auto directories = found == snapshot->relative_directories.end() ? std::vector<std::u16string>() : found->second;
                auto native = blink::mojom::NativeFileInfo::New(
                    file.path, file.display_name.AsUTF16Unsafe(), std::move(directories));
                native->serialized_blob = CreateAlloyGuestUploadBlob(original, file.path);
                if (!native->serialized_blob) { listener->FileSelectionCanceled(); return; }
                files.push_back(blink::mojom::FileChooserFileInfo::NewNativeFile(std::move(native)));
              }
              listener->FileSelected(std::move(files), snapshot->directory, mode);
            }, document, mode, std::move(listener)));
      }, original->GetWeakDocumentPtr(), params.mode, directory, std::move(listener)));
}
}  // namespace content
