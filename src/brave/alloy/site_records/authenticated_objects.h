// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_AUTHENTICATED_OBJECTS_H_
#define BRAVE_ALLOY_SITE_RECORDS_AUTHENTICATED_OBJECTS_H_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"

namespace alloy::site_records {

// These errors survive translation into native VFS/Env/cache errors. In
// particular, kIntegrity must never be translated into "missing, create new".
enum class FileStatus {
  kOk,
  kNotFound,
  kExists,
  kInvalid,
  kDenied,
  kBusy,
  kIo,
  kIntegrity,
  kNotEmpty,
  kNotDirectory,
  kIsDirectory,
  // A root rename happened but the durability fence failed. The coordinator
  // must close admission until reopen; retrying with an old in-memory root
  // could overwrite an actually committed transaction.
  kCommitUncertain,
  // An asynchronous settings request lost its original admission before reply.
  kCancelled,
};

// Expected lifecycle/admission refusals are not storage failures. Keep their
// identity at native adapter boundaries; never infer this from a later fence.
constexpr bool IsAdmissionRefusal(FileStatus status) {
  return status == FileStatus::kDenied || status == FileStatus::kBusy ||
      status == FileStatus::kCancelled;
}

constexpr size_t kRecordChunkSize = 4096;
using ObjectId = std::array<uint8_t, 32>;
using ObjectPage = std::array<uint8_t, kRecordChunkSize>;

// Noncopyable key material. Moves wipe their source, replacement wipes its old
// value, and destruction uses the crypto provider's non-elidable cleansing.
class SecretKey {
 public:
  SecretKey();
  explicit SecretKey(base::span<const uint8_t, 32> bytes);
  SecretKey(SecretKey&& other) noexcept;
  SecretKey& operator=(SecretKey&& other) noexcept;
  SecretKey(const SecretKey&) = delete;
  SecretKey& operator=(const SecretKey&) = delete;
  ~SecretKey();
  base::span<const uint8_t, 32> bytes() const { return bytes_; }
  void Clear();

 private:
  ObjectId bytes_{};
};

void Cleanse(base::span<uint8_t> bytes);
FileStatus RandomId(ObjectId* id);
FileStatus RandomKey(SecretKey* key);
SecretKey DeriveKey(const SecretKey& key,
                    base::span<const uint8_t> salt,
                    base::span<const uint8_t> info);
ObjectId KeyedName(const SecretKey& key, base::span<const uint8_t> name);
ObjectId Digest(base::span<const uint8_t> bytes);
std::string ObjectName(const ObjectId& id);
FileStatus WrapKey(const SecretKey& wrapping_key,
                   const SecretKey& key,
                   base::span<const uint8_t> binding,
                   std::vector<uint8_t>* envelope);
FileStatus UnwrapKey(const SecretKey& wrapping_key,
                     base::span<const uint8_t> envelope,
                     base::span<const uint8_t> binding,
                     SecretKey* key);

struct ObjectRef {
  ObjectId id{};
  ObjectId digest{};
  bool empty() const;
  friend bool operator==(const ObjectRef&, const ObjectRef&) = default;
};

// These numbers are part of format v1, independent of C++ enum order elsewhere.
enum class ObjectType : uint32_t {
  kData = 1,
  kChunkMap = 2,
  kNamespace = 3,
  kCatalogue = 4,
  kRoot = 5,
  kKeyEnvelope = 6,
};

enum class PublishFence {
  kObjectWritten,
  kObjectSynced,
  kObjectsDirectorySynced,
  kRootWritten,
  kRootSynced,
  kRootRenamed,
  kRootDirectorySynced,
  kObsoleteObjectsUnlinked,
  kObsoleteDirectorySynced,
  kRecordDirectoryUnlinked,
  kRecordDirectorySynced,
};

// Used by fault-injection tests; the production default is empty. The path is
// an opaque physical object, never a native filename or domain. Runs on the
// serialized coordinator sequence, not from an untrusted store callback.
using FileFault =
    std::function<FileStatus(PublishFence, const base::FilePath&)>;

// One owning directory descriptor; relative operations never follow a symlink.
// The profile root is flock-locked for the entire coordinator lifetime. This
// rejects accidental second owners across processes AND within one process.
// Adapters share that coordinator, not a mutable process-global encryption key.
class ObjectDirectory {
 public:
  static FileStatus Open(const base::FilePath& path,
                         bool create,
                         bool exclusive_owner,
                         std::unique_ptr<ObjectDirectory>* out);
  ~ObjectDirectory();
  ObjectDirectory(const ObjectDirectory&) = delete;
  ObjectDirectory& operator=(const ObjectDirectory&) = delete;

  FileStatus Child(const ObjectId& name,
                   bool create,
                   std::unique_ptr<ObjectDirectory>* out);

  // Every call allocates a fresh random 256-bit extent ID, even on retry.
  // Exactly one (extent key, chunk index) seal is allowed per physical object.
  // Binding is canonical coordinator-supplied profile/record/generation/
  // purpose/file identity. It must be identical at read, never read from disk.
  FileStatus Seal(ObjectType type,
                  const SecretKey& key,
                  base::span<const uint8_t> binding,
                  uint64_t index,
                  base::span<const uint8_t, kRecordChunkSize> plaintext,
                  ObjectRef* out);
  FileStatus Read(ObjectType type,
                  const SecretKey& key,
                  base::span<const uint8_t> binding,
                  uint64_t index,
                  const ObjectRef& ref,
                  ObjectPage* out) const;
  FileStatus Sync();

  // The bootstrap contains ONLY public key-source parameters and the wrapped
  // random profile key. It is also AEAD-bound to the root. ReadBootstrap does
  // not authenticate; the caller must unwrap the key and ReadRoot before
  // trusting any contents. No private profile/site metadata goes here.
  FileStatus ReadBootstrap(std::vector<uint8_t>* out) const;
  FileStatus ReadRoot(const SecretKey& key,
                      ObjectPage* page,
                      std::vector<uint8_t>* bootstrap) const;
  FileStatus PublishRoot(const SecretKey& key,
                         base::span<const uint8_t> bootstrap,
                         base::span<const uint8_t, kRecordChunkSize> page);

  // Only call on the catalogue-object directory after its new root is durable.
  // Key-bearing pages are individual unlinkable objects, never append-packed
  // alongside live pages. Failure prevents acknowledgement of key destruction.
  FileStatus Prune(const std::set<ObjectId>& live);
  // The profile root contains the authoritative "root", catalogue directory,
  // record directories and possibly unpublished opaque root envelopes. After
  // authenticating root, remove ONLY unreferenced owned opaque objects/dirs.
  // An unexpected name/type is an integrity error, never followed or ignored.
  FileStatus CleanupRoot(const std::set<ObjectId>& live_directories);
  FileStatus RemoveChild(const ObjectId& name);
  void SetFaultForTesting(FileFault fault);
  const base::FilePath& path() const { return path_; }

 private:
  ObjectDirectory(base::FilePath path, base::File directory);
  FileStatus Fault(PublishFence fence, const base::FilePath& path) const;
  FileStatus ReadPhysical(const std::string& name,
                          size_t maximum,
                          std::vector<uint8_t>* out) const;

  const base::FilePath path_;
  base::File directory_;
  FileFault fault_;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_AUTHENTICATED_OBJECTS_H_
