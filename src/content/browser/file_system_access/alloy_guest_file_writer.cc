// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_file_writer.h"
#include "content/browser/file_system_access/alloy_guest_file_binding.h"
#include <array>
#include <limits>
#include <utility>
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/sequenced_task_runner.h"
#include "content/browser/file_system_access/file_system_access_error.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/system/simple_watcher.h"

namespace content {
using Cap = AlloyGuestFileCapability;
using Status = blink::mojom::FileSystemAccessStatus;
using Error = blink::mojom::FileSystemAccessErrorPtr;
Error AlloyGuestFileError(const Cap::Result& result) {
  const auto* status = result ? result->details.FindString("status") : nullptr;
  if (status && *status == "ok") return file_system_access_error::Ok();
  base::File::Error error = result ? base::File::FILE_ERROR_FAILED : base::File::FILE_ERROR_ABORT;
  if (status) {
    if (*status == "not-found") error = base::File::FILE_ERROR_NOT_FOUND;
    else if (*status == "denied") error = base::File::FILE_ERROR_ACCESS_DENIED;
    else if (*status == "exists") error = base::File::FILE_ERROR_EXISTS;
    else if (*status == "not-directory") error = base::File::FILE_ERROR_NOT_A_DIRECTORY;
    else if (*status == "not-file") error = base::File::FILE_ERROR_NOT_A_FILE;
    else if (*status == "not-empty") error = base::File::FILE_ERROR_NOT_EMPTY;
    else if (*status == "aborted") error = base::File::FILE_ERROR_ABORT;
  }
  return file_system_access_error::FromFileError(error);
}
namespace {
class GuestWriter final : public blink::mojom::FileSystemAccessFileWriter,
                          public base::RefCounted<GuestWriter> {
 public:
  GuestWriter(scoped_refptr<Cap> transaction, base::RepeatingCallback<bool()> permission,
      scoped_refptr<FileSystemAccessLockManager::LockHandle> lock, bool auto_close,
      scoped_refptr<AlloyGuestFileBinding> binding)
      : transaction_(std::move(transaction)), permission_(std::move(permission)), lock_(std::move(lock)),
        auto_close_(auto_close), binding_(std::move(binding)), watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {}
  void Bind(mojo::PendingReceiver<blink::mojom::FileSystemAccessFileWriter> receiver) {
    self_ = this;
    receiver_.Bind(std::move(receiver));
    receiver_.set_disconnect_handler(base::BindOnce(&GuestWriter::Disconnected, weak_.GetWeakPtr()));
    retired_ = transaction_->OnRetired(base::BindOnce(&GuestWriter::Retired, weak_.GetWeakPtr()));
    if (!finished_ && binding_) binding_retired_ = binding_->OnRetired(base::BindOnce(&GuestWriter::Retired, weak_.GetWeakPtr()));
  }
  void Write(uint64_t offset, mojo::ScopedDataPipeConsumerHandle stream, WriteCallback callback) override {
    if (!Ready() || !stream.is_valid() || offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      std::move(callback).Run(file_system_access_error::FromStatus(Status::kInvalidState), 0); return;
    }
    busy_ = true; offset_ = offset; written_ = 0; write_reply_ = std::move(callback);
    input_ = std::move(stream); operation_live_ = std::make_shared<bool>(true);
    if (watcher_.Watch(input_.get(), MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
        base::BindRepeating(&GuestWriter::Readable, weak_.GetWeakPtr())) != MOJO_RESULT_OK) {
      EndWrite(file_system_access_error::FromStatus(Status::kOperationFailed)); return;
    }
    ReadNext();
  }
  void Truncate(uint64_t length, TruncateCallback callback) override {
    if (!Ready() || length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      std::move(callback).Run(file_system_access_error::FromStatus(Status::kInvalidState)); return;
    }
    busy_ = true; operation_live_ = std::make_shared<bool>(true);
    truncate_reply_ = std::move(callback);
    base::DictValue details; details.Set("length", std::to_string(length));
    transaction_->Call("truncate", std::move(details), {}, base::BindOnce(
        [](base::WeakPtr<GuestWriter> weak, std::shared_ptr<bool> live, Cap::Result result) {
          if (!weak || weak->finished_ || !*live || !weak->truncate_reply_) return;
          auto self = base::WrapRefCounted(weak.get());
          auto error = AlloyGuestFileError(result);
          self->failed_ |= error->status != Status::kOk;
          self->busy_ = false; *self->operation_live_ = false; self->operation_live_.reset();
          auto callback = std::move(self->truncate_reply_);
          std::move(callback).Run(std::move(error)); self->MaybeAutoClose();
        }, weak_.GetWeakPtr(), operation_live_), OperationPermission());
  }
  void Close(CloseCallback callback) override {
    auto retained = base::WrapRefCounted(this);
    if (!Ready()) {
      std::move(callback).Run(file_system_access_error::FromStatus(Status::kInvalidState)); return;
    }
    if (failed_) {
      std::move(callback).Run(file_system_access_error::FromStatus(Status::kOperationFailed));
      Abort(base::DoNothing()); return;
    }
    busy_ = true; close_reply_ = std::move(callback);
    operation_live_ = std::make_shared<bool>(true);
    transaction_->Call("close", {}, {}, base::BindOnce(
        [](base::WeakPtr<GuestWriter> weak, Cap::Result result) {
          if (!weak || weak->finished_ || !weak->close_reply_) return;
          auto self = base::WrapRefCounted(weak.get());
          auto callback = std::move(self->close_reply_);
          auto error = AlloyGuestFileError(result);
          std::move(callback).Run(std::move(error));
          self->Finish();
        }, weak_.GetWeakPtr()), OperationPermission());
  }
  void Abort(AbortCallback callback) override {
    if (finished_ || aborting_) { std::move(callback).Run(file_system_access_error::FromStatus(Status::kInvalidState)); return; }
    auto retained = base::WrapRefCounted(this);
    aborting_ = true;
    CancelWrite();
    busy_ = true;
    transaction_->Call("abort", {}, {}, base::BindOnce(
        [](base::WeakPtr<GuestWriter> weak, AbortCallback callback, Cap::Result result) {
          if (!weak || weak->finished_) return;
          auto self = base::WrapRefCounted(weak.get());
          auto error = AlloyGuestFileError(result);
          std::move(callback).Run(std::move(error)); self->Finish();
        }, weak_.GetWeakPtr(), std::move(callback)), permission_);
  }
 private:
  friend class base::RefCounted<GuestWriter>;
  ~GuestWriter() = default;
  bool Ready() const { return !finished_ && !aborting_ && !busy_ && transaction_->Admitted() &&
      (!binding_ || binding_->Admitted()) && (!permission_ || permission_.Run()); }
  base::RepeatingCallback<bool()> OperationPermission() {
    return base::BindRepeating([](std::shared_ptr<bool> live, base::RepeatingCallback<bool()> permission,
        scoped_refptr<AlloyGuestFileBinding> binding) {
      return *live && (!binding || binding->Admitted()) && (!permission || permission.Run());
    }, operation_live_, permission_, binding_);
  }
  void Readable(MojoResult, const mojo::HandleSignalsState&) { ReadNext(); }
  void ReadNext() {
    if (finished_ || !input_.is_valid()) return;
    if (!transaction_->Admitted() || (binding_ && !binding_->Admitted()) || (permission_ && !permission_.Run())) {
      EndWrite(file_system_access_error::FromStatus(Status::kPermissionDenied)); return;
    }
    std::array<uint8_t, Cap::kChunk> buffer;
    size_t count = 0;
    const auto result = input_->ReadData(MOJO_READ_DATA_FLAG_NONE, buffer, count);
    if (result == MOJO_RESULT_SHOULD_WAIT) { watcher_.ArmOrNotify(); return; }
    if (result == MOJO_RESULT_FAILED_PRECONDITION) { EndWrite(file_system_access_error::Ok()); return; }
    if (result != MOJO_RESULT_OK || !count ||
        count > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - offset_ - written_) {
      EndWrite(file_system_access_error::FromStatus(Status::kOperationFailed)); return;
    }
    base::DictValue details; details.Set("offset", std::to_string(offset_ + written_));
    details.Set("length", std::to_string(count));
    transaction_->Call("write", std::move(details),
        {std::string(reinterpret_cast<const char*>(buffer.data()), count)}, base::BindOnce(
        [](base::WeakPtr<GuestWriter> self, size_t count, Cap::Result result) {
          if (!self || self->finished_ || !self->write_reply_) return;
          auto error = AlloyGuestFileError(result);
          if (error->status != Status::kOk) { self->EndWrite(std::move(error)); return; }
          self->written_ += count;
          base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
              FROM_HERE, base::BindOnce(&GuestWriter::ReadNext, self));
        }, weak_.GetWeakPtr(), count), OperationPermission());
  }
  void EndWrite(Error error) {
    if (!write_reply_) return;
    auto retained = base::WrapRefCounted(this);
    failed_ |= error->status != Status::kOk;
    watcher_.Cancel(); input_.reset(); busy_ = false;
    if (operation_live_) *operation_live_ = false;
    operation_live_.reset();
    auto callback = std::move(write_reply_);
    std::move(callback).Run(std::move(error), written_);
    MaybeAutoClose();
  }
  void CancelWrite() {
    if (operation_live_) *operation_live_ = false;
    if (write_reply_) EndWrite(file_system_access_error::FromStatus(Status::kOperationAborted));
    watcher_.Cancel(); input_.reset(); busy_ = false;
    if (truncate_reply_) std::move(truncate_reply_).Run(file_system_access_error::FromStatus(Status::kOperationAborted));
    if (close_reply_) std::move(close_reply_).Run(file_system_access_error::FromStatus(Status::kOperationAborted));
  }
  void Retired() {
    if (finished_) return;
    auto retained = base::WrapRefCounted(this);
    finished_ = true; CancelWrite();
    Finish();
  }
  void Finish() {
    auto retained = base::WrapRefCounted(this);
    finished_ = true;
    if (operation_live_) *operation_live_ = false;
    watcher_.Cancel(); input_.reset(); receiver_.reset(); retired_ = {}; binding_retired_ = {};
    transaction_->Retire(); lock_.reset(); self_.reset();
  }
  void Disconnected() {
    if (finished_) return;
    disconnected_ = true;
    if (auto_close_) MaybeAutoClose(); else Abort(base::DoNothing());
  }
  void MaybeAutoClose() {
    if (!finished_ && !aborting_ && !busy_ && disconnected_ && auto_close_) {
      if (!transaction_->Admitted() || (binding_ && !binding_->Admitted()) ||
          (permission_ && !permission_.Run())) Abort(base::DoNothing());
      else Close(base::DoNothing());
    }
  }
  const scoped_refptr<Cap> transaction_;
  const base::RepeatingCallback<bool()> permission_;
  scoped_refptr<FileSystemAccessLockManager::LockHandle> lock_;
  const bool auto_close_;
  const scoped_refptr<AlloyGuestFileBinding> binding_;
  scoped_refptr<GuestWriter> self_;
  mojo::Receiver<blink::mojom::FileSystemAccessFileWriter> receiver_{this};
  mojo::ScopedDataPipeConsumerHandle input_;
  mojo::SimpleWatcher watcher_;
  base::CallbackListSubscription retired_;
  base::CallbackListSubscription binding_retired_;
  std::shared_ptr<bool> operation_live_;
  WriteCallback write_reply_;
  TruncateCallback truncate_reply_;
  CloseCallback close_reply_;
  uint64_t offset_ = 0, written_ = 0;
  bool busy_ = false, finished_ = false, failed_ = false, disconnected_ = false, aborting_ = false;
  base::WeakPtrFactory<GuestWriter> weak_{this};
};
}  // namespace

mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter> CreateAlloyGuestFileWriter(
    scoped_refptr<Cap> transaction, base::RepeatingCallback<bool()> permission,
    scoped_refptr<FileSystemAccessLockManager::LockHandle> lock, bool auto_close,
    scoped_refptr<AlloyGuestFileBinding> binding) {
  if (!transaction || !transaction->Admitted()) return {};
  mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter> remote;
  auto writer = base::MakeRefCounted<GuestWriter>(std::move(transaction), std::move(permission),
      std::move(lock), auto_close, std::move(binding));
  writer->Bind(remote.InitWithNewPipeAndPassReceiver());
  return remote;
}
}  // namespace content
