// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_value_store.h"

#include "base/functional/bind.h"
#include "brave/alloy/site_records/record_leveldb_env.h"

namespace alloy::site_records {
FileStatus CreateRecordValueStore(
    std::shared_ptr<RecordFileSystem> files,
    RecordCapability capability,
    StorePurpose purpose,
    const base::FilePath& database,
    std::unique_ptr<value_store::LeveldbValueStore>* out) {
  if (!out || database.empty() || database.ReferencesParent() ||
      database.value() == "." || database.value() == "/") return FileStatus::kInvalid;
  std::unique_ptr<RecordLevelDbEnv> env;
  const auto status = RecordLevelDbEnv::Create(files, capability, purpose, &env);
  if (status != FileStatus::kOk) return status;
  auto backend = std::make_unique<value_store::LazyLevelDb::Backend>();
  backend->options = env->Options();
  backend->env = std::move(env);
  backend->admitted = base::BindRepeating([](RecordCapability cap, StorePurpose purpose) {
    return cap.Allows(purpose, StorageAccess::kReadRecordFiles) &&
        cap.Allows(purpose, StorageAccess::kWriteRecordFiles);
  }, capability, purpose);
  backend->allow_recovery = false;
  backend->sync_writes = true;
  *out = std::make_unique<value_store::LeveldbValueStore>(
      "AlloyRecordSettings", database, std::move(backend), nullptr);
  return FileStatus::kOk;
}

}  // namespace alloy::site_records
