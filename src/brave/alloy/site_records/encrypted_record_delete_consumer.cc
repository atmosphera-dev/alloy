// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Cold native deletion in a separate process, using Chromium's own build/ABI.
// The SDK browser consumer independently observes the resulting native settings.
#include "brave/alloy/site_records/encrypted_profile.h"
#include "brave/alloy/site_records/secret_service_key_source.h"
#include "brave/alloy/site_records/site_pref_store.h"
#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/i18n/icu_util.h"
#include "base/test/task_environment.h"
#include "url/gurl.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {
constexpr char kPassword[] = "ENCRYPTED_CONTEXT_PASSWORD_32";
void Check(bool value, const char* message) {
  if (!value) { std::fprintf(stderr, "ENCRYPTED_FAIL %s\n", message); std::abort(); }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && (argc != 3 || std::string_view(argv[2]) != "--all")) return 2;
  namespace sr = alloy::site_records;
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::test::TaskEnvironment tasks;
  Check(base::i18n::InitializeICU(), "native ICU initialization");
  std::unique_ptr<sr::EncryptedProfile> profile;
  Check(sr::EncryptedProfile::Open(base::FilePath(argv[1]), &profile) == sr::FileStatus::kOk, "delete open");
  sr::PasswordKeySource password(base::as_byte_span(std::string_view(kPassword)));
  sr::SecretServiceKeySource keyring;
  sr::KeySource& source = std::getenv("ALLOY_ENCRYPTED_KEYRING") ? static_cast<sr::KeySource&>(keyring) : password;
  Check(profile->Unlock(source) == sr::FileStatus::kOk, "delete unlock");
  auto prefs = base::MakeRefCounted<sr::SitePrefStore>(profile->files(), base::BindRepeating([](sr::FileStatus status) {
    Check(status == sr::FileStatus::kOk, "delete I/O");
  }));
  Check(prefs->ReadPrefs() == PersistentPrefStore::PREF_READ_ERROR_NONE, "delete read");
  std::vector<sr::RecordInfo> records;
  if (argc == 3) {
    Check(profile->files()->ListRecords(&records) == sr::FileStatus::kOk, "list records before delete-all");
    Check(!records.empty(), "delete-all positive control");
  } else {
    sr::RecordInfo alpha;
    alpha.domain = "alpha.test";
    records.push_back(std::move(alpha));
  }
  for (const auto& record : records) {
    sr::RecordDeletion deletion;
    Check(prefs->DeleteSite(GURL("https://"+record.domain+"/"), {}, &deletion) == sr::FileStatus::kOk,
          "real encrypted record deletion");
  }
  if (argc == 3) {
    Check(profile->files()->ListRecords(&records) == sr::FileStatus::kOk && records.empty(),
          "delete-all empty record set");
  }
  tasks.RunUntilIdle();
  prefs.reset();
  tasks.RunUntilIdle();
  Check(profile->Lock() == sr::FileStatus::kOk, "delete lock");
  std::puts("PASS encrypted-native-record-delete");
  return 0;
}
