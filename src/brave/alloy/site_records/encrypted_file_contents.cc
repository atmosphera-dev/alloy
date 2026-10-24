// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/encrypted_file_contents.h"

#include <algorithm>
#include <utility>

#include "brave/alloy/site_records/encrypted_tree.h"

namespace alloy::site_records {
namespace {

struct CleansedPage {
  ObjectPage bytes{};
  ~CleansedPage() { Cleanse(bytes); }
};

void Append64(std::vector<uint8_t>* out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out->push_back(static_cast<uint8_t>(value >> shift));
  }
}

void AppendToken(std::vector<uint8_t>* out, const base::UnguessableToken& token) {
  Append64(out, token.GetHighForSerialization());
  Append64(out, token.GetLowForSerialization());
}

void AppendTag(std::vector<uint8_t>* out, uint32_t tag) {
  for (int shift : {24, 16, 8, 0}) {
    out->push_back(static_cast<uint8_t>(tag >> shift));
  }
}

std::vector<uint8_t> FileBinding(const RecordCapability& capability,
                                 StorePurpose purpose,
                                 const ObjectId& file_id) {
  // Fixed-width, big-endian fields in a versioned schema; AdditionalData()
  // length-delimits this entire schema and binds the format, type and index.
  std::vector<uint8_t> result;
  AppendToken(&result, capability.profile());
  AppendToken(&result, capability.record());
  AppendToken(&result, capability.generation());
  AppendTag(&result, PersistentStoreTag(purpose));
  result.insert(result.end(), file_id.begin(), file_id.end());
  return result;
}

SecretKey FileKey(const SecretKey& record_key,
                   const RecordCapability& capability,
                   StorePurpose purpose,
                   const ObjectId& id) {
  constexpr std::string_view kInfo = "alloy-file-v1";
  std::vector<uint8_t> info(kInfo.begin(), kInfo.end());
  AppendTag(&info, PersistentStoreTag(purpose));
  AppendToken(&info, capability.generation());
  return DeriveKey(record_key, id, info);
}

ObjectId ChunkKey(uint64_t index) {
  ObjectId key{};
  auto bytes = base::span(key).last<8>();
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = index >> (56 - i * 8);
  }
  return key;
}

bool ChunkIndex(const ObjectId& key, uint64_t* index) {
  if (!std::ranges::all_of(base::span(key).first<24>(),
                           [](uint8_t b) { return b == 0; })) {
    return false;
  }
  *index = 0;
  for (uint8_t b : base::span(key).last<8>()) {
    *index = (*index << 8) | b;
  }
  return true;
}

std::array<uint8_t, 64> EncodeRef(const ObjectRef& ref) {
  std::array<uint8_t, 64> bytes;
  base::span(bytes).first<32>().copy_from(ref.id);
  base::span(bytes).last<32>().copy_from(ref.digest);
  return bytes;
}

FileStatus DecodeRef(base::span<const uint8_t> bytes, ObjectRef* ref) {
  if (bytes.size() != 64) {
    return FileStatus::kIntegrity;
  }
  base::span(ref->id).copy_from(bytes.first<32>());
  base::span(ref->digest).copy_from(bytes.last<32>());
  return ref->id == ObjectId{} || ref->digest == ObjectId{}
             ? FileStatus::kIntegrity
             : FileStatus::kOk;
}

uint64_t ChunkCount(uint64_t length) {
  return (length + kRecordChunkSize - 1) / kRecordChunkSize;
}

}  // namespace

uint32_t PersistentStoreTag(StorePurpose purpose) {
  switch (purpose) {
    case StorePurpose::kCookies: return 1;
    case StorePurpose::kDomStorage: return 2;
    case StorePurpose::kIndexedDb: return 3;
    case StorePurpose::kCacheStorage: return 4;
    case StorePurpose::kFileSystem: return 5;
    case StorePurpose::kServiceWorkers: return 6;
    case StorePurpose::kBackgroundJobs: return 7;
    case StorePurpose::kHttpCache: return 8;
    case StorePurpose::kCodeCache: return 9;
    case StorePurpose::kHistory: return 10;
    case StorePurpose::kVisitedLinks: return 11;
    case StorePurpose::kFavicons: return 12;
    case StorePurpose::kSiteSettings: return 13;
    case StorePurpose::kQuota: return 14;
    case StorePurpose::kTransportSecurity: return 15;
    case StorePurpose::kServerProperties: return 16;
    case StorePurpose::kReportingConfiguration: return 17;
    case StorePurpose::kSharedDictionaries: return 18;
    case StorePurpose::kMediaDeviceSalts: return 19;
    case StorePurpose::kOriginTrials: return 20;
    case StorePurpose::kNotifications: return 21;
    case StorePurpose::kBlobSpill: return 22;
    case StorePurpose::kDocumentWorkingState:
    case StorePurpose::kNetworkWorkingState:
      return 0;
  }
  return 0;
}

EncryptedFileContents::EncryptedFileContents(
    ObjectDirectory& directory,
    const SecretKey& record_key,
    RecordCapability capability,
    StorePurpose purpose,
    const FileContentsRoot& root)
    : directory_(directory),
      capability_(std::move(capability)),
      purpose_(purpose),
      binding_(FileBinding(capability_, purpose_, root.file_id)),
      file_key_(FileKey(record_key, capability_, purpose_, root.file_id)),
      map_key_(DeriveKey(file_key_, {},
                        base::as_byte_span(
                            std::string_view("alloy-chunk-map-v1")))),
      root_(root) {}

EncryptedFileContents::~EncryptedFileContents() {
  Revoke();
}

FileStatus EncryptedFileContents::Create(
    ObjectDirectory& directory,
    const SecretKey& record_key,
    RecordCapability capability,
    StorePurpose purpose,
    std::unique_ptr<EncryptedFileContents>* out) {
  if (!capability.Allows(purpose, StorageAccess::kWriteRecordFiles) ||
      PersistentStoreTag(purpose) == 0) {
    return FileStatus::kDenied;
  }
  FileContentsRoot root;
  const auto status = RandomId(&root.file_id);
  if (status != FileStatus::kOk) {
    return status;
  }
  out->reset(new EncryptedFileContents(directory, record_key,
                                       std::move(capability), purpose, root));
  return FileStatus::kOk;
}

FileStatus EncryptedFileContents::Open(
    ObjectDirectory& directory,
    const SecretKey& record_key,
    RecordCapability capability,
    StorePurpose purpose,
    const FileContentsRoot& root,
    std::unique_ptr<EncryptedFileContents>* out) {
  if (!capability.Allows(purpose, StorageAccess::kReadRecordFiles) ||
      PersistentStoreTag(purpose) == 0) {
    return FileStatus::kDenied;
  }
  if (root.file_id == ObjectId{} || root.length > kMaxLength ||
      root.revision > kMaxRevision) {
    return FileStatus::kIntegrity;
  }
  auto file = std::unique_ptr<EncryptedFileContents>(new EncryptedFileContents(
      directory, record_key, std::move(capability), purpose, root));
  const auto status = file->ValidateMap();
  if (status == FileStatus::kOk) {
    *out = std::move(file);
  }
  return status;
}

FileStatus EncryptedFileContents::Admission(StorageAccess access) const {
  if (revoked_ || !capability_.Allows(purpose_, access)) {
    return FileStatus::kDenied;
  }
  return failure_;
}

FileStatus EncryptedFileContents::Remember(FileStatus status) {
  if (status == FileStatus::kIntegrity) {
    failure_ = status;
  }
  return status;
}

void EncryptedFileContents::Revoke() {
  revoked_ = true;
  file_key_.Clear();
  map_key_.Clear();
}

bool EncryptedFileContents::KeysClearedForTesting() const {
  const auto zero = [](const SecretKey& key) {
    return std::ranges::all_of(key.bytes(), [](uint8_t byte) { return byte == 0; });
  };
  return zero(file_key_) && zero(map_key_);
}

FileStatus EncryptedFileContents::Reachable(std::set<ObjectId>* objects) {
  auto status = Admission(StorageAccess::kReadRecordFiles);
  if (status != FileStatus::kOk) {
    return status;
  }
  EncryptedTree tree(directory_, ObjectType::kChunkMap, map_key_, binding_);
  std::set<ObjectId> live;
  status = tree.Reachable(root_.chunks, &live);
  if (status == FileStatus::kOk) {
    status = tree.Visit(root_.chunks,
        [&](const ObjectId& key, base::span<const uint8_t> value) {
          uint64_t index;
          ObjectRef ref;
          if (!ChunkIndex(key, &index) || index >= ChunkCount(root_.length) ||
              DecodeRef(value, &ref) != FileStatus::kOk ||
              !live.insert(ref.id).second) {
            return FileStatus::kIntegrity;
          }
          return FileStatus::kOk;
        });
  }
  if (status == FileStatus::kOk) {
    objects->insert(live.begin(), live.end());
  }
  return Remember(status);
}

FileStatus EncryptedFileContents::ValidateMap() {
  EncryptedTree tree(directory_, ObjectType::kChunkMap, map_key_, binding_);
  return tree.Visit(root_.chunks,
                    [&](const ObjectId& key, base::span<const uint8_t> value) {
                      uint64_t index;
                      ObjectRef ref;
                      return ChunkIndex(key, &index) &&
                                     index < ChunkCount(root_.length)
                                 ? DecodeRef(value, &ref)
                                 : FileStatus::kIntegrity;
                    });
}

FileStatus EncryptedFileContents::ReadChunk(uint64_t index, ObjectPage* page) {
  EncryptedTree tree(directory_, ObjectType::kChunkMap, map_key_, binding_);
  std::vector<uint8_t> value;
  auto status = tree.Get(root_.chunks, ChunkKey(index), &value);
  if (status == FileStatus::kNotFound) {
    page->fill(0);
    return FileStatus::kOk;
  }
  if (status != FileStatus::kOk) {
    return Remember(status);
  }
  ObjectRef ref;
  status = DecodeRef(value, &ref);
  if (status != FileStatus::kOk || index >= ChunkCount(root_.length)) {
    return Remember(FileStatus::kIntegrity);
  }
  status = directory_.Read(ObjectType::kData, file_key_, binding_, index, ref,
                            page);
  if (status == FileStatus::kOk && root_.length % kRecordChunkSize != 0 &&
      index == root_.length / kRecordChunkSize &&
      !std::ranges::all_of(
          base::span(*page).subspan(
              static_cast<size_t>(root_.length % kRecordChunkSize)),
          [](uint8_t byte) { return byte == 0; })) {
    status = FileStatus::kIntegrity;
  }
  return Remember(status);
}

FileStatus EncryptedFileContents::SealChunk(uint64_t index,
                                             const ObjectPage& page,
                                             ObjectRef* chunks) {
  ObjectRef extent;
  auto status = directory_.Seal(ObjectType::kData, file_key_, binding_, index,
                                 page, &extent);
  if (status != FileStatus::kOk) {
    return status;
  }
  EncryptedTree tree(directory_, ObjectType::kChunkMap, map_key_, binding_);
  ObjectRef updated;
  status = tree.Put(*chunks, ChunkKey(index), EncodeRef(extent), &updated);
  if (status == FileStatus::kOk) {
    *chunks = updated;
  }
  return Remember(status);
}

FileStatus EncryptedFileContents::Read(uint64_t offset,
                                        base::span<uint8_t> out,
                                        size_t* read) {
  // Never leave stale caller bytes masquerading as a successful short read or
  // as plaintext returned before an authentication failure later in a range.
  *read = 0;
  std::ranges::fill(out, 0);
  auto status = Admission(StorageAccess::kReadRecordFiles);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (offset > kMaxLength) {
    return FileStatus::kInvalid;
  }
  const auto length = static_cast<size_t>(std::min<uint64_t>(
      out.size(), root_.length > offset ? root_.length - offset : 0));
  size_t done = 0;
  while (done < length) {
    CleansedPage page;
    const auto position = offset + done;
    const size_t begin = position % kRecordChunkSize;
    const auto count = std::min(length - done, kRecordChunkSize - begin);
    status = ReadChunk(position / kRecordChunkSize, &page.bytes);
    if (status != FileStatus::kOk) {
      Cleanse(out);
      return status;
    }
    out.subspan(done, count).copy_from(
        base::span(page.bytes).subspan(begin, count));
    done += count;
  }
  status = Admission(StorageAccess::kReadRecordFiles);
  if (status == FileStatus::kOk) {
    *read = done;
  } else {
    Cleanse(out);
  }
  return status;
}

FileStatus EncryptedFileContents::Write(uint64_t offset,
                                         base::span<const uint8_t> bytes) {
  auto status = Admission(StorageAccess::kWriteRecordFiles);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (offset > kMaxLength || bytes.size() > kMaxLength - offset ||
      root_.revision == kMaxRevision) {
    return FileStatus::kInvalid;
  }
  if (bytes.empty()) {
    return FileStatus::kOk;
  }
  auto chunks = root_.chunks;
  size_t done = 0;
  while (done < bytes.size()) {
    CleansedPage page;
    const auto position = offset + done;
    const size_t begin = position % kRecordChunkSize;
    const auto count = std::min(bytes.size() - done, kRecordChunkSize - begin);
    // Verify even a full overwrite of a mapped old chunk. Otherwise a write
    // could silently repair corruption and hide lost user data from its owner.
    status = ReadChunk(position / kRecordChunkSize, &page.bytes);
    if (status != FileStatus::kOk) {
      return status;
    }
    base::span(page.bytes).subspan(begin, count).copy_from(
        bytes.subspan(done, count));
    status = SealChunk(position / kRecordChunkSize, page.bytes, &chunks);
    if (status != FileStatus::kOk) {
      return status;
    }
    done += count;
  }
  status = Admission(StorageAccess::kWriteRecordFiles);
  if (status == FileStatus::kOk) {
    root_.chunks = chunks;
    root_.length = std::max(root_.length, offset + bytes.size());
    ++root_.revision;
  }
  return status;
}

FileStatus EncryptedFileContents::SetLength(uint64_t length) {
  auto status = Admission(StorageAccess::kWriteRecordFiles);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (length > kMaxLength || root_.revision == kMaxRevision) {
    return FileStatus::kInvalid;
  }
  if (length == root_.length) {
    return FileStatus::kOk;
  }
  auto chunks = root_.chunks;
  if (length < root_.length) {
    EncryptedTree tree(directory_, ObjectType::kChunkMap, map_key_, binding_);
    std::vector<ObjectId> removed;
    status = tree.Visit(chunks, [&](const ObjectId& key,
                                     base::span<const uint8_t> value) {
      uint64_t index;
      ObjectRef ref;
      if (!ChunkIndex(key, &index) || index >= ChunkCount(root_.length) ||
          DecodeRef(value, &ref) != FileStatus::kOk) {
        return FileStatus::kIntegrity;
      }
      if (index >= ChunkCount(length)) {
        removed.push_back(key);
      }
      return FileStatus::kOk;
    });
    if (status != FileStatus::kOk) {
      return Remember(status);
    }
    for (const auto& key : removed) {
      ObjectRef updated;
      status = tree.Erase(chunks, key, &updated);
      if (status != FileStatus::kOk) {
        return Remember(status);
      }
      chunks = updated;
    }
    if (length % kRecordChunkSize != 0) {
      CleansedPage tail;
      status = ReadChunk(length / kRecordChunkSize, &tail.bytes);
      if (status != FileStatus::kOk) {
        return status;
      }
      std::ranges::fill(base::span(tail.bytes).subspan(
                            static_cast<size_t>(length % kRecordChunkSize)),
                        0);
      status = SealChunk(length / kRecordChunkSize, tail.bytes, &chunks);
      if (status != FileStatus::kOk) {
        return status;
      }
    }
  }
  status = Admission(StorageAccess::kWriteRecordFiles);
  if (status == FileStatus::kOk) {
    root_.chunks = chunks;
    root_.length = length;
    ++root_.revision;
  }
  return status;
}

}  // namespace alloy::site_records
