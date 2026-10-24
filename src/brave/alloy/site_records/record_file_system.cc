// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/record_file_system.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "brave/alloy/site_records/encrypted_tree.h"
#include "url/gurl.h"

namespace alloy::site_records {
namespace {

constexpr uint64_t kMaxPublications = (uint64_t{1} << 32) - 1;
constexpr size_t kMaxRecordMetadata = 2048;

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
}

void WipeString(std::string* text) {
  Cleanse(base::as_writable_byte_span(*text));
  text->clear();
}

struct Encoder {
  std::vector<uint8_t> bytes;
  ~Encoder() { Cleanse(bytes); }
  void U32(uint32_t value) {
    for (int shift : {24, 16, 8, 0}) {
      bytes.push_back(value >> shift);
    }
  }
  void U64(uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      bytes.push_back(value >> shift);
    }
  }
  void Raw(base::span<const uint8_t> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }
  void Blob(base::span<const uint8_t> value) {
    U32(value.size());
    Raw(value);
  }
  void Text(std::string_view text) { Blob(base::as_byte_span(text)); }
  void Token(base::UnguessableToken token) {
    U64(token.GetHighForSerialization());
    U64(token.GetLowForSerialization());
  }
  void Ref(const ObjectRef& ref) {
    Raw(ref.id);
    Raw(ref.digest);
  }
  void FileRoot(const FileContentsRoot& root) {
    Raw(root.file_id);
    U64(root.length);
    U64(root.revision);
    Ref(root.chunks);
  }
};

struct Decoder {
  explicit Decoder(base::span<const uint8_t> input) : remaining(input) {}
  base::span<const uint8_t> remaining;
  bool valid = true;
  base::span<const uint8_t> Take(size_t size) {
    if (remaining.size() < size) {
      valid = false;
      return {};
    }
    const auto result = remaining.first(size);
    remaining = remaining.subspan(size);
    return result;
  }
  uint32_t U32() {
    uint32_t result = 0;
    for (uint8_t b : Take(4)) {
      result = (result << 8) | b;
    }
    return result;
  }
  uint64_t U64() {
    uint64_t result = 0;
    for (uint8_t b : Take(8)) {
      result = (result << 8) | b;
    }
    return result;
  }
  ObjectId Id() {
    ObjectId id{};
    const auto bytes = Take(32);
    if (valid) {
      base::span(id).copy_from(bytes);
    }
    return id;
  }
  base::span<const uint8_t> Blob(size_t maximum) {
    const auto length = U32();
    if (length > maximum) {
      valid = false;
      return {};
    }
    return Take(length);
  }
  std::string Text(size_t maximum) {
    const auto bytes = Blob(maximum);
    return std::string(bytes.begin(), bytes.end());
  }
  base::UnguessableToken Token() {
    const auto high = U64();
    const auto low = U64();
    auto token = base::UnguessableToken::Deserialize(high, low);
    valid &= token.has_value();
    return token.value_or(base::UnguessableToken());
  }
  ObjectRef Ref() {
    ObjectRef ref;
    ref.id = Id();
    ref.digest = Id();
    valid &= (ref.id == ObjectId{}) == (ref.digest == ObjectId{});
    return ref;
  }
  FileContentsRoot FileRoot() {
    FileContentsRoot root;
    root.file_id = Id();
    root.length = U64();
    root.revision = U64();
    root.chunks = Ref();
    valid &= root.file_id != ObjectId{} &&
             root.length <= EncryptedFileContents::kMaxLength &&
             root.revision <= EncryptedFileContents::kMaxRevision;
    return root;
  }
  bool Done() const { return valid && remaining.empty(); }
};

struct WipedBytes {
  std::vector<uint8_t> bytes;
  ~WipedBytes() { Cleanse(bytes); }
};

struct WipedPage {
  ObjectPage page{};
  ~WipedPage() { Cleanse(page); }
};

struct NamespaceEntry {
  uint32_t purpose = 0;
  ObjectId parent{};
  std::string name;
  bool directory = false;
  int64_t modified_us = 0;
  FileContentsRoot root;
  ~NamespaceEntry() { WipeString(&name); }
};

bool ValidComponent(std::string_view name) {
  return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

FileStatus Components(std::string_view name, std::vector<std::string>* out) {
  if (name.empty() || name == ".") {
    return FileStatus::kOk;
  }
  if (name.size() > 4096) {
    return FileStatus::kInvalid;
  }
  while (!name.empty()) {
    const auto slash = name.find('/');
    const auto part = name.substr(0, slash);
    if (!ValidComponent(part)) {
      return FileStatus::kInvalid;
    }
    out->emplace_back(part);
    if (slash == std::string_view::npos) {
      return FileStatus::kOk;
    }
    name.remove_prefix(slash + 1);
    if (name.empty()) {
      return FileStatus::kInvalid;
    }
  }
  return FileStatus::kOk;
}

RecordFileInfo EntryInfo(const NamespaceEntry& entry) {
  return {entry.root.file_id, entry.root.length, entry.modified_us,
          entry.directory};
}

StorePurpose PurposeForTag(uint32_t tag) {
  constexpr auto purposes = std::to_array<StorePurpose>({
      StorePurpose::kCookies, StorePurpose::kDomStorage,
      StorePurpose::kIndexedDb, StorePurpose::kCacheStorage,
      StorePurpose::kFileSystem, StorePurpose::kServiceWorkers,
      StorePurpose::kBackgroundJobs, StorePurpose::kHttpCache,
      StorePurpose::kCodeCache, StorePurpose::kHistory,
      StorePurpose::kVisitedLinks, StorePurpose::kFavicons,
      StorePurpose::kSiteSettings, StorePurpose::kQuota,
      StorePurpose::kTransportSecurity, StorePurpose::kServerProperties,
      StorePurpose::kReportingConfiguration, StorePurpose::kSharedDictionaries,
      StorePurpose::kMediaDeviceSalts, StorePurpose::kOriginTrials,
      StorePurpose::kNotifications, StorePurpose::kBlobSpill});
  // DecodeEntry rejects tags outside 1..22 before this internal call.
  return base::span(purposes)[tag - 1];
}

FileStatus DecodeEntry(base::span<const uint8_t> bytes, NamespaceEntry* entry) {
  Decoder in(bytes);
  if (in.U32() != 1) {
    return FileStatus::kIntegrity;
  }
  entry->purpose = in.U32();
  entry->parent = in.Id();
  entry->name = in.Text(255);
  const auto type = in.U32();
  entry->directory = type == 1;
  entry->modified_us = static_cast<int64_t>(in.U64());
  entry->root = in.FileRoot();
  if (!in.Done() || type > 1 || entry->purpose < 1 || entry->purpose > 22 ||
      !ValidComponent(entry->name) ||
      (entry->directory && (entry->root.length != 0 ||
                            entry->root.revision != 0 ||
                            !entry->root.chunks.empty()))) {
    return FileStatus::kIntegrity;
  }
  return FileStatus::kOk;
}

std::vector<uint8_t> RecordBinding(base::UnguessableToken profile,
                                   const RecordInfo& record) {
  Encoder out;
  out.Token(profile);
  out.Token(record.id);
  out.Token(record.generation);
  out.Raw(record.directory);
  return std::move(out.bytes);
}

}  // namespace

struct RecordFileState {
  NamespaceEntry entry;
  std::unique_ptr<EncryptedFileContents> contents;
  bool linked = true;
  bool dirty = false;
  size_t handles = 0;
  std::map<const RecordFile*, RecordFileLock> locks;
};

struct RecordFileSystem::Impl {
  struct Record {
    RecordInfo info;
    SecretKey key;
    SecretKey namespace_key;
    SecretKey naming_key;
    std::vector<uint8_t> binding;
    std::vector<uint8_t> wrapped_key;
    std::unique_ptr<RecordCapabilityIssuer> issuer;
    std::unique_ptr<ObjectDirectory> objects;
    ObjectRef names;
    bool dirty = false;
    bool gc_needed = false;
    std::map<ObjectId, std::shared_ptr<RecordFileState>> files;

    ~Record() { Wipe(); }
    void Wipe() {
      if (issuer) {
        issuer->Revoke();
      }
      key.Clear();
      namespace_key.Clear();
      naming_key.Clear();
      WipeString(&info.domain);
      Cleanse(info.metadata);
      info.metadata.clear();
      for (auto& [id, file] : files) {
        file->contents->Revoke();
        WipeString(&file->entry.name);
        file->locks.clear();
      }
    }
  };

  explicit Impl(SecretKey key) : profile_key(std::move(key)) {}
  mutable std::mutex mutex;
  bool open = true;
  FileStatus failure = FileStatus::kOk;
  SecretKey profile_key;
  SecretKey lookup_key;
  SecretKey catalogue_key;
  SecretKey wrapping_key;
  base::UnguessableToken profile;
  ObjectId catalogue_directory{};
  uint64_t revision = 0;
  ObjectRef catalogue;
  std::vector<uint8_t> public_bootstrap;
  std::unique_ptr<ObjectDirectory> root;
  std::unique_ptr<ObjectDirectory> catalogue_objects;
  std::map<base::UnguessableToken, std::unique_ptr<Record>> records;
  std::set<ObjectId> deleting;
  FileFault fault;

  FileStatus Admission() const {
    return !open ? FileStatus::kDenied : failure;
  }

  void WipeKeys() {
    for (auto& [id, record] : records) {
      record->Wipe();
    }
    profile_key.Clear();
    lookup_key.Clear();
    catalogue_key.Clear();
    wrapping_key.Clear();
  }

  FileStatus Observe(FileStatus status) {
    if (status == FileStatus::kIntegrity ||
        status == FileStatus::kCommitUncertain) {
      failure = status;
      WipeKeys();
    }
    return status;
  }

  void InitializeKeys() {
    Encoder salt;
    salt.Token(profile);
    lookup_key = DeriveKey(profile_key, salt.bytes,
        base::as_byte_span(std::string_view("alloy-directory-hmac-v1")));
    catalogue_key = DeriveKey(profile_key, salt.bytes,
        base::as_byte_span(std::string_view("alloy-catalogue-v1")));
    wrapping_key = DeriveKey(profile_key, salt.bytes,
        base::as_byte_span(std::string_view("alloy-record-wrapping-v1")));
    catalogue_directory = KeyedName(lookup_key,
        base::as_byte_span(std::string_view("\0alloy-profile-private-v1", 25)));
  }

  std::vector<uint8_t> CatalogueBinding() const {
    Encoder out;
    out.Token(profile);
    return std::move(out.bytes);
  }

  EncryptedTree CatalogueTree() {
    return EncryptedTree(*catalogue_objects, ObjectType::kCatalogue,
                           catalogue_key, CatalogueBinding());
  }

  EncryptedTree Names(Record& record) {
    return EncryptedTree(*record.objects, ObjectType::kNamespace,
                           record.namespace_key, record.binding);
  }

  ObjectId NameKey(Record& record, uint32_t purpose, const ObjectId& parent,
                   std::string_view name) const {
    Encoder out;
    out.U32(purpose);
    out.Raw(parent);
    out.Text(name);
    return KeyedName(record.naming_key, out.bytes);
  }

  void InitializeRecordKeys(Record& record) {
    record.binding = RecordBinding(profile, record.info);
    record.namespace_key = DeriveKey(record.key, record.binding,
        base::as_byte_span(std::string_view("alloy-namespace-v1")));
    record.naming_key = DeriveKey(record.key, record.binding,
        base::as_byte_span(std::string_view("alloy-native-naming-v1")));
    record.issuer = std::make_unique<RecordCapabilityIssuer>(
        profile, record.info.id, record.info.generation, StorageMode::kEncrypted);
  }

  std::set<ObjectId> LiveDirectories() const {
    std::set<ObjectId> live{catalogue_directory};
    live.insert(deleting.begin(), deleting.end());
    for (const auto& [id, record] : records) {
      live.insert(record->info.directory);
    }
    return live;
  }

  FileStatus Check(const RecordCapability& cap, StorePurpose purpose,
                    StorageAccess access, Record** out) {
    auto status = Admission();
    if (status != FileStatus::kOk) {
      return status;
    }
    const auto found = records.find(cap.record());
    if (!cap.Allows(purpose, access) || cap.profile() != profile ||
        found == records.end() ||
        cap.generation() != found->second->info.generation) {
      return FileStatus::kDenied;
    }
    *out = found->second.get();
    return FileStatus::kOk;
  }

  FileStatus CheckHandle(RecordFile& handle, StorageAccess access,
                          Record** record) {
    if (handle.closed_) {
      return FileStatus::kDenied;
    }
    return Check(handle.capability_, handle.purpose_, access, record);
  }

  FileStatus GetEntry(Record& record, uint32_t purpose, const ObjectId& parent,
                       std::string_view name, NamespaceEntry* entry) {
    WipedBytes value;
    const auto key = NameKey(record, purpose, parent, name);
    auto status = Names(record).Get(record.names, key, &value.bytes);
    if (status != FileStatus::kOk) {
      return status;
    }
    status = DecodeEntry(value.bytes, entry);
    if (status != FileStatus::kOk || entry->purpose != purpose ||
        entry->parent != parent || entry->name != name) {
      return FileStatus::kIntegrity;
    }
    return FileStatus::kOk;
  }

  FileStatus PutEntry(Record& record, const NamespaceEntry& entry,
                       ObjectRef* names) {
    Encoder value;
    value.U32(1);
    value.U32(entry.purpose);
    value.Raw(entry.parent);
    value.Text(entry.name);
    value.U32(entry.directory ? 1 : 0);
    value.U64(entry.modified_us);
    value.FileRoot(entry.root);
    ObjectRef updated;
    const auto status = Names(record).Put(*names,
        NameKey(record, entry.purpose, entry.parent, entry.name), value.bytes,
        &updated);
    if (status == FileStatus::kOk) {
      *names = updated;
    }
    return status;
  }

  FileStatus Parent(Record& record, uint32_t purpose, std::string_view path,
                     ObjectId* parent, std::string* name,
                     std::vector<ObjectId>* ancestors = nullptr) {
    std::vector<std::string> parts;
    auto status = Components(path, &parts);
    if (status != FileStatus::kOk || parts.empty()) {
      return FileStatus::kInvalid;
    }
    *parent = ObjectId{};
    if (ancestors) {
      ancestors->push_back(*parent);
    }
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      NamespaceEntry entry;
      status = GetEntry(record, purpose, *parent, base::span(parts)[i], &entry);
      if (status != FileStatus::kOk) {
        return status;
      }
      if (!entry.directory) {
        return FileStatus::kNotDirectory;
      }
      *parent = entry.root.file_id;
      if (ancestors) {
        ancestors->push_back(*parent);
      }
    }
    *name = parts.back();
    return FileStatus::kOk;
  }

  using EntryVisitor = std::function<FileStatus(const ObjectId&,
                                                const NamespaceEntry&)>;
  FileStatus VisitEntries(Record& record, const EntryVisitor& visitor) {
    return Names(record).Visit(record.names,
        [&](const ObjectId& key, base::span<const uint8_t> value) {
          NamespaceEntry entry;
          auto status = DecodeEntry(value, &entry);
          if (status != FileStatus::kOk ||
              key != NameKey(record, entry.purpose, entry.parent, entry.name)) {
            return FileStatus::kIntegrity;
          }
          return visitor(key, entry);
        });
  }

  RecordFileInfo Info(Record& record, const NamespaceEntry& entry) {
    auto info = EntryInfo(entry);
    const auto found = record.files.find(entry.root.file_id);
    if (found != record.files.end()) {
      info.length = found->second->contents->root().length;
      info.modified_us = found->second->entry.modified_us;
    }
    return info;
  }

  FileStatus PutRecord(Record& record, ObjectRef* into) {
    Encoder value;
    value.U32(1);
    value.Token(record.info.id);
    value.Token(record.info.generation);
    value.Text(record.info.domain);
    value.U64(record.info.created_us);
    value.Blob(record.info.metadata);
    value.Blob(record.wrapped_key);
    value.Ref(record.names);
    ObjectRef updated;
    const auto status = CatalogueTree().Put(*into, record.info.directory,
                                             value.bytes, &updated);
    if (status == FileStatus::kOk) {
      *into = updated;
    }
    return status;
  }

  FileStatus LoadRecord(const ObjectId& directory,
                         base::span<const uint8_t> value) {
    auto record = std::make_unique<Record>();
    Decoder in(value);
    if (in.U32() != 1) {
      return FileStatus::kIntegrity;
    }
    record->info.directory = directory;
    record->info.id = in.Token();
    record->info.generation = in.Token();
    record->info.domain = in.Text(255);
    record->info.created_us = static_cast<int64_t>(in.U64());
    const auto metadata = in.Blob(kMaxRecordMetadata);
    record->info.metadata.assign(metadata.begin(), metadata.end());
    const auto wrapped = in.Blob(80);
    record->wrapped_key.assign(wrapped.begin(), wrapped.end());
    record->names = in.Ref();
    const auto expected = KeyedName(lookup_key,
        base::as_byte_span(record->info.domain));
    if (!in.Done() || record->info.domain.empty() ||
        record->info.directory != expected || directory == catalogue_directory ||
        records.contains(record->info.id)) {
      return FileStatus::kIntegrity;
    }
    auto status = UnwrapKey(wrapping_key, record->wrapped_key,
                             RecordBinding(profile, record->info), &record->key);
    if (status != FileStatus::kOk) {
      return status;
    }
    InitializeRecordKeys(*record);
    status = root->Child(directory, false, &record->objects);
    if (status != FileStatus::kOk) {
      return status == FileStatus::kNotFound ? FileStatus::kIntegrity : status;
    }
    // Validate the namespace root/tree on unlock. Chunk data is verified on
    // request; missing children are never interpreted as an absent native file.
    status = VisitEntries(*record, [](const ObjectId&, const NamespaceEntry&) {
      return FileStatus::kOk;
    });
    if (status == FileStatus::kOk) {
      records.emplace(record->info.id, std::move(record));
    }
    return status;
  }

  FileStatus PruneRecord(Record& record) {
    std::set<ObjectId> live;
    auto status = Names(record).Reachable(record.names, &live);
    if (status != FileStatus::kOk) {
      return status;
    }
    std::set<ObjectId> files;
    status = VisitEntries(record,
        [&](const ObjectId&, const NamespaceEntry& entry) {
          if (entry.directory) {
            return FileStatus::kOk;
          }
          if (!files.insert(entry.root.file_id).second) {
            return FileStatus::kIntegrity;
          }
          const auto active = record.files.find(entry.root.file_id);
          if (active != record.files.end()) {
            return active->second->contents->Reachable(&live);
          }
          std::unique_ptr<EncryptedFileContents> contents;
          const auto purpose = PurposeForTag(entry.purpose);
          auto result = EncryptedFileContents::Open(*record.objects, record.key,
              record.issuer->Issue(purpose), purpose, entry.root, &contents);
          return result == FileStatus::kOk ? contents->Reachable(&live) : result;
        });
    if (status != FileStatus::kOk) {
      return status;
    }
    // An unlinked/overwritten file may still have native handles. Its objects
    // remain live until those handles close, but it has no namespace entry and
    // can never be reopened or resurrect its detached name.
    for (auto& [id, file] : record.files) {
      if (!files.contains(id)) {
        status = file->contents->Reachable(&live);
        if (status != FileStatus::kOk) {
          return status;
        }
      }
    }
    return record.objects->Prune(live);
  }

  FileStatus Commit() {
    auto status = Admission();
    if (status != FileStatus::kOk) {
      return status;
    }
    if (revision == kMaxPublications) {
      return FileStatus::kInvalid;
    }
    auto next_catalogue = catalogue;
    for (auto& [id, record] : records) {
      auto names = record->names;
      for (auto& [file_id, file] : record->files) {
        if (file->dirty && file->linked) {
          file->entry.root = file->contents->root();
          status = PutEntry(*record, file->entry, &names);
          if (status != FileStatus::kOk) {
            return status;
          }
          record->dirty = true;
        }
      }
      record->names = names;
      if (record->dirty) {
        status = record->objects->Sync();
        if (status != FileStatus::kOk) {
          return status;
        }
        status = PutRecord(*record, &next_catalogue);
        if (status != FileStatus::kOk) {
          return status;
        }
      }
    }
    status = catalogue_objects->Sync();
    if (status != FileStatus::kOk) {
      return status;
    }
    Encoder body;
    body.U32(1);
    body.Token(profile);
    body.U64(revision + 1);
    body.Raw(catalogue_directory);
    body.Ref(next_catalogue);
    WipedPage root_page;
    base::span(root_page.page).first(body.bytes.size()).copy_from(body.bytes);
    status = root->PublishRoot(profile_key, public_bootstrap, root_page.page);
    if (status != FileStatus::kOk) {
      return status;
    }
    catalogue = next_catalogue;
    ++revision;
    std::set<ObjectId> live;
    status = CatalogueTree().Reachable(catalogue, &live);
    if (status == FileStatus::kOk) {
      status = catalogue_objects->Prune(live);
    }
    if (status != FileStatus::kOk) {
      // Publication succeeded but key-envelope retirement did not. Reopen
      // must complete authenticated cleanup before admitting records again.
      return FileStatus::kCommitUncertain;
    }
    for (auto& [id, record] : records) {
      if (record->dirty || record->gc_needed) {
        status = PruneRecord(*record);
        if (status != FileStatus::kOk) {
          return status == FileStatus::kIntegrity ? status
                                                  : FileStatus::kCommitUncertain;
        }
      }
      record->dirty = false;
      record->gc_needed = false;
      for (auto& [file_id, file] : record->files) {
        file->dirty = false;
      }
    }
    return FileStatus::kOk;
  }

  FileStatus OpenNative(const RecordCapability& cap, StorePurpose purpose,
                          std::string_view path, RecordOpen mode,
                          std::weak_ptr<RecordFileSystem> owner,
                          std::shared_ptr<RecordFile>* out) {
    Record* record;
    auto status = Check(cap, purpose,
        mode == RecordOpen::kOpen ? StorageAccess::kReadRecordFiles
                                  : StorageAccess::kWriteRecordFiles, &record);
    if (status != FileStatus::kOk) {
      return status;
    }
    const auto tag = PersistentStoreTag(purpose);
    NamespaceEntry entry;
    status = Parent(*record, tag, path, &entry.parent, &entry.name);
    if (status != FileStatus::kOk) {
      return status;
    }
    NamespaceEntry existing;
    status = GetEntry(*record, tag, entry.parent, entry.name, &existing);
    const bool create = status == FileStatus::kNotFound;
    if (create && mode == RecordOpen::kOpen) {
      return status;
    }
    if (!create && status != FileStatus::kOk) {
      return status;
    }
    if (!create && (mode == RecordOpen::kCreate || existing.directory)) {
      return existing.directory ? FileStatus::kIsDirectory : FileStatus::kExists;
    }
    std::shared_ptr<RecordFileState> state;
    if (!create) {
      entry = existing;
      const auto found = record->files.find(entry.root.file_id);
      if (found != record->files.end()) {
        state = found->second;
      }
    }
    if (!state) {
      state = std::make_shared<RecordFileState>();
      if (create) {
        status = EncryptedFileContents::Create(*record->objects, record->key,
                                                 cap, purpose, &state->contents);
        entry.purpose = tag;
        entry.modified_us = NowMicros();
        if (status == FileStatus::kOk) {
          entry.root = state->contents->root();
        }
      } else {
        status = EncryptedFileContents::Open(*record->objects, record->key,
            cap, purpose, entry.root, &state->contents);
      }
      if (status != FileStatus::kOk) {
        return status;
      }
      state->entry = entry;
      record->files.emplace(entry.root.file_id, state);
    }
    if (create || mode == RecordOpen::kCreateOrTruncate) {
      if (!create) {
        status = state->contents->SetLength(0);
        if (status != FileStatus::kOk) {
          return status;
        }
        state->entry.modified_us = NowMicros();
      }
      state->dirty = true;
      status = Commit();
      if (status != FileStatus::kOk) {
        return status;
      }
    }
    ++state->handles;
    out->reset(new RecordFile(std::move(owner), cap, purpose, std::move(state)));
    return FileStatus::kOk;
  }
};

RecordFileSystem::RecordFileSystem(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
RecordFileSystem::~RecordFileSystem() {
  Lock();
}

FileStatus RecordFileSystem::Create(const base::FilePath& path,
                                     SecretKey profile_key,
                                     std::vector<uint8_t> public_bootstrap,
                                     std::shared_ptr<RecordFileSystem>* out) {
  auto impl = std::make_unique<Impl>(std::move(profile_key));
  auto status = ObjectDirectory::Open(path, true, true, &impl->root);
  if (status != FileStatus::kOk) {
    return status;
  }
  impl->profile = base::UnguessableToken::Create();
  impl->public_bootstrap = std::move(public_bootstrap);
  impl->InitializeKeys();
  status = impl->root->Child(impl->catalogue_directory, true,
                              &impl->catalogue_objects);
  if (status == FileStatus::kOk) {
    status = impl->Commit();
  }
  if (status == FileStatus::kOk) {
    *out = std::shared_ptr<RecordFileSystem>(
        new RecordFileSystem(std::move(impl)));
  }
  return status;
}

FileStatus RecordFileSystem::Open(const base::FilePath& path,
                                   SecretKey profile_key,
                                   std::shared_ptr<RecordFileSystem>* out) {
  auto impl = std::make_unique<Impl>(std::move(profile_key));
  auto status = ObjectDirectory::Open(path, false, true, &impl->root);
  if (status != FileStatus::kOk) {
    return status;
  }
  WipedPage page;
  status = impl->root->ReadRoot(impl->profile_key, &page.page,
                                 &impl->public_bootstrap);
  if (status != FileStatus::kOk) {
    return status;
  }
  Decoder in(page.page);
  if (in.U32() != 1) {
    return FileStatus::kIntegrity;
  }
  impl->profile = in.Token();
  impl->revision = in.U64();
  const auto directory = in.Id();
  impl->catalogue = in.Ref();
  if (!in.valid || impl->revision == 0 || impl->revision > kMaxPublications ||
      !std::ranges::all_of(in.remaining, [](auto b) { return b == 0; })) {
    return FileStatus::kIntegrity;
  }
  impl->InitializeKeys();
  if (directory != impl->catalogue_directory) {
    return FileStatus::kIntegrity;
  }
  status = impl->root->Child(directory, false, &impl->catalogue_objects);
  if (status != FileStatus::kOk) {
    return status == FileStatus::kNotFound ? FileStatus::kIntegrity : status;
  }
  status = impl->CatalogueTree().Visit(impl->catalogue,
      [&](const ObjectId& id, base::span<const uint8_t> bytes) {
        return impl->LoadRecord(id, bytes);
      });
  if (status != FileStatus::kOk) {
    return status;
  }
  std::set<ObjectId> live;
  status = impl->CatalogueTree().Reachable(impl->catalogue, &live);
  if (status == FileStatus::kOk) {
    status = impl->catalogue_objects->Prune(live);
  }
  if (status == FileStatus::kOk) {
    // No keys or record access are returned before orphan/key-envelope cleanup.
    for (auto& [id, record] : impl->records) {
      status = impl->PruneRecord(*record);
      if (status != FileStatus::kOk) {
        return status;
      }
    }
    status = impl->root->CleanupRoot(impl->LiveDirectories());
  }
  if (status == FileStatus::kOk) {
    *out = std::shared_ptr<RecordFileSystem>(
        new RecordFileSystem(std::move(impl)));
  }
  return status;
}

FileStatus RecordFileSystem::ReadBootstrap(const base::FilePath& path,
                                            std::vector<uint8_t>* out) {
  std::unique_ptr<ObjectDirectory> directory;
  auto status = ObjectDirectory::Open(path, false, true, &directory);
  return status == FileStatus::kOk ? directory->ReadBootstrap(out) : status;
}

FileStatus RecordFileSystem::Lock() {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->open) {
    return FileStatus::kOk;
  }
  const auto status = impl_->Commit();
  impl_->WipeKeys();
  impl_->records.clear();
  impl_->deleting.clear();
  impl_->catalogue_objects.reset();
  impl_->root.reset();
  impl_->open = false;
  return status;
}

bool RecordFileSystem::unlocked() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->Admission() == FileStatus::kOk;
}
base::UnguessableToken RecordFileSystem::profile_id() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->Admission() == FileStatus::kOk ? impl_->profile
                                              : base::UnguessableToken();
}
std::vector<uint8_t> RecordFileSystem::bootstrap() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->public_bootstrap;
}
FileStatus RecordFileSystem::Sync() {
  std::lock_guard lock(impl_->mutex);
  return impl_->Observe(impl_->Commit());
}

FileStatus RecordFileSystem::ResolveRecord(const GURL& url,
                                            const url::Origin& origin,
                                            bool create, RecordInfo* out) {
  std::lock_guard lock(impl_->mutex);
  auto status = impl_->Admission();
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto domain = RecordDomainForPrincipal(url, origin);
  if (domain.empty()) {
    return FileStatus::kDenied;
  }
  const auto directory = KeyedName(impl_->lookup_key, base::as_byte_span(domain));
  if (impl_->deleting.contains(directory)) {
    return FileStatus::kBusy;
  }
  for (const auto& [id, record] : impl_->records) {
    if (record->info.directory == directory) {
      *out = record->info;
      return FileStatus::kOk;
    }
  }
  if (!create) {
    return FileStatus::kNotFound;
  }
  auto record = std::make_unique<Impl::Record>();
  record->info.id = base::UnguessableToken::Create();
  record->info.generation = base::UnguessableToken::Create();
  record->info.directory = directory;
  record->info.domain = domain;
  record->info.created_us = NowMicros();
  status = RandomKey(&record->key);
  if (status != FileStatus::kOk) {
    return status;
  }
  impl_->InitializeRecordKeys(*record);
  status = WrapKey(impl_->wrapping_key, record->key, record->binding,
                     &record->wrapped_key);
  if (status != FileStatus::kOk) {
    return status;
  }
  status = impl_->root->Child(directory, true, &record->objects);
  if (status == FileStatus::kExists) {
    // A deleted generation's files may still be pending. Never reuse them or
    // admit the replacement until their owned directory has been removed.
    status = impl_->root->RemoveChild(directory);
    if (status == FileStatus::kOk) {
      status = impl_->root->Child(directory, true, &record->objects);
    }
  }
  if (status != FileStatus::kOk) {
    return impl_->Observe(status);
  }
  record->objects->SetFaultForTesting(impl_->fault);
  record->dirty = true;
  const auto id = record->info.id;
  impl_->records.emplace(id, std::move(record));
  status = impl_->Commit();
  if (status == FileStatus::kOk) {
    *out = impl_->records.at(id)->info;
  }
  return impl_->Observe(status);
}

FileStatus RecordFileSystem::ListRecords(std::vector<RecordInfo>* out) {
  std::lock_guard lock(impl_->mutex);
  const auto status = impl_->Admission();
  if (status != FileStatus::kOk) {
    return status;
  }
  out->clear();
  for (const auto& [id, record] : impl_->records) {
    out->push_back(record->info);
  }
  return FileStatus::kOk;
}

FileStatus RecordFileSystem::SetRecordMetadata(
    base::UnguessableToken id, base::span<const uint8_t> metadata) {
  std::lock_guard lock(impl_->mutex);
  const auto status = impl_->Admission();
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end()) {
    return FileStatus::kNotFound;
  }
  if (metadata.size() > kMaxRecordMetadata) {
    return FileStatus::kInvalid;
  }
  Cleanse(found->second->info.metadata);
  found->second->info.metadata.assign(metadata.begin(), metadata.end());
  found->second->dirty = true;
  return impl_->Observe(impl_->Commit());
}

RecordCapability RecordFileSystem::Capability(base::UnguessableToken id,
                                               StorePurpose purpose) {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->records.find(id);
  return impl_->Admission() == FileStatus::kOk &&
                 found != impl_->records.end() && PersistentStoreTag(purpose)
             ? found->second->issuer->Issue(purpose) : RecordCapability();
}

FileStatus RecordFileSystem::DeleteRecord(base::UnguessableToken id,
                                           std::function<void()> teardown,
                                           RecordDeletion* result) {
  std::unique_lock lock(impl_->mutex);
  auto status = impl_->Admission();
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto found = impl_->records.find(id);
  if (found == impl_->records.end()) {
    return FileStatus::kNotFound;
  }
  auto record = std::move(found->second);
  const auto directory = record->info.directory;
  impl_->records.erase(found);
  impl_->deleting.insert(directory);
  ObjectRef next;
  status = impl_->CatalogueTree().Erase(impl_->catalogue, directory, &next);
  if (status == FileStatus::kOk) {
    impl_->catalogue = next;
    status = impl_->Commit();
  }
  // All in-flight operations were serialized before this transaction; blocked
  // operations recheck admission after this revoke, including detached files.
  record->Wipe();
  if (status != FileStatus::kOk) {
    // No deletion acknowledgement or callback: old key-bearing pages may still
    // exist. Fail closed until reopen instead of reintroducing a revoked key.
    return impl_->Observe(FileStatus::kCommitUncertain);
  }
  lock.unlock();
  if (teardown) {
    teardown();
  }
  lock.lock();
  status = impl_->open ? impl_->root->RemoveChild(directory) : FileStatus::kIo;
  impl_->deleting.erase(directory);
  *result = status == FileStatus::kOk ? RecordDeletion::kDeleted
                                     : RecordDeletion::kDeletedFilesPending;
  return status == FileStatus::kIntegrity ? impl_->Observe(status)
                                         : FileStatus::kOk;
}

FileStatus RecordFileSystem::OpenFile(const RecordCapability& cap,
                                       StorePurpose purpose,
                                       std::string_view path, RecordOpen mode,
                                       std::shared_ptr<RecordFile>* out) {
  std::shared_ptr<RecordFile> opened;
  FileStatus status;
  {
    std::lock_guard lock(impl_->mutex);
    status = impl_->Observe(impl_->OpenNative(cap, purpose, path, mode,
                                              weak_from_this(), &opened));
  }
  // Replacing a caller-owned handle can run its destructor and native Close.
  // Transfer ownership only after releasing the commit barrier, just like
  // deletion teardown; no native resource finalizer runs under this mutex.
  if (status == FileStatus::kOk) {
    *out = std::move(opened);
  }
  return status;
}

FileStatus RecordFileSystem::Stat(const RecordCapability& cap,
                                   StorePurpose purpose, std::string_view path,
                                   RecordFileInfo* out) {
  std::lock_guard lock(impl_->mutex);
  Impl::Record* record;
  auto status = impl_->Check(cap, purpose, StorageAccess::kReadRecordFiles,
                              &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (path.empty() || path == ".") {
    *out = RecordFileInfo{};
    out->directory = true;
    return FileStatus::kOk;
  }
  NamespaceEntry entry;
  status = impl_->Parent(*record, PersistentStoreTag(purpose), path,
                           &entry.parent, &entry.name);
  if (status == FileStatus::kOk) {
    status = impl_->GetEntry(*record, PersistentStoreTag(purpose), entry.parent,
                               entry.name, &entry);
  }
  if (status == FileStatus::kOk) {
    *out = impl_->Info(*record, entry);
  }
  return impl_->Observe(status);
}

FileStatus RecordFileSystem::List(const RecordCapability& cap,
                                   StorePurpose purpose, std::string_view path,
                                   std::vector<RecordDirectoryEntry>* out) {
  std::lock_guard lock(impl_->mutex);
  Impl::Record* record;
  auto status = impl_->Check(cap, purpose, StorageAccess::kReadRecordFiles,
                              &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  ObjectId parent{};
  const auto tag = PersistentStoreTag(purpose);
  if (!path.empty() && path != ".") {
    NamespaceEntry entry;
    status = impl_->Parent(*record, tag, path, &entry.parent, &entry.name);
    if (status == FileStatus::kOk) {
      status = impl_->GetEntry(*record, tag, entry.parent, entry.name, &entry);
    }
    if (status != FileStatus::kOk) {
      return impl_->Observe(status);
    }
    if (!entry.directory) {
      return FileStatus::kNotDirectory;
    }
    parent = entry.root.file_id;
  }
  out->clear();
  status = impl_->VisitEntries(*record,
      [&](const ObjectId&, const NamespaceEntry& entry) {
        if (entry.purpose == tag && entry.parent == parent) {
          out->push_back({entry.name, impl_->Info(*record, entry)});
        }
        return FileStatus::kOk;
      });
  if (status != FileStatus::kOk) {
    out->clear();
  }
  return impl_->Observe(status);
}

FileStatus RecordFileSystem::CreateDirectory(const RecordCapability& cap,
                                              StorePurpose purpose,
                                              std::string_view path,
                                              bool recursive) {
  std::lock_guard lock(impl_->mutex);
  Impl::Record* record;
  auto status = impl_->Check(cap, purpose, StorageAccess::kWriteRecordFiles,
                              &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  std::vector<std::string> parts;
  status = Components(path, &parts);
  if (status != FileStatus::kOk || parts.empty()) {
    return status;
  }
  ObjectId parent{};
  const auto tag = PersistentStoreTag(purpose);
  auto names = record->names;
  for (size_t i = 0; i < parts.size(); ++i) {
    NamespaceEntry entry;
    // Resolve against the staged root while constructing a directory chain.
    const auto old = record->names;
    record->names = names;
    status = impl_->GetEntry(*record, tag, parent, base::span(parts)[i], &entry);
    record->names = old;
    if (status == FileStatus::kOk) {
      if (!entry.directory) {
        return FileStatus::kNotDirectory;
      }
    } else if (status == FileStatus::kNotFound) {
      if (!recursive && i + 1 != parts.size()) {
        return status;
      }
      entry.purpose = tag;
      entry.parent = parent;
      entry.name = base::span(parts)[i];
      entry.directory = true;
      entry.modified_us = NowMicros();
      status = RandomId(&entry.root.file_id);
      if (status == FileStatus::kOk) {
        status = impl_->PutEntry(*record, entry, &names);
      }
      if (status != FileStatus::kOk) {
        return impl_->Observe(status);
      }
    } else {
      return impl_->Observe(status);
    }
    parent = entry.root.file_id;
  }
  record->names = names;
  record->dirty = true;
  return impl_->Observe(impl_->Commit());
}

FileStatus RecordFileSystem::Remove(const RecordCapability& cap,
                                     StorePurpose purpose,
                                     std::string_view path, bool recursive) {
  std::lock_guard lock(impl_->mutex);
  Impl::Record* record;
  auto status = impl_->Check(cap, purpose, StorageAccess::kWriteRecordFiles,
                              &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto tag = PersistentStoreTag(purpose);
  NamespaceEntry target;
  status = impl_->Parent(*record, tag, path, &target.parent, &target.name);
  if (status == FileStatus::kOk) {
    status = impl_->GetEntry(*record, tag, target.parent, target.name, &target);
  }
  if (status != FileStatus::kOk) {
    return impl_->Observe(status);
  }
  std::map<ObjectId, ObjectId> removed;
  removed.emplace(impl_->NameKey(*record, tag, target.parent, target.name),
                  target.root.file_id);
  std::set<ObjectId> parents;
  if (target.directory) {
    parents.insert(target.root.file_id);
    size_t previous = 0;
    do {
      previous = removed.size();
      status = impl_->VisitEntries(*record,
          [&](const ObjectId& key, const NamespaceEntry& entry) {
            if (entry.purpose == tag && parents.contains(entry.parent)) {
              removed.emplace(key, entry.root.file_id);
              if (entry.directory) {
                parents.insert(entry.root.file_id);
              }
            }
            return FileStatus::kOk;
          });
      if (status != FileStatus::kOk) {
        return impl_->Observe(status);
      }
      if (!recursive && removed.size() > 1) {
        return FileStatus::kNotEmpty;
      }
    } while (removed.size() != previous);
  }
  auto names = record->names;
  for (const auto& [key, id] : removed) {
    ObjectRef next;
    status = impl_->Names(*record).Erase(names, key, &next);
    if (status != FileStatus::kOk) {
      return impl_->Observe(status);
    }
    names = next;
  }
  for (const auto& [key, id] : removed) {
    const auto found = record->files.find(id);
    if (found != record->files.end()) {
      found->second->linked = false;
      found->second->dirty = false;
    }
  }
  record->names = names;
  record->dirty = true;
  return impl_->Observe(impl_->Commit());
}

FileStatus RecordFileSystem::Rename(const RecordCapability& cap,
                                     StorePurpose purpose,
                                     std::string_view from,
                                     std::string_view to) {
  std::lock_guard lock(impl_->mutex);
  Impl::Record* record;
  auto status = impl_->Check(cap, purpose, StorageAccess::kWriteRecordFiles,
                              &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto tag = PersistentStoreTag(purpose);
  NamespaceEntry source;
  NamespaceEntry target;
  status = impl_->Parent(*record, tag, from, &source.parent, &source.name);
  if (status == FileStatus::kOk) {
    status = impl_->GetEntry(*record, tag, source.parent, source.name, &source);
  }
  std::vector<ObjectId> ancestors;
  if (status == FileStatus::kOk) {
    status = impl_->Parent(*record, tag, to, &target.parent, &target.name,
                             &ancestors);
  }
  if (status != FileStatus::kOk) {
    return impl_->Observe(status);
  }
  if (source.parent == target.parent && source.name == target.name) {
    return FileStatus::kOk;
  }
  if (source.directory && std::ranges::find(ancestors, source.root.file_id) !=
                              ancestors.end()) {
    return FileStatus::kInvalid;
  }
  NamespaceEntry replaced;
  status = impl_->GetEntry(*record, tag, target.parent, target.name, &replaced);
  if (status == FileStatus::kOk) {
    if (source.directory != replaced.directory) {
      return replaced.directory ? FileStatus::kIsDirectory
                                 : FileStatus::kNotDirectory;
    }
    if (replaced.directory) {
      bool empty = true;
      status = impl_->VisitEntries(*record,
          [&](const ObjectId&, const NamespaceEntry& entry) {
            empty &= entry.purpose != tag || entry.parent != replaced.root.file_id;
            return FileStatus::kOk;
          });
      if (status != FileStatus::kOk || !empty) {
        return status == FileStatus::kOk ? FileStatus::kNotEmpty
                                         : impl_->Observe(status);
      }
    }
  } else if (status != FileStatus::kNotFound) {
    return impl_->Observe(status);
  }
  auto names = record->names;
  ObjectRef next;
  status = impl_->Names(*record).Erase(names,
      impl_->NameKey(*record, tag, source.parent, source.name), &next);
  if (status != FileStatus::kOk) {
    return impl_->Observe(status);
  }
  source.parent = target.parent;
  source.name = target.name;
  status = impl_->PutEntry(*record, source, &next);
  if (status != FileStatus::kOk) {
    return impl_->Observe(status);
  }
  const auto old = record->files.find(replaced.root.file_id);
  if (old != record->files.end()) {
    old->second->linked = false;
    old->second->dirty = false;
  }
  const auto live = record->files.find(source.root.file_id);
  if (live != record->files.end()) {
    live->second->entry.parent = target.parent;
    live->second->entry.name = target.name;
  }
  record->names = next;
  record->dirty = true;
  return impl_->Observe(impl_->Commit());
}

void RecordFileSystem::SetFaultForTesting(FileFault fault) {
  std::lock_guard lock(impl_->mutex);
  impl_->fault = std::move(fault);
  if (impl_->root) {
    impl_->root->SetFaultForTesting(impl_->fault);
    impl_->catalogue_objects->SetFaultForTesting(impl_->fault);
    for (auto& [id, record] : impl_->records) {
      record->objects->SetFaultForTesting(impl_->fault);
    }
  }
}

bool RecordFileSystem::KeysClearedForTesting() const {
  std::lock_guard lock(impl_->mutex);
  const auto zero = [](const SecretKey& key) {
    return std::ranges::all_of(key.bytes(), [](uint8_t b) { return b == 0; });
  };
  if (!zero(impl_->profile_key) || !zero(impl_->lookup_key) ||
      !zero(impl_->catalogue_key) || !zero(impl_->wrapping_key)) {
    return false;
  }
  for (const auto& [id, record] : impl_->records) {
    if (!zero(record->key) || !zero(record->namespace_key) ||
        !zero(record->naming_key)) {
      return false;
    }
  }
  return true;
}

RecordFile::RecordFile(std::weak_ptr<RecordFileSystem> owner,
                         RecordCapability capability, StorePurpose purpose,
                         std::shared_ptr<RecordFileState> state)
    : owner_(std::move(owner)), capability_(std::move(capability)),
      purpose_(purpose), state_(std::move(state)) {}
RecordFile::~RecordFile() {
  Close();
}

FileStatus RecordFile::Read(uint64_t offset, base::span<uint8_t> bytes,
                            size_t* read) {
  *read = 0;
  std::ranges::fill(bytes, 0);
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  auto status = owner->impl_->CheckHandle(*this, StorageAccess::kReadRecordFiles,
                                          &record);
  if (status == FileStatus::kOk) {
    status = state_->contents->Read(offset, bytes, read);
  }
  return owner->impl_->Observe(status);
}

FileStatus RecordFile::Write(uint64_t offset, base::span<const uint8_t> bytes) {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  auto status = owner->impl_->CheckHandle(*this, StorageAccess::kWriteRecordFiles,
                                          &record);
  if (status == FileStatus::kOk) {
    record->gc_needed = true;
    status = state_->contents->Write(offset, bytes);
    if (status == FileStatus::kOk && !bytes.empty()) {
      state_->dirty = true;
      state_->entry.modified_us = NowMicros();
    }
  }
  return owner->impl_->Observe(status);
}

FileStatus RecordFile::SetLength(uint64_t length) {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  auto status = owner->impl_->CheckHandle(*this, StorageAccess::kWriteRecordFiles,
                                          &record);
  if (status == FileStatus::kOk) {
    record->gc_needed = true;
    status = state_->contents->SetLength(length);
    if (status == FileStatus::kOk) {
      state_->dirty = true;
      state_->entry.modified_us = NowMicros();
    }
  }
  return owner->impl_->Observe(status);
}

FileStatus RecordFile::GetInfo(RecordFileInfo* out) {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  const auto status = owner->impl_->CheckHandle(*this,
      StorageAccess::kReadRecordFiles, &record);
  if (status == FileStatus::kOk) {
    *out = EntryInfo(state_->entry);
    out->length = state_->contents->root().length;
  }
  return status;
}

FileStatus RecordFile::Sync() {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  const auto status = owner->impl_->CheckHandle(*this,
      StorageAccess::kWriteRecordFiles, &record);
  return status == FileStatus::kOk ? owner->impl_->Observe(owner->impl_->Commit())
                                    : status;
}

FileStatus RecordFile::Close() {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  if (closed_) {
    return FileStatus::kOk;
  }
  RecordFileSystem::Impl::Record* record;
  auto status = owner->impl_->CheckHandle(*this, StorageAccess::kWriteRecordFiles,
                                          &record);
  closed_ = true;
  state_->locks.erase(this);
  --state_->handles;
  if (status == FileStatus::kOk && state_->dirty && state_->linked) {
    status = owner->impl_->Commit();
  }
  if (status == FileStatus::kOk && state_->handles == 0 &&
      (!state_->dirty || !state_->linked)) {
    state_->contents->Revoke();
    record->files.erase(state_->contents->root().file_id);
    record->gc_needed = true;
  }
  return owner->impl_->Observe(status);
}

FileStatus RecordFile::Lock(RecordFileLock requested) {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  const auto status = owner->impl_->CheckHandle(*this,
      StorageAccess::kReadRecordFiles, &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  auto& own = state_->locks[this];
  if (requested < own || requested == RecordFileLock::kNone) {
    return FileStatus::kInvalid;
  }
  for (const auto& [handle, held] : state_->locks) {
    if (handle == this || held == RecordFileLock::kNone) {
      continue;
    }
    if ((requested == RecordFileLock::kShared && held >= RecordFileLock::kPending) ||
        (requested >= RecordFileLock::kReserved && held >= RecordFileLock::kReserved)) {
      return FileStatus::kBusy;
    }
  }
  if (requested == RecordFileLock::kExclusive &&
      std::ranges::any_of(state_->locks, [&](const auto& entry) {
        return entry.first != this && entry.second != RecordFileLock::kNone;
      })) {
    own = RecordFileLock::kPending;
    return FileStatus::kBusy;
  }
  own = requested;
  return FileStatus::kOk;
}

FileStatus RecordFile::Unlock(RecordFileLock requested) {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  const auto status = owner->impl_->CheckHandle(*this,
      StorageAccess::kReadRecordFiles, &record);
  if (status != FileStatus::kOk) {
    return status;
  }
  auto& held = state_->locks[this];
  if (requested > held || requested > RecordFileLock::kShared) {
    return FileStatus::kInvalid;
  }
  held = requested;
  return FileStatus::kOk;
}

FileStatus RecordFile::HasReservedLock(bool* out) {
  const auto owner = owner_.lock();
  if (!owner) {
    return FileStatus::kDenied;
  }
  std::lock_guard lock(owner->impl_->mutex);
  RecordFileSystem::Impl::Record* record;
  const auto status = owner->impl_->CheckHandle(*this,
      StorageAccess::kReadRecordFiles, &record);
  if (status == FileStatus::kOk) {
    *out = std::ranges::any_of(state_->locks, [](const auto& entry) {
      return entry.second >= RecordFileLock::kReserved;
    });
  }
  return status;
}

bool RecordFile::KeysClearedForTesting() const {
  const auto owner = owner_.lock();
  if (owner) {
    std::lock_guard lock(owner->impl_->mutex);
    return state_->contents->KeysClearedForTesting();
  }
  return state_->contents->KeysClearedForTesting();
}

}  // namespace alloy::site_records
