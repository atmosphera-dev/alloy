// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_sqlite_vfs.h"

#include <unistd.h>
#include <array>
#include <cstring>
#include <string>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "base/test/multiprocess_test.h"
#include "base/time/time.h"
#include "sql/initialization.h"
#include "sql/statement.h"
#include "sql/test/test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {
constexpr auto kPurpose = StorePurpose::kIndexedDb;
constexpr std::string_view kMarker = "SQLITE_ENCRYPTED_MARKER_19dbcf710239";
SecretKey TestKey() {
  ObjectId bytes;
  bytes.fill(0x52);
  return SecretKey(bytes);
}

}  // namespace

MULTIPROCESS_TEST_MAIN(AlloySqliteWalCrashChild) {
  const auto path = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath("record-profile");
  std::shared_ptr<RecordFileSystem> files;
  if (RecordFileSystem::Open(path, TestKey(), &files) != FileStatus::kOk) return 1;
  RecordInfo record;
  const GURL url("https://sqlite.example/");
  if (files->ResolveRecord(url, url::Origin::Create(url), false, &record) != FileStatus::kOk) return 2;
  std::unique_ptr<RecordSqliteVfs> vfs;
  if (RecordSqliteVfs::Create(files, files->Capability(record.id, kPurpose), kPurpose, &vfs) != FileStatus::kOk) return 3;
  sqlite3* db = nullptr;
  if (sqlite3_open_v2("crash.sqlite", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                       vfs->name()) != SQLITE_OK) return 4;
  const int result = sqlite3_exec(db,
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA wal_autocheckpoint=0;"
      "CREATE TABLE t(value TEXT); INSERT INTO t VALUES('SQLITE_ENCRYPTED_MARKER_19dbcf710239')",
      nullptr, nullptr, nullptr);
  // Skip close/checkpoint, profile Lock and every destructor. Reopen must use
  // the committed encrypted WAL and reconstruct its in-process index.
  _exit(result == SQLITE_OK ? 0 : 5);
}

namespace {

class RecordSqliteVfsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    path_ = temp_.GetPath().AppendASCII("profile");
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Create(path_, TestKey(), {}, &files_));
    Resolve();
    sql::EnsureSqliteInitialized();
    default_name_ = sqlite3_vfs_find(nullptr)->zName;
    ASSERT_EQ(FileStatus::kOk, RecordSqliteVfs::Create(files_, cap_, kPurpose, &vfs_));
    EXPECT_EQ(default_name_, sqlite3_vfs_find(nullptr)->zName);
  }
  void Resolve() {
    const GURL url("https://sqlite.example/");
    ASSERT_EQ(FileStatus::kOk, files_->ResolveRecord(url, url::Origin::Create(url), true, &record_));
    cap_ = files_->Capability(record_.id, kPurpose);
  }
  void Reopen() {
    vfs_.reset();
    ASSERT_EQ(FileStatus::kOk, files_->Lock());
    files_.reset();
    ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestKey(), &files_));
    Resolve();
    ASSERT_EQ(FileStatus::kOk, RecordSqliteVfs::Create(files_, cap_, kPurpose, &vfs_));
  }
  void NoPlaintext() {
    base::CommandLine command(base::FilePath("/usr/bin/timeout"));
    for (const auto* arg : {"--kill-after=5", "30", "rg", "-a", "-i", "-l"}) command.AppendArg(arg);
    command.AppendArg(std::string(kMarker));
    command.AppendArgPath(path_);
    int exit = -1;
    std::string output;
    ASSERT_TRUE(base::GetAppOutputWithExitCode(command, &output, &exit));
    EXPECT_EQ(1, exit) << output;
    EXPECT_TRUE(output.empty());
  }
  int OpenRaw(const char* name, sqlite3** db) {
    return sqlite3_open_v2(name, db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
        SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE, vfs_->name());
  }
  void Exec(sqlite3* db, const char* text) {
    ASSERT_EQ(SQLITE_OK, sqlite3_exec(db, text, nullptr, nullptr, nullptr)) << sqlite3_errmsg(db);
  }
  void CheckRow(sqlite3* db) {
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(SQLITE_OK, sqlite3_prepare_v2(db, "SELECT value FROM t", -1, &statement, nullptr));
    ASSERT_EQ(SQLITE_ROW, sqlite3_step(statement));
    EXPECT_EQ(kMarker, reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    ASSERT_EQ(SQLITE_OK, sqlite3_finalize(statement));
  }
  base::ScopedTempDir temp_;
  base::FilePath path_;
  RecordInfo record_;
  RecordCapability cap_;
  std::shared_ptr<RecordFileSystem> files_;
  std::unique_ptr<RecordSqliteVfs> vfs_;
  std::string default_name_;
};

TEST_F(RecordSqliteVfsTest, RealSqlDatabaseRollbackAndWalRoundTrip) {
  for (bool wal : {false, true}) {
    SCOPED_TRACE(wal);
    const auto name = wal ? "wal.sqlite" : "rollback.sqlite";
    {
      sql::Database db(vfs_->Options(sql::DatabaseOptions().set_wal_mode(wal)), sql::test::kTestTag);
      ASSERT_TRUE(db.Open(base::FilePath(name))) << db.GetErrorMessage() << " status=" << static_cast<int>(vfs_->last_status());
      ASSERT_TRUE(db.Execute("CREATE TABLE t(value TEXT)"));
      sql::Statement insert(db.GetUniqueStatement("INSERT INTO t VALUES(?)"));
      insert.BindString(0, kMarker);
      ASSERT_TRUE(insert.Run());
      insert.Clear();
      db.Close();
    }
    NoPlaintext();
    Reopen();
    {
      sql::Database db(vfs_->Options(sql::DatabaseOptions().set_wal_mode(wal)), sql::test::kTestTag);
      ASSERT_TRUE(db.Open(base::FilePath(name))) << db.GetErrorMessage() << " status=" << static_cast<int>(vfs_->last_status());
      sql::Statement row(db.GetUniqueStatement("SELECT value FROM t"));
      ASSERT_TRUE(row.Step());
      EXPECT_EQ(kMarker, row.ColumnString(0));
      row.Clear();
      sql::Statement mmap(db.GetUniqueStatement("PRAGMA mmap_size"));
      ASSERT_TRUE(mmap.Step());
      EXPECT_EQ(0, mmap.ColumnInt64(0));
      mmap.Clear();
      db.Close();
    }
    EXPECT_EQ(default_name_, sqlite3_vfs_find(nullptr)->zName);
  }
}

TEST_F(RecordSqliteVfsTest, SharedInProcessWalAcrossRegistrationsAndReopen) {
  std::unique_ptr<RecordSqliteVfs> other;
  ASSERT_EQ(FileStatus::kOk, RecordSqliteVfs::Create(files_, cap_, kPurpose, &other));
  sqlite3* a = nullptr;
  sqlite3* b = nullptr;
  ASSERT_EQ(SQLITE_OK, OpenRaw("shared.sqlite", &a));
  Exec(a, "PRAGMA locking_mode=NORMAL; PRAGMA journal_mode=WAL; CREATE TABLE t(value TEXT)");
  ASSERT_EQ(SQLITE_OK, sqlite3_open_v2("shared.sqlite", &b,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_PRIVATECACHE, other->name()));
  Exec(b, "PRAGMA locking_mode=NORMAL");
  Exec(a, "BEGIN IMMEDIATE; INSERT INTO t VALUES('SQLITE_ENCRYPTED_MARKER_19dbcf710239')");
  EXPECT_EQ(SQLITE_BUSY, sqlite3_exec(b, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr));
  Exec(a, "COMMIT");
  CheckRow(b);
  NoPlaintext();
  ASSERT_EQ(SQLITE_OK, sqlite3_close(b));
  ASSERT_EQ(SQLITE_OK, sqlite3_close(a));
  other.reset();
  Reopen();
  ASSERT_EQ(SQLITE_OK, OpenRaw("shared.sqlite", &a));
  CheckRow(a);
  ASSERT_EQ(SQLITE_OK, sqlite3_close(a));
  std::vector<RecordDirectoryEntry> entries;
  ASSERT_EQ(FileStatus::kOk, files_->List(cap_, kPurpose, "", &entries));
  for (const auto& entry : entries) EXPECT_FALSE(entry.name.ends_with("-shm"));
}

TEST_F(RecordSqliteVfsTest, EverySqliteFileKindUsesEncryptedNamespace) {
  const std::array<int, 8> kinds = {SQLITE_OPEN_MAIN_DB, SQLITE_OPEN_MAIN_JOURNAL,
      SQLITE_OPEN_TEMP_DB, SQLITE_OPEN_TEMP_JOURNAL, SQLITE_OPEN_TRANSIENT_DB,
      SQLITE_OPEN_SUBJOURNAL, SQLITE_OPEN_SUPER_JOURNAL, SQLITE_OPEN_WAL};
  auto* v = vfs_->vfs_for_testing();
  for (size_t i = 0; i < kinds.size(); ++i) {
    auto* file = static_cast<sqlite3_file*>(sqlite3_malloc(v->szOsFile));
    ASSERT_TRUE(file);
    const auto name = "kind-" + std::to_string(i);
    int flags = 0;
    ASSERT_EQ(SQLITE_OK, v->xOpen(v, name.c_str(), file,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | kinds[i], &flags));
    ASSERT_EQ(SQLITE_OK, file->pMethods->xWrite(file, kMarker.data(), kMarker.size(), 100));
    ASSERT_EQ(SQLITE_OK, file->pMethods->xSync(file, SQLITE_SYNC_FULL));
    std::array<char, 80> bytes;
    bytes.fill('X');
    ASSERT_EQ(SQLITE_IOERR_SHORT_READ, file->pMethods->xRead(file, bytes.data(), bytes.size(), 100));
    EXPECT_EQ(kMarker, std::string_view(bytes.data(), kMarker.size()));
    for (size_t j = kMarker.size(); j < bytes.size(); ++j) EXPECT_EQ(0, base::span(bytes)[j]);
    void* mapping = reinterpret_cast<void*>(1);
    ASSERT_EQ(SQLITE_OK, file->pMethods->xFetch(file, 0, 4096, &mapping));
    EXPECT_EQ(nullptr, mapping);
    ASSERT_EQ(SQLITE_OK, file->pMethods->xClose(file));
    sqlite3_free(file);
  }
  auto* temp = static_cast<sqlite3_file*>(sqlite3_malloc(v->szOsFile));
  ASSERT_EQ(SQLITE_OK, v->xOpen(v, nullptr, temp, SQLITE_OPEN_READWRITE |
      SQLITE_OPEN_CREATE | SQLITE_OPEN_DELETEONCLOSE | SQLITE_OPEN_TEMP_DB, nullptr));
  ASSERT_EQ(SQLITE_OK, temp->pMethods->xWrite(temp, kMarker.data(), kMarker.size(), 0));
  ASSERT_EQ(SQLITE_OK, temp->pMethods->xClose(temp));
  sqlite3_free(temp);
  std::vector<RecordDirectoryEntry> entries;
  ASSERT_EQ(FileStatus::kOk, files_->List(cap_, kPurpose, "", &entries));
  ASSERT_EQ(kinds.size(), entries.size());
  NoPlaintext();
}

TEST_F(RecordSqliteVfsTest, TamperingSurfacesIntegrityAndCannotRaze) {
  sqlite3* db = nullptr;
  ASSERT_EQ(SQLITE_OK, OpenRaw("tampered.sqlite", &db));
  Exec(db, "CREATE TABLE t(value TEXT); INSERT INTO t VALUES('SQLITE_ENCRYPTED_MARKER_19dbcf710239')");
  ASSERT_EQ(SQLITE_OK, sqlite3_close(db));
  NoPlaintext();
  const auto directory = path_.AppendASCII(ObjectName(record_.directory));
  base::FileEnumerator scan(directory, false, base::FileEnumerator::FILES);
  size_t tampered = 0;
  for (auto path = scan.Next(); !path.empty(); path = scan.Next()) {
    auto bytes = base::ReadFileToBytes(path);
    ASSERT_TRUE(bytes && !bytes->empty());
    base::span(*bytes)[0] ^= 1;
    ASSERT_TRUE(base::WriteFile(path, *bytes));
    ++tampered;
  }
  ASSERT_GT(tampered, 0u);
  db = nullptr;
  const int result = OpenRaw("tampered.sqlite", &db);
  EXPECT_EQ(SQLITE_IOERR, result & 0xff);
  EXPECT_EQ(FileStatus::kIntegrity, vfs_->last_status());
  EXPECT_FALSE(files_->unlocked());
  EXPECT_EQ(FileStatus::kIntegrity, files_->Remove(cap_, kPurpose, "tampered.sqlite"));
  if (db) sqlite3_close(db);
}

TEST_F(RecordSqliteVfsTest, LockRevokesOpenFilesAndWrongPurposeIsRefused) {
  std::unique_ptr<RecordSqliteVfs> wrong;
  EXPECT_EQ(FileStatus::kDenied, RecordSqliteVfs::Create(files_, cap_, StorePurpose::kCookies, &wrong));
  sqlite3* db = nullptr;
  ASSERT_EQ(SQLITE_OK, OpenRaw("revoked.sqlite", &db));
  Exec(db, "CREATE TABLE t(value TEXT)");
  sqlite3_file* file = nullptr;
  ASSERT_EQ(SQLITE_OK, sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file));
  ASSERT_TRUE(file);
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  std::array<char, 16> bytes;
  bytes.fill('X');
  EXPECT_EQ(SQLITE_IOERR_ACCESS, file->pMethods->xRead(file, bytes.data(), bytes.size(), 0));
  for (char byte : bytes) EXPECT_EQ(0, byte);
  EXPECT_NE(SQLITE_OK, file->pMethods->xWrite(file, kMarker.data(), kMarker.size(), 0));
  sqlite3_close(db);
}

TEST_F(RecordSqliteVfsTest, CommittedEncryptedWalRecoversAfterActualProcessExit) {
  vfs_.reset();
  ASSERT_EQ(FileStatus::kOk, files_->Lock());
  files_.reset();
  auto command = base::GetMultiProcessTestChildBaseCommandLine();
  command.AppendSwitchPath("record-profile", path_);
  auto child = base::SpawnMultiProcessTestChild("AlloySqliteWalCrashChild", command, {});
  ASSERT_TRUE(child.IsValid());
  int exit = -1;
  const bool stopped = base::WaitForMultiprocessTestChildExit(child, base::Seconds(30), &exit);
  if (!stopped) base::TerminateMultiProcessTestChild(child, 99, true);
  ASSERT_TRUE(stopped);
  ASSERT_EQ(0, exit);
  ASSERT_EQ(FileStatus::kOk, RecordFileSystem::Open(path_, TestKey(), &files_));
  Resolve();
  RecordFileInfo wal;
  ASSERT_EQ(FileStatus::kOk, files_->Stat(cap_, kPurpose, "crash.sqlite-wal", &wal));
  EXPECT_GT(wal.length, 32u);
  ASSERT_EQ(FileStatus::kOk, RecordSqliteVfs::Create(files_, cap_, kPurpose, &vfs_));
  sqlite3* db = nullptr;
  ASSERT_EQ(SQLITE_OK, OpenRaw("crash.sqlite", &db));
  CheckRow(db);
  ASSERT_EQ(SQLITE_OK, sqlite3_close(db));
  NoPlaintext();
}

}  // namespace
}  // namespace alloy::site_records
