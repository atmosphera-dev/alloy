// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_SECRET_SERVICE_KEY_SOURCE_H_
#define BRAVE_ALLOY_SITE_RECORDS_SECRET_SERVICE_KEY_SOURCE_H_

#include "brave/alloy/site_records/key_source.h"

namespace alloy::site_records {

enum class KeyringResult { kOk, kUnavailable, kLocked, kMissing, kFailed };

// Synchronous background-thread source. No retained bus, credentials or keys.
// Uses an already-running Secret Service, never OSCrypt's global safe-storage
// key, a basic/fixed fallback, service activation or interactive unlock.
class SecretServiceKeySource final : public KeySource {
 public:
  KeySourceKind kind() const override;
  FileStatus Create(const KeySourceParameters& parameters, SecretKey* key) override;
  FileStatus Derive(const KeySourceParameters& parameters, SecretKey* key) override;
  KeyringResult result() const { return result_; }

 private:
  FileStatus Get(const KeySourceParameters& parameters, bool create, SecretKey* key);
  KeyringResult result_ = KeyringResult::kFailed;
};

}  // namespace alloy::site_records
#endif
