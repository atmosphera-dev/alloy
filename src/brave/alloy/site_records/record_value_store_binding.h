// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_VALUE_STORE_BINDING_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_VALUE_STORE_BINDING_H_
#include <memory>
#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/task/sequenced_task_runner.h"
#include "brave/alloy/site_records/record_capability.h"
#include "components/value_store/value_store.h"
namespace url { class Origin; }
namespace alloy::site_records {
class RecordFileSystem;
enum class FileStatus;
struct RecordValueStoreBinding {
  base::RepeatingCallback<std::unique_ptr<value_store::ValueStore>()> create;
  base::RepeatingCallback<bool()> admitted;
  base::RepeatingCallback<void(const value_store::ValueStore::Status&)> error;
  scoped_refptr<base::SequencedTaskRunner> runner;
  base::RepeatingCallback<base::CallbackListSubscription(base::RepeatingClosure)> observe_retirement;
};
// Native UI only queues provisioning; actual stores use this same original FIFO.
RecordValueStoreBinding BindOriginValueStore(
    std::shared_ptr<RecordFileSystem> files, RecordCapability original_profile,
    url::Origin origin, scoped_refptr<base::SequencedTaskRunner> runner,
    base::RepeatingCallback<void(FileStatus)> on_error);
}  // namespace alloy::site_records
#endif
