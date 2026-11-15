// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_file_streams.h"
#include "content/browser/file_system_access/alloy_guest_file_binding.h"

#include <algorithm>
#include <ostream>
#include <utility>
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "content/browser/blob_storage/chrome_blob_storage_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "storage/browser/blob/blob_data_builder.h"
#include "storage/browser/blob/blob_data_handle.h"
#include "storage/browser/blob/blob_data_item.h"
#include "storage/browser/blob/blob_impl.h"
#include "storage/browser/blob/blob_storage_context.h"

namespace content {
namespace {
using Cap = AlloyGuestFileCapability;
class ReadPump final : public base::RefCounted<ReadPump> {
 public:
  ReadPump(scoped_refptr<Cap> cap, mojo::ScopedDataPipeProducerHandle producer,
      uint64_t offset, uint64_t length, int64_t modified,
      base::RepeatingCallback<bool()> permission, scoped_refptr<AlloyGuestFileBinding> binding,
      base::OnceCallback<void(int)> complete)
      : cap_(std::move(cap)), producer_(std::move(producer)), offset_(offset), length_(length),
        modified_(modified), permission_(std::move(permission)), binding_(std::move(binding)), complete_(std::move(complete)),
        watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        closed_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {}
  void Start() {
    self_ = this;
    retired_ = cap_->OnRetired(base::BindOnce(&ReadPump::Finish, weak_.GetWeakPtr(), net::ERR_ABORTED));
    if (done_) return;
    if (binding_) binding_retired_ = binding_->OnRetired(base::BindOnce(&ReadPump::Finish, weak_.GetWeakPtr(), net::ERR_ABORTED));
    if (done_) return;
    if (!producer_.is_valid() || !Admitted()) { Finish(net::ERR_ACCESS_DENIED); return; }
    if (closed_watcher_.Watch(producer_.get(), MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED, base::BindRepeating(
            [](base::WeakPtr<ReadPump> self, MojoResult, const mojo::HandleSignalsState&) {
              if (self) self->Finish(net::ERR_ABORTED);
            },
            weak_.GetWeakPtr())) != MOJO_RESULT_OK) { Finish(net::ERR_FAILED); return; }
    closed_watcher_.ArmOrNotify();
    const auto watched = watcher_.Watch(producer_.get(),
        MOJO_HANDLE_SIGNAL_WRITABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
        base::BindRepeating(&ReadPump::Writable, weak_.GetWeakPtr()));
    if (watched != MOJO_RESULT_OK) { Finish(net::ERR_FAILED); return; }
    Next();
  }
 private:
  friend class base::RefCounted<ReadPump>;
  ~ReadPump() = default;
  bool Admitted() const { return cap_->Admitted() && (!binding_ || binding_->Admitted()) &&
      (!permission_ || permission_.Run()); }
  void Finish(int result) {
    if (done_) return;
    auto retained = base::WrapRefCounted(this);
    done_ = true;
    watcher_.Cancel(); closed_watcher_.Cancel(); producer_.reset(); retired_ = {}; binding_retired_ = {};
    auto complete = std::move(complete_); self_.reset();
    std::move(complete).Run(result);
  }
  void Next() {
    if (done_) return;
    if (!Admitted()) { Finish(net::ERR_ACCESS_DENIED); return; }
    if (sent_ == length_) { Finish(net::OK); return; }
    const auto count = std::min<uint64_t>(Cap::kChunk, length_ - sent_);
    cap_->Read(offset_ + sent_, count, modified_, base::BindOnce(
        [](scoped_refptr<ReadPump> self, uint64_t count, Cap::Result result) {
          if (self->done_) return;
          const auto* status = result ? result->details.FindString("status") : nullptr;
          if (!status || *status != "ok" || result->bytes.size() != 1 ||
              result->bytes[0].size() != count || !self->Admitted()) {
            self->Finish(net::ERR_FAILED); return;
          }
          self->bytes_ = std::move(result->bytes[0]); self->position_ = 0;
          self->Drain();
        }, base::WrapRefCounted(this), count), base::BindRepeating(
            [](base::WeakPtr<ReadPump> self) { return self && !self->done_ && self->Admitted(); }, weak_.GetWeakPtr()));
  }
  void Drain() {
    if (done_) return;
    if (!Admitted()) { Finish(net::ERR_ACCESS_DENIED); return; }
    while (position_ < bytes_.size()) {
      size_t written = 0;
      const auto result = producer_->WriteData(base::as_byte_span(bytes_).subspan(position_),
          MOJO_WRITE_DATA_FLAG_NONE, written);
      if (result == MOJO_RESULT_SHOULD_WAIT) { watcher_.ArmOrNotify(); return; }
      if (result != MOJO_RESULT_OK || !written || written > bytes_.size() - position_) {
        Finish(net::ERR_FAILED); return;
      }
      position_ += written; sent_ += written;
    }
    bytes_.clear(); position_ = 0;
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&ReadPump::Next, weak_.GetWeakPtr()));
  }
  void Writable(MojoResult result, const mojo::HandleSignalsState& state) {
    if (done_) return;
    if (result != MOJO_RESULT_OK || (state.satisfied_signals & MOJO_HANDLE_SIGNAL_PEER_CLOSED)) {
      Finish(net::ERR_ABORTED); return;
    }
    Drain();
  }
  scoped_refptr<Cap> cap_;
  scoped_refptr<ReadPump> self_;
  mojo::ScopedDataPipeProducerHandle producer_;
  const uint64_t offset_, length_;
  const int64_t modified_;
  base::RepeatingCallback<bool()> permission_;
  scoped_refptr<AlloyGuestFileBinding> binding_;
  base::OnceCallback<void(int)> complete_;
  mojo::SimpleWatcher watcher_;
  mojo::SimpleWatcher closed_watcher_;
  base::CallbackListSubscription retired_;
  base::CallbackListSubscription binding_retired_;
  std::string bytes_;
  size_t position_ = 0;
  uint64_t sent_ = 0;
  bool done_ = false;
  base::WeakPtrFactory<ReadPump> weak_{this};
};

class AppDataHandle final : public storage::BlobDataItem::DataHandle {
 public:
  AppDataHandle(scoped_refptr<Cap> cap, uint64_t size, int64_t modified,
      scoped_refptr<base::SequencedTaskRunner> ui, base::RepeatingCallback<bool()> permission,
      scoped_refptr<AlloyGuestFileBinding> binding)
      : cap_(std::move(cap)), size_(size), modified_(modified), ui_(std::move(ui)),
        permission_(std::move(permission)), binding_(std::move(binding)) {}
  uint64_t GetSize() const override { return size_; }
  void Read(mojo::ScopedDataPipeProducerHandle producer, uint64_t offset,
      uint64_t length, base::OnceCallback<void(int)> callback) override {
    if (offset > size_ || length > size_ - offset) { std::move(callback).Run(net::ERR_REQUEST_RANGE_NOT_SATISFIABLE); return; }
    auto complete = base::BindPostTask(base::SequencedTaskRunner::GetCurrentDefault(), std::move(callback));
    ui_->PostTask(FROM_HERE, base::BindOnce(
        [](scoped_refptr<Cap> cap, mojo::ScopedDataPipeProducerHandle producer,
            uint64_t offset, uint64_t length, int64_t modified,
            base::RepeatingCallback<bool()> permission, scoped_refptr<AlloyGuestFileBinding> binding,
            base::OnceCallback<void(int)> complete) {
          auto pump = base::MakeRefCounted<ReadPump>(std::move(cap), std::move(producer),
              offset, length, modified, std::move(permission), std::move(binding), std::move(complete));
          pump->Start();
        }, cap_, std::move(producer), offset, length, modified_, permission_, binding_, std::move(complete)));
  }
  uint64_t GetSideDataSize() const override { return 0; }
  void ReadSideData(base::OnceCallback<void(int, mojo_base::BigBuffer)> callback) override {
    std::move(callback).Run(net::OK, mojo_base::BigBuffer());
  }
  void PrintTo(std::ostream* output) const override { *output << "app-owned file stream"; }
 private:
  ~AppDataHandle() override = default;
  const scoped_refptr<Cap> cap_;
  const uint64_t size_;
  const int64_t modified_;
  const scoped_refptr<base::SequencedTaskRunner> ui_;
  const base::RepeatingCallback<bool()> permission_;
  const scoped_refptr<AlloyGuestFileBinding> binding_;
};
}  // namespace

void CreateAlloyGuestFileBlob(scoped_refptr<ChromeBlobStorageContext> context,
    scoped_refptr<Cap> file, uint64_t size, int64_t modified, std::string uuid,
    std::string mime, mojo::PendingReceiver<blink::mojom::Blob> receiver,
    base::RepeatingCallback<bool()> permission, scoped_refptr<AlloyGuestFileBinding> binding) {
  auto data = base::MakeRefCounted<AppDataHandle>(std::move(file), size, modified,
      base::SequencedTaskRunner::GetCurrentDefault(), std::move(permission), std::move(binding));
  GetIOThreadTaskRunner({})->PostTask(FROM_HERE, base::BindOnce(
      [](scoped_refptr<ChromeBlobStorageContext> context, scoped_refptr<AppDataHandle> data,
          std::string uuid, std::string mime, mojo::PendingReceiver<blink::mojom::Blob> receiver) {
        if (!context->context()) return;
        auto builder = std::make_unique<storage::BlobDataBuilder>(uuid);
        builder->set_content_type(mime);
        builder->AppendReadableDataHandle(std::move(data));
        auto handle = context->context()->AddFinishedBlob(std::move(builder));
        if (!handle || handle->IsBroken() || !handle->IsStorageAccessAllowed()) return;
        storage::BlobImpl::Create(std::move(handle), std::move(receiver));
      }, std::move(context), std::move(data), std::move(uuid), std::move(mime), std::move(receiver)));
}
}  // namespace content
