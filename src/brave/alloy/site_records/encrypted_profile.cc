// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_profile.h"

#include <utility>

namespace alloy::site_records {
namespace {
constexpr size_t kDescriptorSize = 56;
constexpr size_t kEnvelopeSize = 80;

FileStatus ParseBootstrap(base::span<const uint8_t> bootstrap,
                           KeySourceParameters* parameters) {
  return bootstrap.size() == kDescriptorSize + kEnvelopeSize
      ? DecodeKeySource(bootstrap.first(kDescriptorSize), parameters)
      : FileStatus::kIntegrity;
}
}  // namespace

EncryptedProfile::EncryptedProfile(base::FilePath path) : path_(std::move(path)) {}
EncryptedProfile::~EncryptedProfile() { Lock(); }

FileStatus EncryptedProfile::Create(const base::FilePath& path, KeySource& source,
                                     std::unique_ptr<EncryptedProfile>* out) {
  KeySourceParameters parameters;
  parameters.kind = source.kind();
  auto status = RandomId(&parameters.salt);
  if (status != FileStatus::kOk) {
    return status;
  }
  auto descriptor = EncodeKeySource(parameters);
  KeySourceParameters validated;
  status = DecodeKeySource(descriptor, &validated);
  if (status != FileStatus::kOk) {
    return status;
  }
  SecretKey kek;
  status = source.Derive(parameters, &kek);
  if (status != FileStatus::kOk) {
    return status;
  }
  SecretKey profile_key;
  status = RandomKey(&profile_key);
  if (status != FileStatus::kOk) {
    return status;
  }
  std::vector<uint8_t> envelope;
  status = WrapKey(kek, profile_key, descriptor, &envelope);
  kek.Clear();
  if (status != FileStatus::kOk) {
    return status;
  }
  descriptor.insert(descriptor.end(), envelope.begin(), envelope.end());
  auto profile = std::unique_ptr<EncryptedProfile>(new EncryptedProfile(path));
  status = RecordFileSystem::Create(path, std::move(profile_key),
                                      std::move(descriptor), &profile->files_);
  if (status == FileStatus::kOk) {
    *out = std::move(profile);
  }
  return status;
}

FileStatus EncryptedProfile::Open(const base::FilePath& path,
                                   std::unique_ptr<EncryptedProfile>* out) {
  std::vector<uint8_t> bootstrap;
  auto status = RecordFileSystem::ReadBootstrap(path, &bootstrap);
  KeySourceParameters parameters;
  if (status == FileStatus::kOk) {
    status = ParseBootstrap(bootstrap, &parameters);
  }
  if (status == FileStatus::kOk) {
    out->reset(new EncryptedProfile(path));
  }
  return status;
}

FileStatus EncryptedProfile::Unlock(KeySource& source) {
  base::UnguessableToken operation;
  {
    std::lock_guard lock(mutex_);
    if (busy_ || locking_) {
      return FileStatus::kBusy;
    }
    if (files_) {
      return FileStatus::kExists;
    }
    busy_ = true;
    operation_ = operation = base::UnguessableToken::Create();
  }
  // Source callbacks can reenter state/Lock. A cancellation invalidates this
  // operation, but leaves busy set until its keys and opened store are wiped.
  std::shared_ptr<RecordFileSystem> opened;
  auto status = [&] {
    std::vector<uint8_t> bootstrap;
    auto result = RecordFileSystem::ReadBootstrap(path_, &bootstrap);
    KeySourceParameters parameters;
    if (result == FileStatus::kOk) {
      result = ParseBootstrap(bootstrap, &parameters);
    }
    if (result != FileStatus::kOk || parameters.kind != source.kind()) {
      return result == FileStatus::kOk ? FileStatus::kDenied : result;
    }
    SecretKey kek;
    result = source.Derive(parameters, &kek);
    if (result != FileStatus::kOk) {
      return result;
    }
    SecretKey profile_key;
    result = UnwrapKey(kek, base::span(bootstrap).last(kEnvelopeSize),
                        base::span(bootstrap).first(kDescriptorSize), &profile_key);
    kek.Clear();
    if (result != FileStatus::kOk) {
      // Wrong credentials or an altered envelope both fail authentication.
      return result;
    }
    result = RecordFileSystem::Open(path_, std::move(profile_key), &opened);
    if (result == FileStatus::kOk && opened->bootstrap() != bootstrap) {
      opened->Lock();
      opened.reset();
      return FileStatus::kIntegrity;
    }
    return result;
  }();
  std::shared_ptr<RecordFileSystem> old_locked;
  bool cleanup_pending = false;
  {
    std::lock_guard lock(mutex_);
    if (operation_ != operation) {
      status = FileStatus::kDenied;
    }
    if (status == FileStatus::kOk) {
      old_locked = std::move(locked_files_);
      files_ = std::move(opened);
    }
    cleanup_pending = static_cast<bool>(opened);
    if (!cleanup_pending) {
      busy_ = false;
    }
  }
  if (opened) {
    opened->Lock();
  }
  if (cleanup_pending) {
    std::lock_guard lock(mutex_);
    busy_ = false;
  }
  return status;
}

FileStatus EncryptedProfile::Lock() {
  std::shared_ptr<RecordFileSystem> files;
  {
    std::lock_guard lock(mutex_);
    operation_ = base::UnguessableToken::Create();
    if (busy_ || locking_) {
      // Cancel an outstanding unlock, but do not acknowledge key destruction
      // while its provider is still running. In particular this is safe when
      // called reentrantly by that provider: waiting here would deadlock it.
      return FileStatus::kBusy;
    }
    locking_ = true;
    files = std::move(files_);
  }
  const auto status = files ? files->Lock() : FileStatus::kOk;
  if (files) {
    std::shared_ptr<RecordFileSystem> old;
    {
      std::lock_guard lock(mutex_);
      old = std::move(locked_files_);
      locked_files_ = std::move(files);
    }
  }
  {
    std::lock_guard lock(mutex_);
    locking_ = false;
  }
  return status;
}

bool EncryptedProfile::unlocked() const {
  return state() == ProfileState::kUnlocked;
}
ProfileState EncryptedProfile::state() const {
  std::lock_guard lock(mutex_);
  if (busy_ || locking_) {
    return ProfileState::kTransition;
  }
  return !files_ ? ProfileState::kLocked
                : files_->unlocked() ? ProfileState::kUnlocked : ProfileState::kError;
}
std::shared_ptr<RecordFileSystem> EncryptedProfile::files() const {
  std::lock_guard lock(mutex_);
  return files_;
}
bool EncryptedProfile::KeysClearedForTesting() const {
  std::shared_ptr<RecordFileSystem> locked;
  {
    std::lock_guard lock(mutex_);
    if (files_ || busy_ || locking_) {
      return false;
    }
    locked = locked_files_;
  }
  return !locked || locked->KeysClearedForTesting();
}
}  // namespace alloy::site_records
