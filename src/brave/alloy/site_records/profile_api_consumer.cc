// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// An SDK-side consumer: use the generated C++ wrapper, not engine internals.
#include "include/alloy/alloy_profile.h"
#include "include/cef_api_hash.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
int failures = 0;
constexpr char kPassword[] = "CPP_PROFILE_PASSWORD_09fd1d7c\xc3\xa4\xf0\x9f\x8c\xb1";
void Check(bool condition, const char* name) {
  if (!condition) {
    std::fprintf(stderr, "FAIL cpp-profile-%s\n", name);
    ++failures;
  }
}

class Source final : public AlloyKeySource {
 public:
  CefRefPtr<AlloyProfile> profile;
  int calls = 0;
  bool cancel = false;
  bool DeriveKey(const void* salt, size_t salt_size,
                   void* key, size_t key_size) override {
    ++calls;
    if (salt_size != 32 || key_size != 32) return false;
    if (profile) {
      Check(profile->GetState() == ALLOY_PROFILE_TRANSITION, "callback-state");
      if (cancel) Check(!profile->Lock(), "callback-cancel");
    }
    std::array<uint8_t, 32> input{};
    std::array<uint8_t, 32> output{};
    std::memcpy(input.data(), salt, input.size());
    for (size_t i = 0; i < output.size(); ++i) output[i] = input[i] ^ 0xa6;
    std::memcpy(key, output.data(), output.size());
    return true;
  }
 private:
  ~Source() override = default;
  IMPLEMENT_REFCOUNTING(Source);
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 2;
  const auto* hash = cef_api_hash(CEF_API_VERSION, 0);
  Check(hash && std::strcmp(hash, CEF_API_HASH_PLATFORM) == 0, "api-hash");
  if (failures) return 1;
  if (argc == 3) {
    if (std::strcmp(argv[1], "--keyring")) return 2;
    int result = ALLOY_KEYRING_FAILED;
    auto keyring_profile = AlloyCreateProfileWithKeyring(argv[2], result);
    Check(keyring_profile && result == ALLOY_KEYRING_OK, "keyring-create");
    if (!keyring_profile) return 1;
    Check(keyring_profile->Lock(), "keyring-lock");
    keyring_profile = nullptr;
    keyring_profile = AlloyOpenProfile(argv[2]);
    Check(static_cast<bool>(keyring_profile), "keyring-reopen");
    if (!keyring_profile) return 1;
    Check(keyring_profile->UnlockWithKeyring() == ALLOY_KEYRING_OK, "keyring-unlock");
    Check(keyring_profile->GetState() == ALLOY_PROFILE_UNLOCKED, "keyring-state");
    Check(keyring_profile->Lock(), "keyring-relock");
    if (failures) return 1;
    std::puts("PASS profile-api-keyring-cpp-wrapper");
    return 0;
  }
  CefString malformed;
  malformed.FromString16(std::u16string(1, static_cast<char16_t>(0xd800)));
  Check(!AlloyCreateProfile(std::string(argv[1]) + "/invalid-unicode", malformed),
        "malformed-password-refused");
  const std::string path = std::string(argv[1]) + "/cpp-password";
  auto profile = AlloyCreateProfile(path, kPassword);
  Check(static_cast<bool>(profile), "create");
  if (!profile) return 1;
  Check(profile->GetState() == ALLOY_PROFILE_UNLOCKED, "created-state");
  int64_t period = -1; int timebase = -1;
  Check(profile->GetDecaySettings(period, timebase) && period == 0 &&
      timebase == ALLOY_DECAY_SINCE_LAST_VISIT, "decay-no-default");
  Check(profile->SetDecaySettings(86400, ALLOY_DECAY_SINCE_LAST_VISIT), "decay-persist");
  Check(profile->Lock(), "lock");
  Check(!profile->UnlockWithPassword("wrong-password"), "wrong-password");
  profile = nullptr;
  profile = AlloyOpenProfile(path);
  Check(static_cast<bool>(profile), "reopen");
  if (!profile) return 1;
  Check(profile->GetState() == ALLOY_PROFILE_LOCKED, "reopen-state");
  Check(profile->UnlockWithPassword(kPassword), "unicode-unlock");
  Check(profile->GetDecaySettings(period, timebase) && period == 86400, "decay-cold-read");
  profile = nullptr;

  const std::string external_path = std::string(argv[1]) + "/cpp-external";
  CefRefPtr<Source> source = new Source();
  profile = AlloyCreateProfileWithKeySource(external_path, source);
  Check(static_cast<bool>(profile), "external-create");
  if (!profile) return 1;
  Check(source->calls == 1 && source->HasOneRef(), "source-lifetime");
  Check(profile->Lock(), "external-lock");
  source->profile = profile;
  source->cancel = true;
  Check(!profile->UnlockWithKeySource(source), "external-cancel");
  Check(profile->GetState() == ALLOY_PROFILE_LOCKED, "cancel-state");
  source->cancel = false;
  Check(profile->UnlockWithKeySource(source), "external-unlock");
  Check(source->HasOneRef(), "source-lifetime-after-unlock");
  source->profile = nullptr;
  profile = nullptr;
  profile = AlloyOpenProfile(external_path);
  Check(static_cast<bool>(profile), "external-reopen");
  if (profile) Check(profile->GetState() == ALLOY_PROFILE_LOCKED, "external-reopen-state");
  profile = nullptr;
  if (failures) return 1;
  std::puts("PASS profile-api-cpp-wrapper");
  return 0;
}
