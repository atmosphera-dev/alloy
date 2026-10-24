// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/secret_service_key_source.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "base/command_line.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "brave/alloy/site_records/encrypted_profile.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
// Opt-in only through tools/test_keyring.py's private session. Ordinary suite
// invocation must never inspect the builder's or a developer's real keyring.
bool PrivateKeyring() {
  const char* marker = std::getenv("ALLOY_TEST_KEYRING");
  const char* home = std::getenv("HOME");
  const char* address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
  return marker && home && address && std::string(marker) == home &&
      std::string(home).starts_with("/home/builder/work/alloy-b/") &&
      std::string(address).starts_with("unix:");
}

TEST(SecretServiceKeySourceTest, PerProfileKeysLookupAndDeletion) {
  if (!PrivateKeyring()) GTEST_SKIP() << "Requires owned private test keyring";
  KeySourceParameters a, b, absent;
  a.kind = b.kind = absent.kind = KeySourceKind::kSecretService;
  ASSERT_EQ(FileStatus::kOk, RandomId(&a.salt));
  ASSERT_EQ(FileStatus::kOk, RandomId(&b.salt));
  ASSERT_EQ(FileStatus::kOk, RandomId(&absent.salt));
  SecretServiceKeySource source;
  SecretKey first, second, reread;
  ASSERT_EQ(FileStatus::kOk, source.Create(a, &first));
  ASSERT_EQ(FileStatus::kOk, source.Create(b, &second));
  EXPECT_FALSE(std::ranges::equal(first.bytes(), second.bytes()));
  ASSERT_EQ(FileStatus::kOk, source.Derive(a, &reread));
  EXPECT_TRUE(std::ranges::equal(first.bytes(), reread.bytes()));
  EXPECT_EQ(FileStatus::kDenied, source.Create(a, &reread));
  EXPECT_TRUE(std::ranges::all_of(reread.bytes(), [](auto b) { return b == 0; }));
  EXPECT_EQ(FileStatus::kDenied, source.Derive(absent, &reread));
  EXPECT_EQ(KeyringResult::kMissing, source.result());
  std::string output;
  int exit = -1;
  ASSERT_TRUE(base::GetAppOutputWithExitCode(base::CommandLine({
      "/usr/bin/timeout", "10", "/usr/bin/secret-tool", "clear", "application",
      "alloy", "xdg:schema", "org.alloy.ProfileKey.v1", "profile", ObjectName(a.salt)}),
      &output, &exit));
  ASSERT_EQ(0, exit);
  EXPECT_EQ(FileStatus::kDenied, source.Derive(a, &reread));
  EXPECT_EQ(KeyringResult::kMissing, source.result());
  EXPECT_TRUE(std::ranges::all_of(reread.bytes(), [](auto b) { return b == 0; }));
  ASSERT_EQ(FileStatus::kOk, source.Derive(b, &reread));
  EXPECT_TRUE(std::ranges::equal(second.bytes(), reread.bytes()));
}

TEST(SecretServiceKeySourceTest, ProfileReopenLockWipesAndRevokesHandles) {
  if (!PrivateKeyring()) GTEST_SKIP() << "Requires owned private test keyring";
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  const auto path = temp.GetPath().AppendASCII("keyring-profile");
  SecretServiceKeySource source;
  std::unique_ptr<EncryptedProfile> profile;
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path, source, &profile));
  const GURL url("https://keyring-private.example/");
  RecordInfo record;
  ASSERT_EQ(FileStatus::kOk, profile->files()->ResolveRecord(
      url, url::Origin::Create(url), true, &record));
  auto cap = profile->files()->Capability(record.id, StorePurpose::kIndexedDb);
  std::shared_ptr<RecordFile> file;
  ASSERT_EQ(FileStatus::kOk, profile->files()->OpenFile(cap, StorePurpose::kIndexedDb,
      "database", RecordOpen::kCreate, &file));
  const std::string marker = "KEYRING_PRIVATE_DATA_7382";
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(marker)));
  ASSERT_EQ(FileStatus::kOk, profile->Lock());
  EXPECT_TRUE(profile->KeysClearedForTesting());
  EXPECT_TRUE(file->KeysClearedForTesting());
  EXPECT_FALSE(cap.active());
  EXPECT_EQ(FileStatus::kDenied, file->Write(0, base::as_byte_span(marker)));
  profile.reset();
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Open(path, &profile));
  PasswordKeySource wrong(base::as_byte_span(std::string_view("wrong")));
  EXPECT_EQ(FileStatus::kDenied, profile->Unlock(wrong));
  ASSERT_EQ(FileStatus::kOk, profile->Unlock(source));
  cap = profile->files()->Capability(record.id, StorePurpose::kIndexedDb);
  ASSERT_EQ(FileStatus::kOk, profile->files()->OpenFile(cap, StorePurpose::kIndexedDb,
      "database", RecordOpen::kOpen, &file));
  std::vector<uint8_t> bytes(100);
  size_t size = 0;
  ASSERT_EQ(FileStatus::kOk, file->Read(0, bytes, &size));
  EXPECT_EQ(marker, std::string(bytes.begin(), bytes.begin() + size));
}

TEST(SecretServiceKeySourceTest, SourceMismatchClearsOutputWithoutCallingProvider) {
  SecretKey output;
  ASSERT_EQ(FileStatus::kOk, RandomKey(&output));
  SecretServiceKeySource source;
  EXPECT_EQ(FileStatus::kDenied, source.Derive(KeySourceParameters(), &output));
  EXPECT_EQ(KeyringResult::kFailed, source.result());
  EXPECT_TRUE(std::ranges::all_of(output.bytes(), [](auto b) { return b == 0; }));
}
}  // namespace
}  // namespace alloy::site_records
