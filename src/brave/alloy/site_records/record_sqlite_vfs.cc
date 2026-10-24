// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_sqlite_vfs.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/no_destructor.h"
#include "sql/initialization.h"
#include "third_party/boringssl/src/include/openssl/crypto.h"
#include "third_party/boringssl/src/include/openssl/rand.h"

namespace alloy::site_records {
namespace {

static_assert(static_cast<int>(RecordFileLock::kNone) == SQLITE_LOCK_NONE);
static_assert(static_cast<int>(RecordFileLock::kShared) == SQLITE_LOCK_SHARED);
static_assert(static_cast<int>(RecordFileLock::kReserved) == SQLITE_LOCK_RESERVED);
static_assert(static_cast<int>(RecordFileLock::kPending) == SQLITE_LOCK_PENDING);
static_assert(static_cast<int>(RecordFileLock::kExclusive) == SQLITE_LOCK_EXCLUSIVE);

struct Region {
  explicit Region(size_t size) : words(size / sizeof(uint64_t), 0) {}
  ~Region() { OPENSSL_cleanse(words.data(), words.size() * sizeof(uint64_t)); }
  std::vector<uint64_t> words;
};

struct WalIndex {
  std::mutex mutex;
  std::vector<std::unique_ptr<Region>> regions;
  size_t region_size = 0;
  std::map<const sqlite3_file*, std::array<int, SQLITE_SHM_NLOCK>> locks;
};
struct WalGroup {
  std::mutex mutex;
  std::map<ObjectId, std::weak_ptr<WalIndex>> files;
  std::shared_ptr<WalIndex> For(const ObjectId& id) {
    std::lock_guard lock(mutex);
    for (auto it = files.begin(); it != files.end();) {
      it = it->second.expired() ? files.erase(it) : std::next(it);
    }
    auto& weak = files[id];
    auto index = weak.lock();
    if (!index) weak = index = std::make_shared<WalIndex>();
    return index;
  }
};
struct WalRegistry {
  std::mutex mutex;
  std::map<std::string, std::weak_ptr<WalGroup>> groups;
};

std::shared_ptr<WalGroup> GetWalGroup(const RecordCapability& cap, StorePurpose purpose) {
  static base::NoDestructor<WalRegistry> registry;
  const auto key = cap.profile().ToString() + cap.record().ToString() +
      cap.generation().ToString() + std::to_string(static_cast<int>(purpose));
  std::lock_guard lock(registry->mutex);
  for (auto it = registry->groups.begin(); it != registry->groups.end();) {
    it = it->second.expired() ? registry->groups.erase(it) : std::next(it);
  }
  auto& weak = registry->groups[key];
  auto group = weak.lock();
  if (!group) weak = group = std::make_shared<WalGroup>();
  return group;
}

FileStatus LogicalPath(const char* path, std::string* out) {
  if (!path) return FileStatus::kInvalid;
  std::string_view text(path);
  if (text.starts_with('/')) text.remove_prefix(1);
  if (text.empty() || text.size() > 4095) return FileStatus::kInvalid;
  auto remaining = text;
  while (!remaining.empty()) {
    const auto slash = remaining.find('/');
    const auto part = remaining.substr(0, slash);
    if (part.empty() || part == "." || part == ".." || part.size() > 255 ||
        part.find(':') != std::string_view::npos) return FileStatus::kInvalid;
    if (slash == std::string_view::npos) break;
    remaining.remove_prefix(slash + 1);
    if (remaining.empty()) return FileStatus::kInvalid;
  }
  out->assign(text);
  return FileStatus::kOk;
}

int Error(FileStatus status, int io_error = SQLITE_IOERR) {
  switch (status) {
    case FileStatus::kOk: return SQLITE_OK;
    case FileStatus::kBusy: return SQLITE_BUSY;
    case FileStatus::kIntegrity:
    case FileStatus::kCommitUncertain: return SQLITE_IOERR_DATA;
    case FileStatus::kDenied: return SQLITE_IOERR_ACCESS;
    default: return io_error;
  }
}
}  // namespace

struct RecordSqliteVfs::Impl {
  struct File {
    Impl* owner = nullptr;
    std::shared_ptr<RecordFile> data;
    std::string path;
    std::shared_ptr<WalIndex> wal;
    bool readonly = false;
    bool remove_on_close = false;
    int persist_wal = 0;
  };
  struct Bridge {
    sqlite3_file base{};
    File* file = nullptr;
  };
  static_assert(offsetof(Bridge, base) == 0);
  std::shared_ptr<RecordFileSystem> files;
  RecordCapability cap;
  StorePurpose purpose;
  std::shared_ptr<WalGroup> group;
  std::string name;
  sqlite3_vfs vfs{};
  std::atomic<size_t> open_files{0};
  std::atomic<FileStatus> last{FileStatus::kOk};

  static Impl& Self(sqlite3_vfs* v) { return *static_cast<Impl*>(v->pAppData); }
  static File& As(sqlite3_file* f) { return *reinterpret_cast<Bridge*>(f)->file; }
  int Result(FileStatus status, int error = SQLITE_IOERR) {
    if (status == FileStatus::kOk) return SQLITE_OK;
    auto prior = last.load();
    while (prior != FileStatus::kIntegrity && prior != FileStatus::kCommitUncertain &&
           !last.compare_exchange_weak(prior, status)) {}
    const auto effective = last.load();
    return Error(effective == FileStatus::kIntegrity || effective == FileStatus::kCommitUncertain
                     ? effective : status, error);
  }
  bool Active(bool write = false) const {
    return cap.Allows(purpose, write ? StorageAccess::kWriteRecordFiles
                                     : StorageAccess::kReadRecordFiles);
  }

  static int Close(sqlite3_file* f) {
    auto& file = As(f);
    ShmUnmap(f, 0);
    const auto status = file.data->Close();
    auto removed = FileStatus::kOk;
    if (file.remove_on_close) {
      removed = file.owner->files->Remove(file.owner->cap, file.owner->purpose, file.path);
      if (removed == FileStatus::kNotFound) removed = FileStatus::kOk;
    }
    auto* owner = file.owner;
    delete &file;
    reinterpret_cast<Bridge*>(f)->file = nullptr;
    f->pMethods = nullptr;
    --owner->open_files;
    return owner->Result(status != FileStatus::kOk ? status : removed, SQLITE_IOERR_CLOSE);
  }
  static int Read(sqlite3_file* f, void* out, int amount, sqlite3_int64 offset) {
    auto& file = As(f);
    if (amount < 0 || offset < 0) return SQLITE_IOERR_READ;
    // SQLite supplies exactly amount writable bytes for the xRead protocol.
    auto bytes = UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(out), static_cast<size_t>(amount)));
    size_t read = 0;
    const auto status = file.data->Read(offset, bytes, &read);
    if (status != FileStatus::kOk) return file.owner->Result(status, SQLITE_IOERR_READ);
    return read == bytes.size() ? SQLITE_OK : SQLITE_IOERR_SHORT_READ;
  }
  static int Write(sqlite3_file* f, const void* in, int amount, sqlite3_int64 offset) {
    auto& file = As(f);
    if (file.readonly) return SQLITE_READONLY;
    if (amount < 0 || offset < 0) return SQLITE_IOERR_WRITE;
    // SQLite supplies exactly amount readable bytes for the xWrite protocol.
    auto bytes = UNSAFE_BUFFERS(base::span(static_cast<const uint8_t*>(in), static_cast<size_t>(amount)));
    return file.owner->Result(file.data->Write(offset, bytes), SQLITE_IOERR_WRITE);
  }
  static int Truncate(sqlite3_file* f, sqlite3_int64 size) {
    auto& file = As(f);
    if (file.readonly) return SQLITE_READONLY;
    return size < 0 ? SQLITE_IOERR_TRUNCATE
        : file.owner->Result(file.data->SetLength(size), SQLITE_IOERR_TRUNCATE);
  }
  static int Sync(sqlite3_file* f, int) {
    auto& file = As(f);
    return file.owner->Result(file.data->Sync(), SQLITE_IOERR_FSYNC);
  }
  static int Size(sqlite3_file* f, sqlite3_int64* size) {
    auto& file = As(f);
    RecordFileInfo info;
    const auto status = file.data->GetInfo(&info);
    if (status == FileStatus::kOk) *size = info.length;
    return file.owner->Result(status, SQLITE_IOERR_FSTAT);
  }
  static int Lock(sqlite3_file* f, int requested) {
    auto& file = As(f);
    if (requested < SQLITE_LOCK_SHARED || requested > SQLITE_LOCK_EXCLUSIVE) return SQLITE_MISUSE;
    RecordFileLock current;
    auto status = file.data->GetLock(&current);
    if (status == FileStatus::kOk && requested > static_cast<int>(current)) {
      status = file.data->Lock(static_cast<RecordFileLock>(requested));
    }
    return file.owner->Result(status, SQLITE_IOERR_LOCK);
  }
  static int Unlock(sqlite3_file* f, int requested) {
    auto& file = As(f);
    if (requested != SQLITE_LOCK_NONE && requested != SQLITE_LOCK_SHARED) return SQLITE_MISUSE;
    RecordFileLock current;
    auto status = file.data->GetLock(&current);
    if (status == FileStatus::kOk && requested < static_cast<int>(current)) {
      status = file.data->Unlock(static_cast<RecordFileLock>(requested));
    }
    return file.owner->Result(status, SQLITE_IOERR_UNLOCK);
  }
  static int Reserved(sqlite3_file* f, int* out) {
    auto& file = As(f);
    bool reserved = false;
    const auto status = file.data->HasReservedLock(&reserved);
    *out = reserved;
    return file.owner->Result(status, SQLITE_IOERR_CHECKRESERVEDLOCK);
  }
  static int Control(sqlite3_file* f, int op, void* arg) {
    auto& file = As(f);
    if (!file.owner->Active()) return SQLITE_IOERR_ACCESS;
    switch (op) {
      case SQLITE_FCNTL_LOCKSTATE: {
        RecordFileLock lock;
        const auto status = file.data->GetLock(&lock);
        if (status == FileStatus::kOk) *static_cast<int*>(arg) = static_cast<int>(lock);
        return file.owner->Result(status);
      }
      case SQLITE_FCNTL_MMAP_SIZE: *static_cast<sqlite3_int64*>(arg) = 0; return SQLITE_OK;
      case SQLITE_FCNTL_SIZE_HINT:
      case SQLITE_FCNTL_CHUNK_SIZE: return SQLITE_OK;  // Hints never change logical EOF.
      case SQLITE_FCNTL_LAST_ERRNO: *static_cast<int*>(arg) = 0; return SQLITE_OK;
      case SQLITE_FCNTL_POWERSAFE_OVERWRITE: *static_cast<int*>(arg) = 0; return SQLITE_OK;
      case SQLITE_FCNTL_PERSIST_WAL: {
        auto* value = static_cast<int*>(arg);
        if (*value >= 0) file.persist_wal = *value != 0;
        *value = file.persist_wal;
        return SQLITE_OK;
      }
      case SQLITE_FCNTL_HAS_MOVED: {
        RecordFileInfo named, opened;
        auto status = file.owner->files->Stat(file.owner->cap, file.owner->purpose, file.path, &named);
        if (status == FileStatus::kNotFound) { *static_cast<int*>(arg) = 1; return SQLITE_OK; }
        if (status == FileStatus::kOk) status = file.data->GetInfo(&opened);
        if (status == FileStatus::kOk) *static_cast<int*>(arg) = named.identity != opened.identity;
        return file.owner->Result(status);
      }
      case SQLITE_FCNTL_VFSNAME:
        *static_cast<char**>(arg) = sqlite3_mprintf("%s", file.owner->name.c_str());
        return *static_cast<char**>(arg) ? SQLITE_OK : SQLITE_NOMEM;
      default: return SQLITE_NOTFOUND;
    }
  }
  static int Sector(sqlite3_file*) { return kRecordChunkSize; }
  static int Characteristics(sqlite3_file*) { return 0; }
  static int ShmMap(sqlite3_file* f, int region, int size, int extend, void volatile** out) {
    auto& file = As(f);
    *out = nullptr;
    if (!file.owner->Active()) return SQLITE_IOERR_ACCESS;
    if (region < 0 || size <= 0 || size % sizeof(uint64_t)) return SQLITE_IOERR_SHMMAP;
    if (!file.wal) {
      RecordFileInfo info;
      auto status = file.data->GetInfo(&info);
      if (status != FileStatus::kOk) return file.owner->Result(status, SQLITE_IOERR_SHMMAP);
      file.wal = file.owner->group->For(info.identity);
    }
    std::lock_guard lock(file.wal->mutex);
    if (file.wal->region_size && file.wal->region_size != static_cast<size_t>(size)) return SQLITE_IOERR_SHMSIZE;
    const auto index = static_cast<size_t>(region);
    if (index >= file.wal->regions.size()) {
      if (!extend) return SQLITE_OK;
      file.wal->regions.resize(index + 1);
    }
    auto& page = file.wal->regions[index];
    if (!page && extend) page = std::make_unique<Region>(size);
    if (page) {
      file.wal->region_size = size;
      *out = page->words.data();
    }
    return SQLITE_OK;
  }
  static int ShmLock(sqlite3_file* f, int offset, int count, int flags) {
    auto& file = As(f);
    if (!file.owner->Active()) return SQLITE_IOERR_ACCESS;
    if (!file.wal || offset < 0 || count <= 0 || offset > SQLITE_SHM_NLOCK - count) return SQLITE_IOERR_SHMLOCK;
    const bool unlock = (flags & SQLITE_SHM_UNLOCK) != 0;
    const bool exclusive = (flags & SQLITE_SHM_EXCLUSIVE) != 0;
    if (flags != ((unlock ? SQLITE_SHM_UNLOCK : SQLITE_SHM_LOCK) |
                  (exclusive ? SQLITE_SHM_EXCLUSIVE : SQLITE_SHM_SHARED))) return SQLITE_MISUSE;
    std::lock_guard lock(file.wal->mutex);
    auto& own = file.wal->locks[f];
    if (!unlock) {
      for (const auto& [other, held] : file.wal->locks) {
        if (other == f) continue;
        for (int i = offset; i < offset + count; ++i) {
          const auto value = base::span(held)[static_cast<size_t>(i)];
          if (value && (exclusive || value == -1)) return SQLITE_BUSY;
        }
      }
    }
    auto own_slots = base::span(own);
    for (int i = offset; i < offset + count; ++i) {
      own_slots[static_cast<size_t>(i)] = unlock ? 0 : exclusive ? -1 : 1;
    }
    return SQLITE_OK;
  }
  static void ShmBarrier(sqlite3_file*) { std::atomic_thread_fence(std::memory_order_seq_cst); }
  static int ShmUnmap(sqlite3_file* f, int) {
    auto& file = As(f);
    if (file.wal) {
      {
        std::lock_guard lock(file.wal->mutex);
        file.wal->locks.erase(f);
      }
      file.wal.reset();
    }
    return SQLITE_OK;
  }
  static int Fetch(sqlite3_file* f, sqlite3_int64, int, void** out) {
    *out = nullptr;
    return As(f).owner->Active() ? SQLITE_OK : SQLITE_IOERR_ACCESS;
  }
  static int Unfetch(sqlite3_file*, sqlite3_int64, void*) { return SQLITE_OK; }

  static int Open(sqlite3_vfs* v, const char* path, sqlite3_file* buffer, int flags, int* granted) {
    auto& self = Self(v);
    buffer->pMethods = nullptr;
    if (!self.Active((flags & SQLITE_OPEN_READONLY) == 0)) return self.Result(FileStatus::kDenied);
    std::string logical;
    if (!path) {
      if (!(flags & SQLITE_OPEN_DELETEONCLOSE)) return SQLITE_CANTOPEN;
      ObjectId id;
      auto status = RandomId(&id);
      if (status != FileStatus::kOk) return self.Result(status);
      logical = "sqlite-temp-" + ObjectName(id);
    } else if (LogicalPath(path, &logical) != FileStatus::kOk) {
      return SQLITE_CANTOPEN;
    }
    RecordOpen mode = RecordOpen::kOpen;
    if (flags & SQLITE_OPEN_CREATE) mode = flags & SQLITE_OPEN_EXCLUSIVE ? RecordOpen::kCreate : RecordOpen::kOpenOrCreate;
    std::shared_ptr<RecordFile> data;
    auto status = self.files->OpenFile(self.cap, self.purpose, logical, mode, &data);
    if (status != FileStatus::kOk) return self.Result(status, SQLITE_CANTOPEN);
    auto* file = new File();
    file->owner = &self;
    file->data = std::move(data);
    file->path = std::move(logical);
    file->readonly = (flags & SQLITE_OPEN_READONLY) != 0;
    file->remove_on_close = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
    static const sqlite3_io_methods methods = {
      3, Close, Read, Write, Truncate, Sync, Size, Lock, Unlock, Reserved,
      Control, Sector, Characteristics, ShmMap, ShmLock, ShmBarrier, ShmUnmap, Fetch, Unfetch};
    auto* bridge = new (buffer) Bridge();
    bridge->file = file;
    bridge->base.pMethods = &methods;
    ++self.open_files;
    if (granted) *granted = flags;
    return SQLITE_OK;
  }
  static int Delete(sqlite3_vfs* v, const char* path, int) {
    auto& self = Self(v);
    std::string logical;
    if (LogicalPath(path, &logical) != FileStatus::kOk) return SQLITE_IOERR_DELETE;
    const auto status = self.files->Remove(self.cap, self.purpose, logical);
    return self.Result(status, status == FileStatus::kNotFound ? SQLITE_IOERR_DELETE_NOENT : SQLITE_IOERR_DELETE);
  }
  static int Access(sqlite3_vfs* v, const char* path, int flags, int* out) {
    auto& self = Self(v);
    *out = 0;
    if (!self.Active(flags == SQLITE_ACCESS_READWRITE)) return self.Result(FileStatus::kDenied);
    std::string logical;
    if (LogicalPath(path, &logical) != FileStatus::kOk) return SQLITE_IOERR_ACCESS;
    RecordFileInfo info;
    const auto status = self.files->Stat(self.cap, self.purpose, logical, &info);
    if (status == FileStatus::kNotFound) return SQLITE_OK;
    if (status == FileStatus::kOk) *out = 1;
    return self.Result(status, SQLITE_IOERR_ACCESS);
  }
  static int FullPath(sqlite3_vfs*, const char* path, int size, char* out) {
    std::string logical;
    if (LogicalPath(path, &logical) != FileStatus::kOk || size <= 0 ||
        static_cast<size_t>(size) < logical.size() + 2) return SQLITE_CANTOPEN;
    const std::string full = "/" + logical;
    std::memcpy(out, full.c_str(), full.size() + 1);
    return SQLITE_OK;
  }
  static int Random(sqlite3_vfs*, int size, char* out) {
    return size > 0 && RAND_bytes(reinterpret_cast<uint8_t*>(out), size) == 1 ? size : 0;
  }
  static int Sleep(sqlite3_vfs*, int micros) {
    if (micros > 0) std::this_thread::sleep_for(std::chrono::microseconds(micros));
    return micros > 0 ? micros : 0;
  }
  static int Time(sqlite3_vfs*, sqlite3_int64* out) {
    *out = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 210866760000000LL;
    return SQLITE_OK;
  }
  static int LastError(sqlite3_vfs* v, int size, char* out) {
    if (size > 0) std::memset(out, 0, size);
    return static_cast<int>(Self(v).last.load());
  }
};

RecordSqliteVfs::RecordSqliteVfs(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FileStatus RecordSqliteVfs::Create(std::shared_ptr<RecordFileSystem> files,
                                    RecordCapability cap, StorePurpose purpose,
                                    std::unique_ptr<RecordSqliteVfs>* out) {
  if (!files || !cap.Allows(purpose, StorageAccess::kWriteRecordFiles)) return FileStatus::kDenied;
  RecordFileInfo admitted_root;
  const auto admission = files->Stat(cap, purpose, "", &admitted_root);
  if (admission != FileStatus::kOk) return admission;
  // Initialize Chromium's ordinary default wrapper before registering a named
  // record VFS. Never make this VFS default or feed it to EnsureVfsWrapper().
  sql::EnsureSqliteInitialized();
  auto impl = std::make_unique<Impl>();
  impl->files = std::move(files);
  impl->cap = std::move(cap);
  impl->purpose = purpose;
  impl->group = GetWalGroup(impl->cap, purpose);
  impl->name = "alloy-record-" + base::UnguessableToken::Create().ToString();
  auto& v = impl->vfs;
  v.iVersion = 3;
  v.szOsFile = sizeof(Impl::Bridge);
  v.mxPathname = 4096;
  v.zName = impl->name.c_str();
  v.pAppData = impl.get();
  v.xOpen = Impl::Open; v.xDelete = Impl::Delete; v.xAccess = Impl::Access;
  v.xFullPathname = Impl::FullPath; v.xRandomness = Impl::Random;
  v.xSleep = Impl::Sleep; v.xGetLastError = Impl::LastError;
  v.xCurrentTimeInt64 = Impl::Time;
  if (sqlite3_vfs_register(&v, 0) != SQLITE_OK) return FileStatus::kIo;
  out->reset(new RecordSqliteVfs(std::move(impl)));
  return FileStatus::kOk;
}
RecordSqliteVfs::~RecordSqliteVfs() {
  CHECK_EQ(impl_->open_files.load(), 0u) << "Close SQLite connections before their VFS owner";
  sqlite3_vfs_unregister(&impl_->vfs);
}
const char* RecordSqliteVfs::name() const { return impl_->name.c_str(); }
sql::DatabaseOptions RecordSqliteVfs::Options(sql::DatabaseOptions options) const {
  return std::move(options.set_vfs_name_discouraged(name()).set_mmap_enabled(false));
}
FileStatus RecordSqliteVfs::last_status() const { return impl_->last.load(); }
sqlite3_vfs* RecordSqliteVfs::vfs_for_testing() { return &impl_->vfs; }

}  // namespace alloy::site_records
