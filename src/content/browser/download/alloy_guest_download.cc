// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/download/alloy_guest_download.h"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/ref_counted_delete_on_sequence.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/unguessable_token.h"
#include "base/strings/utf_string_conversions.h"
#include "components/download/public/common/download_create_info.h"
#include "components/download/public/common/download_destination_observer.h"
#include "components/download/public/common/download_file.h"
#include "components/download/public/common/download_save_info.h"
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/browser/renderer_host/frame_tree_node.h"
#include "content/browser/renderer_host/navigation_request.h"
#include "net/base/filename_util.h"
#include "net/base/io_buffer.h"

namespace content {
namespace {
using Cap = AlloyGuestFileCapability;
using Reason = download::DownloadInterruptReason;
constexpr Reason kOk = download::DOWNLOAD_INTERRUPT_REASON_NONE;
constexpr Reason kFailed = download::DOWNLOAD_INTERRUPT_REASON_FILE_FAILED;
constexpr Reason kAborted = download::DOWNLOAD_INTERRUPT_REASON_USER_CANCELED;
const int kSource = 0;
bool Success(const Cap::Result& result) {
  const auto* status = result ? result->details.FindString("status") : nullptr;
  return status && *status == "ok";
}

// UI-sequence endpoint. The download sequence supplies owned bytes/callbacks,
// never synchronously waits, and cannot reach the document or app directly.
class AppDestination final : public base::RefCountedDeleteOnSequence<AppDestination> {
 public:
  using Ready = base::OnceCallback<void(bool, std::optional<crypto::hash::Hasher>)>;
  AppDestination(RenderFrameHost* original, base::DictValue details)
      : base::RefCountedDeleteOnSequence<AppDestination>(base::SequencedTaskRunner::GetCurrentDefault()),
        document_(original ? original->GetWeakDocumentPtr() : WeakDocumentPtr()),
        details_(std::move(details)) {}
  void Open(Ready ready, base::OnceClosure failed, uint64_t prefix, bool sparse, std::string expected_hash) {
    if (ended_) { std::move(ready).Run(false, std::nullopt); return; }
    ready_ = std::move(ready); failed_ = std::move(failed); prefix_ = prefix;
    sparse_ = sparse; expected_hash_ = std::move(expected_hash);
    hash_ = sparse ? std::nullopt : std::make_optional<crypto::hash::Hasher>(crypto::hash::kSha256);
    hash_offset_ = 0;
    if (transaction_) { PrepareStage(); return; }
    Cap::Select(document_.AsRenderFrameHostIfValid(), details_.Clone(), base::BindOnce(
        [](scoped_refptr<AppDestination> self, std::optional<Cap::Entries> selected) {
          if (self->ended_ || !selected || selected->size() != 1 || (*selected)[0]->metadata().directory ||
              !(*selected)[0]->metadata().writable) { self->ReadyDone(false); return; }
          self->selected_ = (*selected)[0];
          base::DictValue details; details.Set("keep-existing", false); details.Set("exclusive", true);
          self->selected_->Call("open-write", std::move(details), {}, base::BindOnce(
              [](scoped_refptr<AppDestination> self, Cap::Result result) {
                auto entries = self->selected_->Children(result);
                if (self->ended_ || !entries || entries->size() != 1 || (*entries)[0]->metadata().directory ||
                    !(*entries)[0]->metadata().writable) { self->ReadyDone(false); return; }
                self->transaction_ = (*entries)[0];
                self->retired_ = self->transaction_->OnRetired(base::BindOnce(&AppDestination::Failed, self->weak_.GetWeakPtr()));
                self->PrepareStage();
              }, self), self->Admission());
        }, base::WrapRefCounted(this)), Admission());
  }
  void Write(uint64_t offset, std::string bytes, base::OnceCallback<void(bool)> reply) {
    if (ended_ || !transaction_ || !transaction_->Admitted()) { std::move(reply).Run(false); return; }
    if (offset < prefix_) {
      const auto validate = std::min<uint64_t>(bytes.size(), prefix_ - offset);
      transaction_->Stat(base::BindOnce([](scoped_refptr<AppDestination> self, uint64_t offset,
          uint64_t validate, std::string bytes, base::OnceCallback<void(bool)> reply, Cap::Result stat) {
        if (!Cap::Body(stat)) { std::move(reply).Run(false); return; }
        self->transaction_->Read(offset, validate, self->transaction_->metadata().modified_ms, base::BindOnce(
            [](scoped_refptr<AppDestination> self, uint64_t offset, uint64_t validate, std::string bytes,
                base::OnceCallback<void(bool)> reply, Cap::Result result) {
              if (!Success(result) || result->bytes.size() != 1 || result->bytes[0] != bytes.substr(0, validate)) {
                std::move(reply).Run(false); return;
              }
              if (validate == bytes.size()) { std::move(reply).Run(true); return; }
              self->WriteTransaction(offset + validate, bytes.substr(validate), std::move(reply));
            }, self, offset, validate, std::move(bytes), std::move(reply)), self->Admission());
      }, base::WrapRefCounted(this), offset, validate, std::move(bytes), std::move(reply)), Admission());
      return;
    }
    WriteTransaction(offset, std::move(bytes), std::move(reply));
  }
  void WriteTransaction(uint64_t offset, std::string bytes, base::OnceCallback<void(bool)> reply) {
    if (ended_ || !transaction_) { std::move(reply).Run(false); return; }
    base::DictValue details; details.Set("offset", std::to_string(offset)); details.Set("length", std::to_string(bytes.size()));
    transaction_->Call("write", std::move(details), {std::move(bytes)}, base::BindOnce(
        [](base::OnceCallback<void(bool)> reply, Cap::Result result) { std::move(reply).Run(Success(result)); }, std::move(reply)), Admission());
  }
  void Commit(base::OnceCallback<void(bool)> reply) {
    if (ended_ || !transaction_) { std::move(reply).Run(false); return; }
    transaction_->Call("close", {}, {}, base::BindOnce(
        [](scoped_refptr<AppDestination> self, base::OnceCallback<void(bool)> reply, Cap::Result result) {
          bool success = !self->ended_ && Success(result);
          if (success) {
            self->ended_ = true; self->retired_ = {}; self->failed_.Reset(); self->transaction_->Retire();
          }
          std::move(reply).Run(success);
        }, base::WrapRefCounted(this), std::move(reply)), Admission());
  }
  void Abort() {
    if (ended_) return;
    ended_ = true; retired_ = {}; failed_.Reset();
    ReadyDone(false);
    if (transaction_) transaction_->Call("abort", {}, {}, base::BindOnce(
        [](scoped_refptr<Cap> transaction, Cap::Result) { transaction->Retire(); }, transaction_));
  }
 private:
  friend class base::RefCountedDeleteOnSequence<AppDestination>;
  friend class base::DeleteHelper<AppDestination>;
  ~AppDestination() { Abort(); }
  base::RepeatingCallback<bool()> Admission() {
    return base::BindRepeating([](base::WeakPtr<AppDestination> self) { return self && !self->ended_; }, weak_.GetWeakPtr());
  }
  void ReadyDone(bool success) {
    if (ready_) std::move(ready_).Run(success && !ended_, success ? std::move(hash_) : std::nullopt);
  }
  void PrepareStage() {
    if (ended_ || !transaction_) { ReadyDone(false); return; }
    transaction_->Stat(base::BindOnce([](scoped_refptr<AppDestination> self, Cap::Result result) {
      if (!Cap::Body(result) || self->transaction_->metadata().size < self->prefix_) { self->ReadyDone(false); return; }
      self->ReadPrefix();
    }, base::WrapRefCounted(this)), Admission());
  }
  void ReadPrefix() {
    if (ended_ || !ready_) { ReadyDone(false); return; }
    if (sparse_) { ReadyDone(true); return; }
    if (hash_offset_ == prefix_) {
      if (!expected_hash_.empty()) {
        std::array<uint8_t, crypto::hash::kSha256Size> bytes;
        crypto::hash::Hasher partial(*hash_); partial.Finish(bytes);
        if (base::span(bytes) != base::as_byte_span(expected_hash_)) { ReadyDone(false); return; }
      }
      base::DictValue details; details.Set("length", std::to_string(prefix_));
      transaction_->Call("truncate", std::move(details), {}, base::BindOnce(
          [](scoped_refptr<AppDestination> self, Cap::Result result) { self->ReadyDone(Success(result)); }, base::WrapRefCounted(this)), Admission());
      return;
    }
    const auto count = std::min<uint64_t>(Cap::kChunk, prefix_ - hash_offset_);
    transaction_->Read(hash_offset_, count, transaction_->metadata().modified_ms, base::BindOnce(
        [](scoped_refptr<AppDestination> self, uint64_t count, Cap::Result result) {
          if (!Success(result) || result->bytes.size() != 1 || result->bytes[0].size() != count) { self->ReadyDone(false); return; }
          self->hash_->Update(result->bytes[0]); self->hash_offset_ += count; self->ReadPrefix();
        }, base::WrapRefCounted(this), count), Admission());
  }
  void Failed() {
    if (ended_) return;
    ended_ = true;
    ReadyDone(false);
    if (failed_) std::move(failed_).Run();
  }
  WeakDocumentPtr document_;
  base::DictValue details_;
  Ready ready_;
  uint64_t prefix_ = 0, hash_offset_ = 0;
  bool sparse_ = false;
  std::string expected_hash_;
  std::optional<crypto::hash::Hasher> hash_;
  bool ended_ = false;
  scoped_refptr<Cap> selected_, transaction_;
  base::CallbackListSubscription retired_;
  base::OnceClosure failed_;
  base::WeakPtrFactory<AppDestination> weak_{this};
};

class SourceData final : public base::SupportsUserData::Data, public download::DownloadItem::Observer,
                         public WebContentsObserver {
 public:
  SourceData(download::DownloadItem* item, WebContents* contents)
      : WebContentsObserver(contents), item_(item) { item_->AddObserver(this); }
  ~SourceData() override { if (destination) destination->Abort(); if (item_) item_->RemoveObserver(this); }
  void OnDownloadUpdated(download::DownloadItem* item) override {
    const auto state = item->GetState();
    if (state == download::DownloadItem::CANCELLED && destination) destination->Abort();
    if (auto* bridge = AlloyGuestIntegration::Get(contents.get()))
      bridge->DownloadProgress(identity, state == download::DownloadItem::COMPLETE ? "complete" :
          state == download::DownloadItem::CANCELLED ? "cancelled" :
          state == download::DownloadItem::IN_PROGRESS ? (item->IsPaused() ? "paused" :
              item->GetFullPath().empty() ? "preparing" : "active") : "interrupted",
          item->GetReceivedBytes(), item->GetTotalBytes());
  }
  void OnDownloadDestroyed(download::DownloadItem*) override { if (destination) destination->Abort(); item_ = nullptr; }
  void DidFinishNavigation(NavigationHandle* navigation) override {
    // During replacement the view has no admitted document for notifications.
    // Replay the native item's current state once that transition ends; the
    // native item, not a parallel cached-progress owner, remains authoritative.
    if (item_ && !navigation->IsSameDocument()) OnDownloadUpdated(item_);
  }
  base::RepeatingCallback<bool()> live;
  std::string identity;
  base::WeakPtr<WebContents> contents;
  WeakDocumentPtr document;
  scoped_refptr<AppDestination> destination;
 private:
  raw_ptr<download::DownloadItem> item_;
};

class GuestDownloadFile final : public download::DownloadFile {
 public:
  GuestDownloadFile(scoped_refptr<AppDestination> destination, std::unique_ptr<download::DownloadSaveInfo> info,
      std::unique_ptr<download::InputStream> stream, base::WeakPtr<download::DownloadDestinationObserver> observer)
      : destination_(std::move(destination)), ui_(base::SequencedTaskRunner::GetCurrentDefault()), observer_(observer),
        path_(info->file_path), potential_length_(info->total_bytes > 0 ? info->total_bytes : -1),
        prefix_(std::max<int64_t>(0, info->GetStartingFileWriteOffset())), expected_hash_(info->hash_of_partial_file) {
    auto source = std::make_unique<Source>(); source->stream = std::move(stream); source->offset = info->offset;
    sources_.push_back(std::move(source));
  }
  ~GuestDownloadFile() override { Cancel(); }
  void Initialize(InitializeCallback initialize, CancelRequestCallback cancel,
      const download::DownloadItem::ReceivedSlices& slices) override {
    worker_ = base::SequencedTaskRunner::GetCurrentDefault(); initialize_ = std::move(initialize); cancel_ = std::move(cancel);
    sparse_ = !slices.empty();
    for (const auto& slice : slices) initial_ranges_.emplace_back(slice.offset, slice.offset + slice.received_bytes);
    if (initial_ranges_.empty() && prefix_) initial_ranges_.emplace_back(0, prefix_);
    for (auto& source : sources_) source->stream->Initialize();
    ui_->PostTask(FROM_HERE, base::BindOnce(&AppDestination::Open, destination_,
        base::BindPostTask(worker_, base::BindOnce(&GuestDownloadFile::Opened, weak_.GetWeakPtr())),
        base::BindPostTask(worker_, base::BindOnce(&GuestDownloadFile::Error, weak_.GetWeakPtr(), kAborted)),
        prefix_, sparse_, expected_hash_));
  }
  void AddInputStream(std::unique_ptr<download::InputStream> stream, int64_t offset) override {
    if (ended_ || offset < 0) return;
    sparse_ = true; hash_.reset();
    auto source = std::make_unique<Source>(); source->stream = std::move(stream); source->offset = offset;
    if (worker_) source->stream->Initialize();
    sources_.push_back(std::move(source)); Pump();
  }
  void RenameAndUniquify(const base::FilePath&, RenameCompletionCallback callback) override { Rename(std::move(callback)); }
  void RenameAndAnnotate(const base::FilePath&, const std::string&, const GURL&, const GURL&,
      const std::optional<url::Origin>&, mojo::PendingRemote<quarantine::mojom::Quarantine>,
      RenameCompletionCallback callback) override { Rename(std::move(callback)); }
  void Detach() override { detached_ = true; }
  void Cancel() override {
    if (detached_ || ended_) return;
    Stop();
    ui_->PostTask(FROM_HERE, base::BindOnce(&AppDestination::Abort, destination_));
  }
  void SetPotentialFileLength(int64_t length) override { if (length >= 0) potential_length_ = length; Pump(); }
  const base::FilePath& FullPath() const override { return path_; }
  bool InProgress() const override { return !ended_; }
  void Pause() override { paused_ = true; }
  void Resume() override { paused_ = false; Pump(); }
  bool IsMemoryFile() override { return true; }
 private:
  struct Source {
    std::unique_ptr<download::InputStream> stream;
    scoped_refptr<net::IOBuffer> buffer;
    size_t size = 0, consumed = 0;
    int64_t offset = 0, received = 0;
    bool completed = false, waiting = false;
  };
  void Rename(RenameCompletionCallback callback) {
    ui_->PostTask(FROM_HERE, base::BindOnce(std::move(callback), kOk, path_));
  }
  void Opened(bool ready, std::optional<crypto::hash::Hasher> hash) {
    if (ended_) return;
    ui_->PostTask(FROM_HERE, base::BindOnce(initialize_, ready ? kOk : kFailed, 0));
    initialize_.Reset();
    if (!ready) { Stop(); return; }
    hash_ = std::move(hash); hashed_bytes_ = prefix_;
    if (sparse_) hash_.reset();
    total_ = 0; for (const auto& range : Ranges()) total_ += range.second - range.first;
    opened_ = true; Pump();
  }
  void Error(Reason reason) {
    if (ended_) return;
    if (initialize_) {
      ui_->PostTask(FROM_HERE, base::BindOnce(initialize_, reason, 0)); initialize_.Reset();
    } else {
      ui_->PostTask(FROM_HERE, base::BindOnce(&download::DownloadDestinationObserver::DestinationError,
          observer_, reason, total_, std::move(hash_)));
    }
    if (cancel_) for (const auto& source : sources_)
      ui_->PostTask(FROM_HERE, base::BindOnce(cancel_, source->offset));
    // The native item retains staged app data for its ordinary resume path.
    // Cancellation, item destruction or source retirement aborts that stage.
    Stop();
  }
  void Stop() {
    ended_ = true; weak_.InvalidateWeakPtrs();
    for (auto& source : sources_) source->stream->ClearDataReadyCallback();
  }
  void CompletedSource(size_t index) {
    if (ended_ || index >= sources_.size()) return;
    auto& source = *sources_[index];
    auto reason = source.stream->GetCompletionStatus();
    if (reason != kOk) { Error(reason); return; }
    source.completed = true; source.waiting = false; Pump();
  }
  void Pump() {
    if (!opened_ || busy_ || paused_ || ended_) return;
    bool pending = false;
    for (size_t index = 0; index < sources_.size(); ++index) {
      auto& source = *sources_[index];
      if (source.completed) continue;
      pending = true;
      if (source.waiting) continue;
      source.stream->ClearDataReadyCallback();
      if (!source.buffer) {
        auto state = source.stream->Read(&source.buffer, &source.size);
        if (state == download::InputStream::EMPTY) {
          source.stream->RegisterDataReadyCallback(base::BindRepeating(
              [](base::WeakPtr<GuestDownloadFile> self, MojoResult) { if (self) self->Pump(); }, weak_.GetWeakPtr()));
          continue;
        }
        if (state == download::InputStream::WAIT_FOR_COMPLETION) {
          source.waiting = true;
          source.stream->RegisterCompletionCallback(base::BindOnce(&GuestDownloadFile::CompletedSource, weak_.GetWeakPtr(), index));
          continue;
        }
        if (state == download::InputStream::COMPLETE) { CompletedSource(index); return; }
        if (!source.buffer || !source.size) { Error(kFailed); return; }
        source.consumed = 0;
      }
      const auto count = std::min<size_t>(Cap::kChunk, source.size - source.consumed);
      if (source.offset < 0 || source.received > std::numeric_limits<int64_t>::max() - source.offset ||
          count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() - source.offset - source.received)) {
        Error(kFailed); return;
      }
      busy_ = true;
      auto bytes = base::span(source.buffer->data(), source.size).subspan(source.consumed, count);
      ui_->PostTask(FROM_HERE, base::BindOnce(&AppDestination::Write, destination_,
          source.offset + source.received, std::string(bytes.begin(), bytes.end()),
          base::BindPostTask(worker_, base::BindOnce(&GuestDownloadFile::Written, weak_.GetWeakPtr(), index, count))));
      return;
    }
    if (pending) return;
    auto ranges = Ranges();
    if (ranges.size() > 1 || (!ranges.empty() && ranges.front().first != 0) ||
        (potential_length_ >= 0 && total_ != potential_length_)) {
      Error(kFailed); return;
    }
    // Every source is drained and every byte acknowledged. Native completion
    // still waits for the app's actual transaction commit.
    busy_ = true;
    ui_->PostTask(FROM_HERE, base::BindOnce(&AppDestination::Commit, destination_,
        base::BindPostTask(worker_, base::BindOnce(&GuestDownloadFile::Committed, weak_.GetWeakPtr()))));
  }
  void Written(size_t index, size_t count, bool success) {
    busy_ = false;
    if (ended_) return;
    if (!success) { Error(kFailed); return; }
    auto& source = *sources_[index];
    const auto offset = source.offset + source.received;
    if (hash_) {
      size_t skip = offset < hashed_bytes_ ? std::min<uint64_t>(count, hashed_bytes_ - offset) : 0;
      if (skip != count && offset + static_cast<int64_t>(skip) != hashed_bytes_) hash_.reset();
      else if (skip != count) {
        hash_->Update(source.buffer->span().subspan(source.consumed + skip, count - skip));
        hashed_bytes_ += count - skip;
      }
    }
    source.received += count; source.consumed += count;
    total_ = 0;
    for (const auto& range : Ranges()) total_ += range.second - range.first;
    if (source.consumed == source.size) { source.buffer.reset(); source.size = source.consumed = 0; }
    std::vector<download::DownloadItem::ReceivedSlice> slices;
    if (sparse_) for (const auto& range : Ranges()) slices.emplace_back(range.first, range.second - range.first, false);
    ui_->PostTask(FROM_HERE, base::BindOnce(&download::DownloadDestinationObserver::DestinationUpdate,
        observer_, total_, 0, slices));
    worker_->PostTask(FROM_HERE, base::BindOnce(&GuestDownloadFile::Pump, weak_.GetWeakPtr()));
  }
  void Committed(bool success) {
    busy_ = false;
    if (ended_) return;
    if (!success) { Error(kFailed); return; }
    ended_ = true;
    ui_->PostTask(FROM_HERE, base::BindOnce(&download::DownloadDestinationObserver::DestinationCompleted,
        observer_, total_, std::move(hash_)));
  }
  std::vector<std::pair<int64_t, int64_t>> Ranges() const {
    std::vector<std::pair<int64_t, int64_t>> ranges, merged;
    ranges = initial_ranges_;
    for (const auto& source : sources_) if (source->received)
      ranges.emplace_back(source->offset, source->offset + source->received);
    std::sort(ranges.begin(), ranges.end());
    for (const auto& range : ranges) {
      if (!merged.empty() && range.first <= merged.back().second)
        merged.back().second = std::max(merged.back().second, range.second);
      else merged.push_back(range);
    }
    return merged;
  }
  const scoped_refptr<AppDestination> destination_;
  const scoped_refptr<base::SequencedTaskRunner> ui_;
  scoped_refptr<base::SequencedTaskRunner> worker_;
  base::WeakPtr<download::DownloadDestinationObserver> observer_;
  const base::FilePath path_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<std::pair<int64_t, int64_t>> initial_ranges_;
  InitializeCallback initialize_;
  CancelRequestCallback cancel_;
  int64_t total_ = 0, potential_length_ = -1;
  const int64_t prefix_;
  const std::string expected_hash_;
  int64_t hashed_bytes_ = 0;
  std::optional<crypto::hash::Hasher> hash_;
  bool opened_ = false, busy_ = false, paused_ = false, ended_ = false, detached_ = false, sparse_ = false;
  base::WeakPtrFactory<GuestDownloadFile> weak_{this};
};
}  // namespace

base::RepeatingCallback<bool()> CaptureAlloyGuestDownloadSource(RenderFrameHost* frame, bool navigation_response) {
  auto* bridge = AlloyGuestIntegration::Get(frame);
  base::RepeatingCallback<bool()> admission;
  if (bridge) {
    auto* node = FrameTreeNode::GloballyFindByID(frame->GetFrameTreeNodeId());
    auto* navigation = navigation_response && node ? node->navigation_request() : nullptr;
    admission = navigation ? bridge->CaptureDownloadAdmission(frame, navigation->GetNavigationId())
                           : bridge->CaptureAdmission(frame, true);
  }
  return base::BindRepeating([](WeakDocumentPtr document, base::RepeatingCallback<bool()> admission) {
    auto* original = document.AsRenderFrameHostIfValid();
    return original && original->IsActive() && admission && admission.Run();
  }, frame ? frame->GetWeakDocumentPtr() : WeakDocumentPtr(), std::move(admission));
}
void RememberAlloyGuestDownloadSource(download::DownloadItem* item, const download::DownloadCreateInfo& info) {
  if (item && info.save_info && info.save_info->app_item_created) info.save_info->app_item_created.Run(item);
}
void PrepareAlloyGuestDownload(download::DownloadCreateInfo* info, download::DownloadItem* previous) {
  auto* frame = RenderFrameHost::FromID(info->render_process_id, info->render_frame_id);
  if (!info->save_info) info->save_info = std::make_unique<download::DownloadSaveInfo>();
  auto& save = *info->save_info;
  std::string existing_identity;
  scoped_refptr<AppDestination> destination;
  if (!info->is_new_download) {
    auto* source = previous ? static_cast<SourceData*>(previous->GetUserData(&kSource)) : nullptr;
    save.app_source_live = source ? source->live : base::RepeatingCallback<bool()>();
    frame = source ? source->document.AsRenderFrameHostIfValid() : nullptr;
    if (source) { existing_identity = source->identity; destination = source->destination; }
  }
  if (!save.app_source_live || !save.app_source_live.Run()) frame = nullptr;
  base::DictValue details; details.Set("mode", "download"); details.Set("source-url", info->url().spec());
  const auto name = net::GenerateFileName(info->url(), info->content_disposition, {},
      base::UTF16ToUTF8(save.suggested_name), info->mime_type, "download");
  const auto identity = base::UnguessableToken::DeserializeFromString(existing_identity)
      ? existing_identity : base::UnguessableToken::Create().ToString();
  details.Set("suggested-name", name.AsUTF8Unsafe());
  details.Set("download-id", identity);
  if (!destination || !frame) destination = base::MakeRefCounted<AppDestination>(frame, std::move(details));
  auto* contents = frame ? WebContents::FromRenderFrameHost(frame)->GetOutermostWebContents() : nullptr;
  save.app_item_created = base::BindRepeating([](scoped_refptr<AppDestination> destination,
      base::RepeatingCallback<bool()> live, std::string identity, WeakDocumentPtr document,
      base::WeakPtr<WebContents> contents, download::DownloadItem* item) {
    auto data = std::make_unique<SourceData>(item, contents.get()); data->live = std::move(live);
    data->identity = std::move(identity); data->document = document; data->contents = contents;
    data->destination = std::move(destination); item->SetUserData(&kSource, std::move(data));
  }, destination, save.app_source_live, identity, frame ? frame->GetWeakDocumentPtr() : WeakDocumentPtr(),
     contents ? contents->GetWeakPtr() : base::WeakPtr<WebContents>());
  save.file.Close();
  save.file_path = base::FilePath("/alloy-app-downloads").AppendASCII(identity).Append(name.BaseName());
  save.app_destination_factory = base::BindOnce([](scoped_refptr<AppDestination> destination,
      std::unique_ptr<download::DownloadSaveInfo> save, std::unique_ptr<download::InputStream> stream,
      base::WeakPtr<download::DownloadDestinationObserver> observer) -> download::DownloadFile* {
    return new GuestDownloadFile(std::move(destination), std::move(save), std::move(stream), observer);
  }, std::move(destination));
}
bool ControlAlloyGuestDownload(WebContents* original, const std::string& identity, const std::string& operation) {
  if (!original || !base::UnguessableToken::DeserializeFromString(identity)) return false;
  download::DownloadItem* selected = nullptr;
  content::DownloadManager::DownloadVector items;
  original->GetBrowserContext()->GetDownloadManager()->GetAllDownloads(&items);
  for (const auto& item : items) {
    auto* source = static_cast<SourceData*>(item->GetUserData(&kSource));
    if (source && source->identity == identity && source->contents.get() == original &&
        source->live && source->live.Run()) { selected = item.get(); break; }
  }
  if (!selected) return false;
  if (operation == "cancel" && selected->GetState() != download::DownloadItem::COMPLETE &&
      selected->GetState() != download::DownloadItem::CANCELLED) { selected->Cancel(true); return true; }
  if (operation == "pause" && selected->GetState() == download::DownloadItem::IN_PROGRESS &&
      !selected->GetFullPath().empty()) { selected->Pause(); return true; }
  if (operation == "resume" && selected->CanResume()) { selected->Resume(true); return true; }
  return false;
}
}  // namespace content
