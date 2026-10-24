// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "cef/include/alloy/alloy_profile.h"

#include <memory>
#include <limits>
#include <utility>
#include <vector>

#include "brave/alloy/site_records/encrypted_profile.h"
#include "cef/libcef/common/api_version_util.h"
#include "third_party/icu/source/common/unicode/ustring.h"

namespace {
namespace sr = alloy::site_records;

std::unique_ptr<sr::PasswordKeySource> PasswordSource(const CefString& password) {
  // Convert into one caller-sized buffer. CefString::ToString and the generic
  // shrinking string conversion helpers create freed, uncleansed UTF-8 copies.
  // ICU writes directly into this owned buffer and rejects malformed UTF-16.
  if (password.length() > static_cast<size_t>(std::numeric_limits<int32_t>::max() / 3)) {
    return nullptr;
  }
  std::vector<uint8_t> utf8(password.length() * 3 + 1, 0);
  int32_t length = 0;
  UErrorCode error = U_ZERO_ERROR;
  u_strToUTF8(reinterpret_cast<char*>(utf8.data()),
      static_cast<int32_t>(utf8.size()), &length,
      password.c_str(), static_cast<int32_t>(password.length()), &error);
  std::unique_ptr<sr::PasswordKeySource> source;
  if (U_SUCCESS(error)) {
    source = std::make_unique<sr::PasswordKeySource>(
        base::span(utf8).first(static_cast<size_t>(length)));
  }
  sr::Cleanse(utf8);
  return source;
}

class ExternalKeySource final : public sr::KeySource {
 public:
  explicit ExternalKeySource(CefRefPtr<AlloyKeySource> source)
      : source_(std::move(source)) {}
  sr::KeySourceKind kind() const override { return sr::KeySourceKind::kExternal; }
  sr::FileStatus Derive(const sr::KeySourceParameters& parameters,
                        sr::SecretKey* key) override {
    key->Clear();
    if (!source_ || parameters.kind != kind()) {
      return sr::FileStatus::kDenied;
    }
    sr::ObjectId output{};
    const bool success = source_->DeriveKey(parameters.salt.data(),
        parameters.salt.size(), output.data(), output.size());
    if (success) {
      *key = sr::SecretKey(output);
    }
    sr::Cleanse(output);
    return success ? sr::FileStatus::kOk : sr::FileStatus::kDenied;
  }
 private:
  CefRefPtr<AlloyKeySource> source_;
};

class AlloyProfileImpl final : public AlloyProfile {
 public:
  explicit AlloyProfileImpl(std::unique_ptr<sr::EncryptedProfile> profile)
      : profile_(std::move(profile)) {}
  bool UnlockWithPassword(const CefString& password) override {
    auto source = PasswordSource(password);
    return source && profile_->Unlock(*source) == sr::FileStatus::kOk;
  }
  bool UnlockWithKeySource(CefRefPtr<AlloyKeySource> source) override {
    ExternalKeySource bridge(std::move(source));
    return profile_->Unlock(bridge) == sr::FileStatus::kOk;
  }
  bool Lock() override { return profile_->Lock() == sr::FileStatus::kOk; }
  alloy_profile_state_t GetState() override {
    switch (profile_->state()) {
      case sr::ProfileState::kLocked: return ALLOY_PROFILE_LOCKED;
      case sr::ProfileState::kUnlocked: return ALLOY_PROFILE_UNLOCKED;
      case sr::ProfileState::kTransition: return ALLOY_PROFILE_TRANSITION;
      case sr::ProfileState::kError: return ALLOY_PROFILE_ERROR;
    }
    return ALLOY_PROFILE_ERROR;
  }
 private:
  ~AlloyProfileImpl() override = default;
  std::unique_ptr<sr::EncryptedProfile> profile_;
  IMPLEMENT_REFCOUNTING(AlloyProfileImpl);
};

base::FilePath ProfilePath(const CefString& path) {
  return base::FilePath::FromUTF8Unsafe(path.ToString());
}
}  // namespace

CefRefPtr<AlloyProfile> AlloyCreateProfile(const CefString& path,
                                           const CefString& password) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  auto source = PasswordSource(password);
  if (!source) return nullptr;
  std::unique_ptr<sr::EncryptedProfile> profile;
  return sr::EncryptedProfile::Create(ProfilePath(path), *source, &profile) == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}

CefRefPtr<AlloyProfile> AlloyCreateProfileWithKeySource(
    const CefString& path, CefRefPtr<AlloyKeySource> source) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  ExternalKeySource bridge(std::move(source));
  std::unique_ptr<sr::EncryptedProfile> profile;
  return sr::EncryptedProfile::Create(ProfilePath(path), bridge, &profile) == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}

CefRefPtr<AlloyProfile> AlloyOpenProfile(const CefString& path) {
  CEF_API_REQUIRE_ADDED(CEF_EXPERIMENTAL);
  std::unique_ptr<sr::EncryptedProfile> profile;
  return sr::EncryptedProfile::Open(ProfilePath(path), &profile) == sr::FileStatus::kOk
      ? CefRefPtr<AlloyProfile>(new AlloyProfileImpl(std::move(profile))) : nullptr;
}
