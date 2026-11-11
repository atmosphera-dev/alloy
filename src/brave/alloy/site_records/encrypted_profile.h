// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_PROFILE_H_
#define BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_PROFILE_H_

#include <memory>
#include <mutex>
#include <thread>

#include "brave/alloy/site_records/key_source.h"
#include "brave/alloy/site_records/record_file_system.h"
#include "brave/alloy/site_records/site_record_decay.h"

namespace alloy::site_records {

enum class ProfileState { kLocked, kUnlocked, kTransition, kError };

// A profile lifecycle, independent of Chromium Profile creation. It can be
// opened while locked, before any browser context or private prefs are read.
// Provider calls and filesystem teardown execute outside its lifecycle mutex.
class EncryptedProfile {
 public:
  static FileStatus Create(const base::FilePath& path, KeySource& source,
                           std::unique_ptr<EncryptedProfile>* out);
  static FileStatus Open(const base::FilePath& path,
                         std::unique_ptr<EncryptedProfile>* out);
  ~EncryptedProfile();
  FileStatus Unlock(KeySource& source);
  FileStatus Lock();
  bool unlocked() const;
  ProfileState state() const;
  std::shared_ptr<RecordFileSystem> files() const;
  FileStatus SetDecayPolicy(DecayPolicy policy);
  FileStatus GetDecayPolicy(DecayPolicy* policy) const;
  // Once native owners exist, their original settings queue owns the R13 UI
  // barrier. The handler only schedules work and must never block on UI.
  void BindNativeDecay(const std::shared_ptr<RecordFileSystem>& files,
                       std::function<void()> handler);
  bool KeysClearedForTesting() const;

 private:
  explicit EncryptedProfile(base::FilePath path);
  void StartDecay(const std::shared_ptr<RecordFileSystem>& files);
  struct DecayState;
  std::shared_ptr<DecayState> decay_;
  std::thread decay_thread_;
  const base::FilePath path_;
  mutable std::mutex mutex_;
  bool busy_ = false;
  bool locking_ = false;
  base::UnguessableToken operation_;
  std::shared_ptr<RecordFileSystem> files_;
  // Retain a locked, wiped coordinator so tests can observe actual buffers.
  std::shared_ptr<RecordFileSystem> locked_files_;
};
}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_ENCRYPTED_PROFILE_H_
