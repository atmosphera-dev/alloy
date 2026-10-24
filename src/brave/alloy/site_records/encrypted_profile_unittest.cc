// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_profile.h"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "argon2.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr std::string_view kPassword = "a-profile-password-for-tests";

class ExternalSource : public KeySource {
 public:
  KeySourceKind kind() const override { return KeySourceKind::kExternal; }
  FileStatus Derive(const KeySourceParameters& parameters, SecretKey* key) override {
    ++calls;
    if (callback) {
      callback();
    }
    ObjectId material;
    material.fill(0x93);
    SecretKey root(material);
    *key = DeriveKey(root, parameters.salt,
                     base::as_byte_span(std::string_view("test-external-source")));
    return FileStatus::kOk;
  }
  int calls = 0;
  std::function<void()> callback;
};

TEST(PasswordKeySourceTest, OfficialReferenceArgon2idKnownAnswer) {
  // Verbatim parameters/expected digest from pinned upstream src/test.c.
  ObjectId output{};
  const std::string password = "password";
  const std::string salt = "somesalt";
  ASSERT_EQ(ARGON2_OK, argon2id_hash_raw(2, 256, 2, password.data(), password.size(),
      salt.data(), salt.size(), output.data(), output.size()));
  EXPECT_EQ("6d093c501fd5999645e0ea3bf620d7b8be7fd2db59c20d9fff9539da2bf57037",
            ObjectName(output));
  Cleanse(output);
}

TEST(PasswordKeySourceTest, ProductionParametersAreDeterministicAndOneShot) {
  KeySourceParameters parameters;
  ASSERT_EQ(FileStatus::kOk, RandomId(&parameters.salt));
  PasswordKeySource first(base::as_byte_span(kPassword));
  PasswordKeySource second(base::as_byte_span(kPassword));
  SecretKey a;
  SecretKey b;
  ASSERT_EQ(FileStatus::kOk, first.Derive(parameters, &a));
  ASSERT_EQ(FileStatus::kOk, second.Derive(parameters, &b));
  EXPECT_TRUE(std::ranges::equal(a.bytes(), b.bytes()));
  EXPECT_TRUE(first.PasswordClearedForTesting());
  EXPECT_TRUE(second.PasswordClearedForTesting());
  EXPECT_EQ(FileStatus::kDenied, first.Derive(parameters, &a));
  EXPECT_TRUE(std::ranges::all_of(a.bytes(), [](auto byte) { return byte == 0; }));
  auto encoded = EncodeKeySource(parameters);
  base::span(encoded)[15] ^= 1;  // A different memory cost is not accepted.
  EXPECT_EQ(FileStatus::kIntegrity, DecodeKeySource(encoded, &parameters));
}

class EncryptedProfileTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
  }
  void CreatePassword() {
    PasswordKeySource source(base::as_byte_span(kPassword));
    ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path_, source, &profile_));
    EXPECT_TRUE(source.PasswordClearedForTesting());
    ASSERT_TRUE(profile_->unlocked());
  }
  void Site(RecordInfo* record) {
    const GURL url("https://private.example/");
    ASSERT_EQ(FileStatus::kOk, profile_->files()->ResolveRecord(url,
        url::Origin::Create(url), true, record));
  }
  std::string Read(const std::shared_ptr<RecordFile>& file) {
    std::vector<uint8_t> bytes(100);
    size_t size = 0;
    EXPECT_EQ(FileStatus::kOk, file->Read(0, bytes, &size));
    return std::string(bytes.begin(), bytes.begin() + size);
  }
  base::ScopedTempDir temp_;
  base::FilePath path_;
  std::unique_ptr<EncryptedProfile> profile_;
};

TEST_F(EncryptedProfileTest, WrongPasswordCannotReadOrWriteAndLockWipesKeys) {
  CreatePassword();
  RecordInfo record;
  Site(&record);
  auto files = profile_->files();
  auto cap = files->Capability(record.id, StorePurpose::kIndexedDb);
  std::shared_ptr<RecordFile> data;
  ASSERT_EQ(FileStatus::kOk, files->OpenFile(cap, StorePurpose::kIndexedDb,
      "database", RecordOpen::kCreate, &data));
  ASSERT_EQ(FileStatus::kOk,
            data->Write(0, base::as_byte_span(std::string_view("site-secret"))));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  EXPECT_EQ(ProfileState::kLocked, profile_->state());
  EXPECT_TRUE(profile_->KeysClearedForTesting());
  EXPECT_TRUE(data->KeysClearedForTesting());
  EXPECT_FALSE(cap.active());
  EXPECT_EQ(FileStatus::kDenied, data->SetLength(1));
  EXPECT_EQ(FileStatus::kDenied,
            data->Write(0, base::as_byte_span(std::string_view("blocked"))));
  std::vector<uint8_t> bytes(20, 0xff);
  size_t read = 1;
  EXPECT_EQ(FileStatus::kDenied, data->Read(0, bytes, &read));
  EXPECT_EQ(0u, read);
  PasswordKeySource wrong(base::as_byte_span(std::string_view("wrong-password")));
  EXPECT_EQ(FileStatus::kIntegrity, profile_->Unlock(wrong));
  EXPECT_TRUE(wrong.PasswordClearedForTesting());
  EXPECT_EQ(ProfileState::kLocked, profile_->state());
  EXPECT_FALSE(profile_->files());
  EXPECT_TRUE(profile_->KeysClearedForTesting());
  PasswordKeySource correct(base::as_byte_span(kPassword));
  ASSERT_EQ(FileStatus::kOk, profile_->Unlock(correct));
  cap = profile_->files()->Capability(record.id, StorePurpose::kIndexedDb);
  ASSERT_EQ(FileStatus::kOk, profile_->files()->OpenFile(cap,
      StorePurpose::kIndexedDb, "database", RecordOpen::kOpen, &data));
  EXPECT_EQ("site-secret", Read(data));
}

TEST_F(EncryptedProfileTest, ProfilePrivateStateSurvivesSiteDeletionAndReopen) {
  CreatePassword();
  auto files = profile_->files();
  auto cap = files->ProfileCapability(StorePurpose::kProfileSettings);
  ASSERT_TRUE(cap.active());
  std::shared_ptr<RecordFile> settings;
  ASSERT_EQ(FileStatus::kOk, files->OpenFile(cap, StorePurpose::kProfileSettings,
      "profile-settings", RecordOpen::kCreate, &settings));
  const std::string marker = "PRIVATE_PROFILE_SETTING_1a7f1903";
  ASSERT_EQ(FileStatus::kOk, settings->Write(0, base::as_byte_span(marker)));
  RecordInfo record;
  Site(&record);
  RecordDeletion deletion;
  ASSERT_EQ(FileStatus::kOk, files->DeleteRecord(record.id, {}, &deletion));
  EXPECT_EQ(RecordDeletion::kDeleted, deletion);
  EXPECT_EQ(FileStatus::kDenied, files->DeleteRecord(cap.record(), {}, &deletion));
  std::vector<RecordInfo> records;
  ASSERT_EQ(FileStatus::kOk, files->ListRecords(&records));
  EXPECT_TRUE(records.empty());
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  EXPECT_TRUE(settings->KeysClearedForTesting());
  profile_.reset();
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Open(path_, &profile_));
  EXPECT_EQ(ProfileState::kLocked, profile_->state());
  PasswordKeySource password(base::as_byte_span(kPassword));
  ASSERT_EQ(FileStatus::kOk, profile_->Unlock(password));
  cap = profile_->files()->ProfileCapability(StorePurpose::kProfileSettings);
  ASSERT_EQ(FileStatus::kOk, profile_->files()->OpenFile(cap,
      StorePurpose::kProfileSettings, "profile-settings", RecordOpen::kOpen, &settings));
  EXPECT_EQ(marker, Read(settings));
}

TEST_F(EncryptedProfileTest, NoDomainNameMetadataOrFilePayloadIsPlaintext) {
  CreatePassword();
  RecordInfo record;
  Site(&record);
  auto files = profile_->files();
  const std::string metadata = "SITE_METADATA_7af53e6b-4ae0";
  ASSERT_EQ(FileStatus::kOk,
            files->SetRecordMetadata(record.id, base::as_byte_span(metadata)));
  std::shared_ptr<RecordFile> file;
  const auto cap = files->Capability(record.id, StorePurpose::kHistory);
  const std::string payload = "HISTORY_TITLE_163cc87f9c32";
  ASSERT_EQ(FileStatus::kOk, files->OpenFile(cap, StorePurpose::kHistory,
      "History.sqlite", RecordOpen::kCreate, &file));
  ASSERT_EQ(FileStatus::kOk, file->Write(0, base::as_byte_span(payload)));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  EXPECT_NE(ObjectName(Digest(base::as_byte_span(record.domain))),
            ObjectName(record.directory));
  base::FileEnumerator physical(path_, true, base::FileEnumerator::FILES);
  for (auto path = physical.Next(); !path.empty(); path = physical.Next()) {
    const auto bytes = base::ReadFileToBytes(path);
    ASSERT_TRUE(bytes);
    const std::string body(bytes->begin(), bytes->end());
    for (const auto& text : {record.domain, metadata, payload,
                            std::string(kPassword), std::string("History.sqlite")}) {
      EXPECT_EQ(std::string::npos, body.find(text));
      EXPECT_EQ(std::string::npos, path.value().find(text));
    }
  }
}

TEST_F(EncryptedProfileTest, ExternalSourceReentersStateAndCancelsUnlock) {
  ExternalSource source;
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path_, source, &profile_));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  source.callback = [&] {
    EXPECT_EQ(ProfileState::kTransition, profile_->state());
    EXPECT_EQ(FileStatus::kBusy, profile_->Lock());
    EXPECT_FALSE(profile_->files());
  };
  EXPECT_EQ(FileStatus::kDenied, profile_->Unlock(source));
  EXPECT_EQ(ProfileState::kLocked, profile_->state());
  EXPECT_TRUE(profile_->KeysClearedForTesting());
  source.callback = {};
  ASSERT_EQ(FileStatus::kOk, profile_->Unlock(source));
  EXPECT_EQ(ProfileState::kUnlocked, profile_->state());
}

TEST_F(EncryptedProfileTest, ConcurrentUnlockCancellationCannotPublishLateKeys) {
  ExternalSource source;
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path_, source, &profile_));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool release = false;
  source.callback = [&] {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return release; });
  };
  FileStatus result = FileStatus::kOk;
  std::thread worker([&] { result = profile_->Unlock(source); });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return entered; });
  }
  EXPECT_EQ(FileStatus::kBusy, profile_->Lock());
  EXPECT_EQ(ProfileState::kTransition, profile_->state());
  ExternalSource second;
  EXPECT_EQ(FileStatus::kBusy, profile_->Unlock(second));
  EXPECT_EQ(0, second.calls);
  {
    std::lock_guard lock(mutex);
    release = true;
    changed.notify_all();
  }
  worker.join();
  EXPECT_EQ(FileStatus::kDenied, result);
  EXPECT_EQ(ProfileState::kLocked, profile_->state());
  EXPECT_TRUE(profile_->KeysClearedForTesting());
}

TEST_F(EncryptedProfileTest, TamperedKdfDescriptorIsRejectedBeforeProviderCall) {
  ExternalSource source;
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path_, source, &profile_));
  ASSERT_EQ(FileStatus::kOk, profile_->Lock());
  const auto path = path_.AppendASCII("root");
  auto bytes = base::ReadFileToBytes(path);
  ASSERT_TRUE(bytes);
  // Root framing is 44 bytes; descriptor byte 15 is a reserved external KDF
  // field. Changing it must fail before invoking any external credential code.
  base::span(*bytes)[44 + 15] ^= 1;
  ASSERT_TRUE(base::WriteFile(path, *bytes));
  EXPECT_EQ(FileStatus::kIntegrity, profile_->Unlock(source));
  EXPECT_EQ(1, source.calls);  // Creation only.
  EXPECT_TRUE(profile_->KeysClearedForTesting());
}

TEST_F(EncryptedProfileTest, PrivateCapabilitiesCannotAdmitSiteActivityStores) {
  ExternalSource source;
  ASSERT_EQ(FileStatus::kOk, EncryptedProfile::Create(path_, source, &profile_));
  const auto files = profile_->files();
  for (auto purpose : {StorePurpose::kCookies, StorePurpose::kHistory,
                       StorePurpose::kIndexedDb, StorePurpose::kSiteSettings,
                       StorePurpose::kHttpCache}) {
    EXPECT_FALSE(files->ProfileCapability(purpose).active());
  }
  const auto private_cap = files->ProfileCapability(StorePurpose::kProfileSettings);
  ASSERT_TRUE(private_cap.active());
  EXPECT_FALSE(files->Capability(private_cap.record(), StorePurpose::kCookies).active());
  RecordCapabilityIssuer wrong_owner(private_cap.profile(), private_cap.record(),
      private_cap.generation(), StorageMode::kEncrypted);
  std::shared_ptr<RecordFile> file;
  EXPECT_EQ(FileStatus::kDenied, files->OpenFile(
      wrong_owner.Issue(StorePurpose::kCookies), StorePurpose::kCookies,
      "Cookies.sqlite", RecordOpen::kCreate, &file));
  RecordInfo site;
  Site(&site);
  EXPECT_FALSE(files->Capability(site.id, StorePurpose::kProfileSettings).active());
}
}  // namespace
}  // namespace alloy::site_records
