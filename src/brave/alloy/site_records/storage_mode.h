// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_STORAGE_MODE_H_
#define BRAVE_ALLOY_SITE_RECORDS_STORAGE_MODE_H_

namespace alloy::site_records {

enum class StorageMode { kStateless, kMemory, kEncrypted };
enum class NetworkState { kKeep, kCut };

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_STORAGE_MODE_H_
