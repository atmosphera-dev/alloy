// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Native cold-process record consumer; API consumer separately uses generated ABI.
#include "brave/alloy/site_records/encrypted_profile.h"
#include "brave/alloy/site_records/secret_service_key_source.h"
#include "brave/alloy/site_records/record_sqlite_vfs.h"
#include "brave/alloy/site_records/record_leveldb_env.h"
#include "brave/alloy/site_records/record_cache_files.h"

#include <cstdio>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/i18n/icu_util.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "leveldb/db.h"
#include "net/base/io_buffer.h"
#include "net/base/test_completion_callback.h"
#include "sql/initialization.h"
#include "url/gurl.h"

namespace sr = alloy::site_records;
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr, "FAIL keyring-native line %d\n", __LINE__); return 1; } } while (0)

int NativeStores(const std::shared_ptr<sr::RecordFileSystem>& files,
                 const sr::RecordInfo& record, bool create) {
  const std::string value = "KEYRING_NATIVE_STORE_" + record.domain;
  {
    std::unique_ptr<sr::RecordSqliteVfs> vfs;
    REQUIRE(sr::RecordSqliteVfs::Create(files,
        files->Capability(record.id, sr::StorePurpose::kHistory),
        sr::StorePurpose::kHistory, &vfs) == sr::FileStatus::kOk);
    sqlite3* db = nullptr;
    REQUIRE(sqlite3_open_v2("history.sqlite", &db, SQLITE_OPEN_READWRITE |
        (create ? SQLITE_OPEN_CREATE : 0), vfs->name()) == SQLITE_OK);
    if (create) {
      REQUIRE(sqlite3_exec(db, ("PRAGMA journal_mode=WAL; CREATE TABLE t(value TEXT);"
          "INSERT INTO t VALUES('" + value + "')").c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    sqlite3_stmt* row = nullptr;
    REQUIRE(sqlite3_prepare_v2(db, "SELECT value FROM t", -1, &row, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(row) == SQLITE_ROW);
    REQUIRE(value == reinterpret_cast<const char*>(sqlite3_column_text(row, 0)));
    REQUIRE(sqlite3_finalize(row) == SQLITE_OK);
    REQUIRE(sqlite3_close(db) == SQLITE_OK);
  }
  {
    std::unique_ptr<sr::RecordLevelDbEnv> env;
    REQUIRE(sr::RecordLevelDbEnv::Create(files,
        files->Capability(record.id, sr::StorePurpose::kIndexedDb),
        sr::StorePurpose::kIndexedDb, &env) == sr::FileStatus::kOk);
    auto options = env->Options();
    options.create_if_missing = create;
    std::unique_ptr<leveldb::DB> db;
    REQUIRE(leveldb_env::OpenDB(options, "/indexed-db", &db).ok());
    if (create) {
      leveldb::WriteOptions write;
      write.sync = true;
      REQUIRE(db->Put(write, "key", value).ok());
    }
    std::string read;
    REQUIRE(db->Get(leveldb::ReadOptions(), "key", &read).ok());
    REQUIRE(value == read);
  }
  {
    scoped_refptr<sr::RecordCacheFileOperationsFactory> factory;
    REQUIRE(sr::RecordCacheFileOperationsFactory::Create(files,
        files->Capability(record.id, sr::StorePurpose::kHttpCache),
        sr::StorePurpose::kHttpCache, &factory) == sr::FileStatus::kOk);
    const base::FilePath path("/keyring-native-cache");
    base::test::TestFuture<disk_cache::BackendResult> opened;
    auto result = disk_cache::CreateCacheBackend(net::DISK_CACHE, net::CACHE_BACKEND_SIMPLE,
        factory, path, 1024 * 1024, disk_cache::ResetHandling::kNeverReset,
        nullptr, nullptr, opened.GetCallback());
    if (result.net_error == net::ERR_IO_PENDING) result = opened.Take();
    REQUIRE(result.net_error == net::OK && result.backend);
    base::test::TestFuture<disk_cache::EntryResult> entry_opened;
    auto entry_result = create ? result.backend->CreateEntry(record.domain, net::HIGHEST, entry_opened.GetCallback())
                              : result.backend->OpenEntry(record.domain, net::HIGHEST, entry_opened.GetCallback());
    if (entry_result.net_error() == net::ERR_IO_PENDING) entry_result = entry_opened.Take();
    REQUIRE(entry_result.net_error() == net::OK);
    disk_cache::ScopedEntryPtr entry(entry_result.ReleaseEntry());
    if (create) {
      auto input = base::MakeRefCounted<net::StringIOBuffer>(value);
      net::TestCompletionCallback written;
      REQUIRE(written.GetResult(entry->WriteData(1, 0, input.get(), value.size(),
          written.callback(), true)) == static_cast<int>(value.size()));
    }
    auto output = base::MakeRefCounted<net::IOBufferWithSize>(200);
    net::TestCompletionCallback read;
    REQUIRE(read.GetResult(entry->ReadData(1, 0, output.get(), 200, read.callback())) ==
            static_cast<int>(value.size()));
    REQUIRE(value == std::string(output->data(), value.size()));
    entry.reset();
    result.backend.reset();
    base::test::TestFuture<void> cleaned;
    disk_cache::WaitForBackendCleanupForTesting(path, cleaned.GetCallback());
    REQUIRE(cleaned.Wait());
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);
  base::test::TaskEnvironment tasks{base::test::TaskEnvironment::MainThreadType::IO};
  REQUIRE(base::i18n::InitializeICU());
  sql::EnsureSqliteInitialized();
  const std::string phase = argv[1];
  const bool create = phase == "create";
  sr::SecretServiceKeySource source;
  std::unique_ptr<sr::EncryptedProfile> profile;
  const base::FilePath path(argv[2]);
  REQUIRE((create ? sr::EncryptedProfile::Create(path, source, &profile)
                  : sr::EncryptedProfile::Open(path, &profile)) == sr::FileStatus::kOk);
  if (!create) {
    auto result = profile->Unlock(source);
    if (phase == "missing" || phase == "locked" || phase == "unavailable") {
      REQUIRE(result != sr::FileStatus::kOk);
      REQUIRE(source.result() == (phase == "missing" ? sr::KeyringResult::kMissing :
          phase == "locked" ? sr::KeyringResult::kLocked : sr::KeyringResult::kUnavailable));
      REQUIRE(profile->state() == sr::ProfileState::kLocked);
      REQUIRE(profile->KeysClearedForTesting());
      std::puts(("PASS keyring-native-" + phase).c_str());
      return 0;
    }
    REQUIRE(result == sr::FileStatus::kOk);
  }
  for (int i = 0; i < 5; ++i) {
    const std::string domain = "keyring-site-" + std::to_string(i) + ".example";
    const GURL url("https://" + domain + "/");
    const std::string value = "KEYRING_TITLE_DATA_" + std::to_string(i) + "_e3bc92";
    sr::RecordInfo record;
    REQUIRE(profile->files()->ResolveRecord(url, url::Origin::Create(url), create, &record) == sr::FileStatus::kOk);
    const auto cap = profile->files()->Capability(record.id, sr::StorePurpose::kHistory);
    std::shared_ptr<sr::RecordFile> file;
    REQUIRE(profile->files()->OpenFile(cap, sr::StorePurpose::kHistory, "History",
        create ? sr::RecordOpen::kCreate : sr::RecordOpen::kOpen, &file) == sr::FileStatus::kOk);
    if (create) REQUIRE(file->Write(0, base::as_byte_span(value)) == sr::FileStatus::kOk);
    else {
      std::vector<uint8_t> bytes(100);
      size_t size = 0;
      REQUIRE(file->Read(0, bytes, &size) == sr::FileStatus::kOk);
      REQUIRE(value == std::string(bytes.begin(), bytes.begin() + size));
    }
    REQUIRE(NativeStores(profile->files(), record, create) == 0);
  }
  {
    auto files = profile->files();
    auto cap = files->ProfileCapability(sr::StorePurpose::kProfileSettings);
    std::shared_ptr<sr::RecordFile> settings;
    REQUIRE(files->OpenFile(cap, sr::StorePurpose::kProfileSettings, "private-settings",
        create ? sr::RecordOpen::kCreate : sr::RecordOpen::kOpen, &settings) == sr::FileStatus::kOk);
    const std::string value = "KEYRING_PRIVATE_SETTINGS_3b8e7";
    if (create) REQUIRE(settings->Write(0, base::as_byte_span(value)) == sr::FileStatus::kOk);
    else {
      std::vector<uint8_t> bytes(100);
      size_t size = 0;
      REQUIRE(settings->Read(0, bytes, &size) == sr::FileStatus::kOk);
      REQUIRE(value == std::string(bytes.begin(), bytes.begin() + size));
    }
  }
  REQUIRE(profile->Lock() == sr::FileStatus::kOk);
  REQUIRE(profile->KeysClearedForTesting());
  REQUIRE(profile->Unlock(source) == sr::FileStatus::kOk);
  REQUIRE(profile->Lock() == sr::FileStatus::kOk);
  std::puts(("PASS keyring-native-" + phase).c_str());
  return 0;
}
