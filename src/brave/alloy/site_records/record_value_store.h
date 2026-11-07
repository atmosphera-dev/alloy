// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_RECORD_VALUE_STORE_H_
#define BRAVE_ALLOY_SITE_RECORDS_RECORD_VALUE_STORE_H_

#include "brave/alloy/site_records/record_file_system.h"
#include "components/value_store/leveldb_value_store.h"

namespace alloy::site_records {

// Bind the complete native ValueStore to a previously resolved owner. Database
// names are virtual names inside that capability's encrypted namespace, never
// OS paths or authority to choose a record. The store owns its Env through native
// DB/compaction teardown. Admission precedes cached reads; writes are durable;
// neither malformed values nor corrupt DBs select native repair/raze/fallback.
FileStatus CreateRecordValueStore(
    std::shared_ptr<RecordFileSystem> files,
    RecordCapability capability,
    StorePurpose purpose,
    const base::FilePath& database,
    std::unique_ptr<value_store::LeveldbValueStore>* out);

}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_RECORD_VALUE_STORE_H_
