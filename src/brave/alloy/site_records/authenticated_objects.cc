// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/authenticated_objects.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <utility>

#include "base/posix/eintr_wrapper.h"
#include "crypto/aead.h"
#include "crypto/hash.h"
#include "crypto/kdf.h"
#include "third_party/boringssl/src/include/openssl/crypto.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "third_party/boringssl/src/include/openssl/sha.h"

namespace alloy::site_records {
namespace {

constexpr size_t kCipherSize = kRecordChunkSize + 16;
constexpr size_t kMaxBootstrap = 4096;
constexpr size_t kRootPrefix = 8 + 32 + 4;
constexpr std::array<uint8_t, 8> kMagic = {'A', 'L', 'L', 'O', 'Y', 1, 1, 0};
constexpr std::array<uint8_t, 20> kExtentInfo = {
    'a', 'l', 'l', 'o', 'y', '-', 'd', 'a', 't', 'a', '-',
    'e', 'x', 't', 'e', 'n', 't', '-', 'v', '1'};

void Append32(std::vector<uint8_t>* bytes, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) {
    bytes->push_back(static_cast<uint8_t>(value >> shift));
  }
}

void Append64(std::vector<uint8_t>* bytes, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes->push_back(static_cast<uint8_t>(value >> shift));
  }
}

std::vector<uint8_t> AdditionalData(ObjectType type,
                                    base::span<const uint8_t> binding,
                                    const ObjectId& id,
                                    uint64_t index,
                                    uint32_t plaintext_size = kRecordChunkSize) {
  std::vector<uint8_t> bytes(kMagic.begin(), kMagic.end());
  Append32(&bytes, static_cast<uint32_t>(type));
  Append32(&bytes, binding.size());
  bytes.insert(bytes.end(), binding.begin(), binding.end());
  bytes.insert(bytes.end(), id.begin(), id.end());
  Append64(&bytes, index);
  Append32(&bytes, plaintext_size);
  return bytes;
}

std::array<uint8_t, 12> Nonce(ObjectType type, uint64_t index) {
  std::array<uint8_t, 12> result{};
  const auto tag = static_cast<uint32_t>(type);
  auto bytes = base::span(result);
  for (size_t i = 0; i < 4; ++i) {
    bytes[i] = static_cast<uint8_t>(tag >> (24 - i * 8));
  }
  for (size_t i = 0; i < 8; ++i) {
    bytes[4 + i] = static_cast<uint8_t>(index >> (56 - i * 8));
  }
  return result;
}

SecretKey ExtentKey(const SecretKey& key,
                    const ObjectId& id,
                    ObjectType type) {
  std::vector<uint8_t> info(kExtentInfo.begin(), kExtentInfo.end());
  Append32(&info, static_cast<uint32_t>(type));
  return DeriveKey(key, id, info);
}

FileStatus OpenDirectory(const base::FilePath& path,
                          bool create,
                          bool exclusive_owner,
                          base::File* directory) {
  if (create && mkdir(path.value().c_str(), 0700) != 0) {
    return errno == EEXIST ? FileStatus::kExists : FileStatus::kIo;
  }
  base::File opened(HANDLE_EINTR(open(path.value().c_str(),
                                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
                                        O_CLOEXEC)));
  if (!opened.IsValid()) {
    return errno == ENOENT ? FileStatus::kNotFound : FileStatus::kIo;
  }
  struct stat st;
  if (fstat(opened.GetPlatformFile(), &st) != 0 ||
      st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
    return FileStatus::kDenied;
  }
  if (exclusive_owner &&
      flock(opened.GetPlatformFile(), LOCK_EX | LOCK_NB) != 0) {
    return FileStatus::kBusy;
  }
  *directory = std::move(opened);
  return FileStatus::kOk;
}

bool ParseId(std::string_view text, ObjectId* out) {
  if (text.size() != 64) {
    return false;
  }
  auto bytes = base::span(*out);
  for (size_t i = 0; i < bytes.size(); ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    const int high = nibble(text[2 * i]);
    const int low = nibble(text[2 * i + 1]);
    if (high < 0 || low < 0) {
      return false;
    }
    bytes[i] = (high << 4) | low;
  }
  return true;
}

FileStatus ParseRoot(base::span<const uint8_t> bytes,
                       ObjectId* id,
                       std::vector<uint8_t>* bootstrap,
                       base::span<const uint8_t>* ciphertext) {
  if (bytes.size() < kRootPrefix + kCipherSize ||
      !std::ranges::equal(bytes.first(kMagic.size()), kMagic)) {
    return FileStatus::kIntegrity;
  }
  base::span(*id).copy_from(bytes.subspan<8, 32>());
  uint32_t length = 0;
  for (uint8_t byte : bytes.subspan<40, 4>()) {
    length = (length << 8) | byte;
  }
  if (length > kMaxBootstrap ||
      bytes.size() != kRootPrefix + length + kCipherSize) {
    return FileStatus::kIntegrity;
  }
  const auto header = bytes.subspan(kRootPrefix, length);
  bootstrap->assign(header.begin(), header.end());
  *ciphertext = bytes.last(kCipherSize);
  return FileStatus::kOk;
}

}  // namespace

SecretKey::SecretKey() = default;
SecretKey::SecretKey(base::span<const uint8_t, 32> bytes) {
  base::span(bytes_).copy_from(bytes);
}
SecretKey::SecretKey(SecretKey&& other) noexcept : bytes_(other.bytes_) {
  other.Clear();
}
SecretKey& SecretKey::operator=(SecretKey&& other) noexcept {
  if (this != &other) {
    Clear();
    bytes_ = other.bytes_;
    other.Clear();
  }
  return *this;
}
SecretKey::~SecretKey() {
  Clear();
}
void SecretKey::Clear() {
  Cleanse(bytes_);
}
void Cleanse(base::span<uint8_t> bytes) {
  OPENSSL_cleanse(bytes.data(), bytes.size());
}
FileStatus RandomId(ObjectId* id) {
  // BoringSSL's OS RNG aborts on entropy failure, rather than using a weak
  // fallback. Preserve its explicit failure return too.
  return RAND_bytes(id->data(), id->size()) == 1 ? FileStatus::kOk
                                                : FileStatus::kIo;
}
FileStatus RandomKey(SecretKey* key) {
  ObjectId bytes{};
  const auto status = RandomId(&bytes);
  if (status == FileStatus::kOk) {
    *key = SecretKey(bytes);
  }
  Cleanse(bytes);
  return status;
}
SecretKey DeriveKey(const SecretKey& key,
                    base::span<const uint8_t> salt,
                    base::span<const uint8_t> info) {
  auto bytes = crypto::kdf::Hkdf<32>(crypto::hash::kSha256, key.bytes(), salt,
                                      info);
  SecretKey result(bytes);
  Cleanse(bytes);
  return result;
}
ObjectId KeyedName(const SecretKey& key, base::span<const uint8_t> name) {
  ObjectId result{};
  unsigned int length = 0;
  HMAC(EVP_sha256(), key.bytes().data(), key.bytes().size(), name.data(),
       name.size(), result.data(), &length);
  return result;
}
ObjectId Digest(base::span<const uint8_t> bytes) {
  ObjectId result{};
  SHA256(bytes.data(), bytes.size(), result.data());
  return result;
}
std::string ObjectName(const ObjectId& id) {
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string name;
  name.reserve(64);
  for (uint8_t byte : id) {
    name.push_back(kHex[byte >> 4]);
    name.push_back(kHex[byte & 15]);
  }
  return name;
}
bool ObjectRef::empty() const {
  return id == ObjectId{} && digest == ObjectId{};
}

FileStatus WrapKey(const SecretKey& wrapping_key,
                     const SecretKey& key,
                     base::span<const uint8_t> binding,
                     std::vector<uint8_t>* envelope) {
  if (binding.size() > kRecordChunkSize) {
    return FileStatus::kInvalid;
  }
  ObjectId id;
  const auto status = RandomId(&id);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto type = ObjectType::kKeyEnvelope;
  const auto subkey = ExtentKey(wrapping_key, id, type);
  const auto cipher = crypto::aead::Seal(
      crypto::aead::AES_256_GCM_SIV, subkey.bytes(), key.bytes(),
      Nonce(type, 0), AdditionalData(type, binding, id, 0, 32));
  if (cipher.size() != 48) {
    return FileStatus::kIo;
  }
  envelope->assign(id.begin(), id.end());
  envelope->insert(envelope->end(), cipher.begin(), cipher.end());
  return FileStatus::kOk;
}

FileStatus UnwrapKey(const SecretKey& wrapping_key,
                       base::span<const uint8_t> envelope,
                       base::span<const uint8_t> binding,
                       SecretKey* key) {
  if (envelope.size() != 80 || binding.size() > kRecordChunkSize) {
    return FileStatus::kIntegrity;
  }
  ObjectId id;
  base::span(id).copy_from(envelope.first<32>());
  const auto type = ObjectType::kKeyEnvelope;
  const auto subkey = ExtentKey(wrapping_key, id, type);
  auto plaintext = crypto::aead::Open(
      crypto::aead::AES_256_GCM_SIV, subkey.bytes(), envelope.last<48>(),
      Nonce(type, 0), AdditionalData(type, binding, id, 0, 32));
  if (!plaintext) {
    return FileStatus::kIntegrity;
  }
  if (plaintext->size() != 32) {
    Cleanse(*plaintext);
    return FileStatus::kIntegrity;
  }
  *key = SecretKey(base::span(*plaintext).first<32>());
  Cleanse(*plaintext);
  return FileStatus::kOk;
}

ObjectDirectory::ObjectDirectory(base::FilePath path, base::File directory)
    : path_(std::move(path)), directory_(std::move(directory)) {}
ObjectDirectory::~ObjectDirectory() = default;

FileStatus ObjectDirectory::Open(const base::FilePath& path,
                                  bool create,
                                  bool exclusive_owner,
                                  std::unique_ptr<ObjectDirectory>* out) {
  base::File directory;
  const auto status = OpenDirectory(path, create, exclusive_owner, &directory);
  if (status == FileStatus::kOk) {
    out->reset(new ObjectDirectory(path, std::move(directory)));
  }
  return status;
}

FileStatus ObjectDirectory::Child(const ObjectId& name,
                                   bool create,
                                   std::unique_ptr<ObjectDirectory>* out) {
  const std::string physical = ObjectName(name);
  if (create && mkdirat(directory_.GetPlatformFile(), physical.c_str(), 0700) !=
                    0) {
    return errno == EEXIST ? FileStatus::kExists : FileStatus::kIo;
  }
  base::File child(HANDLE_EINTR(openat(
      directory_.GetPlatformFile(), physical.c_str(),
      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)));
  if (!child.IsValid()) {
    return errno == ENOENT ? FileStatus::kNotFound : FileStatus::kIntegrity;
  }
  struct stat st;
  if (fstat(child.GetPlatformFile(), &st) != 0 || st.st_uid != geteuid() ||
      (st.st_mode & 0077) != 0) {
    return FileStatus::kDenied;
  }
  auto result = std::unique_ptr<ObjectDirectory>(
      new ObjectDirectory(path_.AppendASCII(physical), std::move(child)));
  result->fault_ = fault_;
  *out = std::move(result);
  return FileStatus::kOk;
}

FileStatus ObjectDirectory::Fault(PublishFence fence,
                                   const base::FilePath& path) const {
  return fault_ ? fault_(fence, path) : FileStatus::kOk;
}
void ObjectDirectory::SetFaultForTesting(FileFault fault) {
  fault_ = std::move(fault);
}

FileStatus ObjectDirectory::Seal(
    ObjectType type,
    const SecretKey& key,
    base::span<const uint8_t> binding,
    uint64_t index,
    base::span<const uint8_t, kRecordChunkSize> plaintext,
    ObjectRef* out) {
  if (binding.size() > kRecordChunkSize || type == ObjectType::kRoot) {
    return FileStatus::kInvalid;
  }
  ObjectRef ref;
  auto status = RandomId(&ref.id);
  if (status != FileStatus::kOk) {
    return status;
  }
  // O_EXCL is also a collision detector: never reuse an extent on retry.
  const auto name = ObjectName(ref.id);
  base::File file(HANDLE_EINTR(openat(directory_.GetPlatformFile(), name.c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                                        O_CLOEXEC,
                                    0600)));
  if (!file.IsValid()) {
    return FileStatus::kIo;
  }
  const auto extent_key = ExtentKey(key, ref.id, type);
  const auto ciphertext = crypto::aead::Seal(
      crypto::aead::AES_256_GCM_SIV, extent_key.bytes(), plaintext,
      Nonce(type, index), AdditionalData(type, binding, ref.id, index));
  const auto physical = path_.AppendASCII(name);
  if (ciphertext.size() != kCipherSize ||
      !file.WriteAndCheck(0, ciphertext)) {
    return FileStatus::kIo;
  }
  status = Fault(PublishFence::kObjectWritten, physical);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (!file.Flush()) {
    return FileStatus::kIo;
  }
  status = Fault(PublishFence::kObjectSynced, physical);
  if (status == FileStatus::kOk) {
    ref.digest = Digest(ciphertext);
    *out = ref;
  }
  return status;
}

FileStatus ObjectDirectory::ReadPhysical(const std::string& name,
                                          size_t maximum,
                                          std::vector<uint8_t>* out) const {
  base::File file(HANDLE_EINTR(openat(directory_.GetPlatformFile(), name.c_str(),
                                    O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
                                        O_CLOEXEC)));
  if (!file.IsValid()) {
    return FileStatus::kIntegrity;
  }
  // The physical layer admits regular ciphertext objects only. O_NONBLOCK
  // prevents a substituted FIFO/device from blocking before we can inspect it;
  // O_NOFOLLOW alone only excludes symlinks.
  struct stat st;
  if (fstat(file.GetPlatformFile(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return FileStatus::kIntegrity;
  }
  const auto length = file.GetLength();
  if (length < 0 || static_cast<uint64_t>(length) > maximum) {
    return FileStatus::kIntegrity;
  }
  out->resize(length);
  return file.ReadAndCheck(0, *out) ? FileStatus::kOk
                                  : FileStatus::kIntegrity;
}

FileStatus ObjectDirectory::Read(ObjectType type,
                                  const SecretKey& key,
                                  base::span<const uint8_t> binding,
                                  uint64_t index,
                                  const ObjectRef& ref,
                                  ObjectPage* out) const {
  if (type == ObjectType::kRoot || ref.empty() ||
      binding.size() > kRecordChunkSize) {
    return FileStatus::kInvalid;
  }
  std::vector<uint8_t> ciphertext;
  auto status = ReadPhysical(ObjectName(ref.id), kCipherSize, &ciphertext);
  if (status != FileStatus::kOk || ciphertext.size() != kCipherSize ||
      Digest(ciphertext) != ref.digest) {
    return FileStatus::kIntegrity;
  }
  const auto extent_key = ExtentKey(key, ref.id, type);
  auto plaintext = crypto::aead::Open(
      crypto::aead::AES_256_GCM_SIV, extent_key.bytes(), ciphertext,
      Nonce(type, index), AdditionalData(type, binding, ref.id, index));
  if (!plaintext || plaintext->size() != kRecordChunkSize) {
    return FileStatus::kIntegrity;
  }
  base::span(*out).copy_from(*plaintext);
  Cleanse(*plaintext);
  return FileStatus::kOk;
}

FileStatus ObjectDirectory::Sync() {
  if (!directory_.Flush()) {
    return FileStatus::kIo;
  }
  return Fault(PublishFence::kObjectsDirectorySynced, path_);
}

FileStatus ObjectDirectory::ReadBootstrap(std::vector<uint8_t>* out) const {
  std::vector<uint8_t> bytes;
  const auto status =
      ReadPhysical("root", kRootPrefix + kMaxBootstrap + kCipherSize, &bytes);
  ObjectId id;
  base::span<const uint8_t> ciphertext;
  return status == FileStatus::kOk ? ParseRoot(bytes, &id, out, &ciphertext)
                                   : status;
}

FileStatus ObjectDirectory::ReadRoot(const SecretKey& key,
                                      ObjectPage* page,
                                      std::vector<uint8_t>* bootstrap) const {
  std::vector<uint8_t> bytes;
  auto status =
      ReadPhysical("root", kRootPrefix + kMaxBootstrap + kCipherSize, &bytes);
  if (status != FileStatus::kOk) {
    return status;
  }
  ObjectId id;
  base::span<const uint8_t> ciphertext;
  status = ParseRoot(bytes, &id, bootstrap, &ciphertext);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto extent_key = ExtentKey(key, id, ObjectType::kRoot);
  auto plaintext = crypto::aead::Open(
      crypto::aead::AES_256_GCM_SIV, extent_key.bytes(), ciphertext,
      Nonce(ObjectType::kRoot, 0),
      AdditionalData(ObjectType::kRoot, *bootstrap, id, 0));
  if (!plaintext || plaintext->size() != kRecordChunkSize) {
    return FileStatus::kIntegrity;
  }
  base::span(*page).copy_from(*plaintext);
  Cleanse(*plaintext);
  return FileStatus::kOk;
}

FileStatus ObjectDirectory::PublishRoot(
    const SecretKey& key,
    base::span<const uint8_t> bootstrap,
    base::span<const uint8_t, kRecordChunkSize> page) {
  if (bootstrap.size() > kMaxBootstrap) {
    return FileStatus::kInvalid;
  }
  ObjectId id;
  auto status = RandomId(&id);
  if (status != FileStatus::kOk) {
    return status;
  }
  const auto name = ObjectName(id);
  const auto path = path_.AppendASCII(name);
  base::File file(HANDLE_EINTR(openat(directory_.GetPlatformFile(), name.c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                                        O_CLOEXEC,
                                    0600)));
  if (!file.IsValid()) {
    return FileStatus::kIo;
  }
  const auto extent_key = ExtentKey(key, id, ObjectType::kRoot);
  const auto ciphertext = crypto::aead::Seal(
      crypto::aead::AES_256_GCM_SIV, extent_key.bytes(), page,
      Nonce(ObjectType::kRoot, 0),
      AdditionalData(ObjectType::kRoot, bootstrap, id, 0));
  if (ciphertext.size() != kCipherSize) {
    return FileStatus::kIo;
  }
  std::vector<uint8_t> bytes(kMagic.begin(), kMagic.end());
  bytes.insert(bytes.end(), id.begin(), id.end());
  Append32(&bytes, bootstrap.size());
  bytes.insert(bytes.end(), bootstrap.begin(), bootstrap.end());
  bytes.insert(bytes.end(), ciphertext.begin(), ciphertext.end());
  bool renamed = false;
  status = [&] {
    if (!file.WriteAndCheck(0, bytes)) {
      return FileStatus::kIo;
    }
    auto result = Fault(PublishFence::kRootWritten, path);
    if (result != FileStatus::kOk) {
      return result;
    }
    if (!file.Flush()) {
      return FileStatus::kIo;
    }
    result = Fault(PublishFence::kRootSynced, path);
    if (result != FileStatus::kOk) {
      return result;
    }
    if (renameat(directory_.GetPlatformFile(), name.c_str(),
                 directory_.GetPlatformFile(), "root") != 0) {
      return FileStatus::kIo;
    }
    renamed = true;
    if (Fault(PublishFence::kRootRenamed, path_) != FileStatus::kOk ||
        !directory_.Flush() ||
        Fault(PublishFence::kRootDirectorySynced, path_) != FileStatus::kOk) {
      return FileStatus::kCommitUncertain;
    }
    return FileStatus::kOk;
  }();
  // Failed unpublished envelopes must not accumulate a named recovery history.
  if (!renamed) {
    if (unlinkat(directory_.GetPlatformFile(), name.c_str(), 0) != 0 ||
        !directory_.Flush()) {
      return FileStatus::kIo;
    }
  }
  return status;
}

FileStatus ObjectDirectory::Prune(const std::set<ObjectId>& live) {
  // openat(".") has an independent directory-stream offset; dup() would share
  // offsets and could make a second GC silently skip entries.
  const int copy = HANDLE_EINTR(openat(directory_.GetPlatformFile(), ".",
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (copy < 0) {
    return FileStatus::kIo;
  }
  DIR* stream = fdopendir(copy);
  if (!stream) {
    close(copy);
    return FileStatus::kIo;
  }
  auto status = FileStatus::kOk;
  for (;;) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) {
      if (errno != 0) {
        status = FileStatus::kIo;
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    ObjectId id;
    struct stat st;
    if (!ParseId(name, &id) ||
        fstatat(directory_.GetPlatformFile(), name.c_str(), &st,
                AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) {
      status = FileStatus::kIntegrity;
      break;
    }
    if (!live.contains(id) &&
        unlinkat(directory_.GetPlatformFile(), name.c_str(), 0) != 0) {
      status = FileStatus::kIo;
      break;
    }
  }
  closedir(stream);
  if (status != FileStatus::kOk) {
    return status;
  }
  status = Fault(PublishFence::kObsoleteObjectsUnlinked, path_);
  if (status != FileStatus::kOk) {
    return status;
  }
  if (!directory_.Flush()) {
    return FileStatus::kIo;
  }
  return Fault(PublishFence::kObsoleteDirectorySynced, path_);
}

FileStatus ObjectDirectory::RemoveChild(const ObjectId& name) {
  std::unique_ptr<ObjectDirectory> child;
  auto status = Child(name, false, &child);
  if (status == FileStatus::kNotFound) {
    return FileStatus::kOk;
  }
  if (status != FileStatus::kOk) {
    return status;
  }
  status = child->Prune({});
  if (status != FileStatus::kOk) {
    return status;
  }
  child.reset();
  const auto physical = path_.AppendASCII(ObjectName(name));
  if (unlinkat(directory_.GetPlatformFile(), ObjectName(name).c_str(),
               AT_REMOVEDIR) != 0) {
    return FileStatus::kIo;
  }
  status = Fault(PublishFence::kRecordDirectoryUnlinked, physical);
  if (status != FileStatus::kOk || !directory_.Flush()) {
    return status == FileStatus::kOk ? FileStatus::kIo : status;
  }
  return Fault(PublishFence::kRecordDirectorySynced, physical);
}

FileStatus ObjectDirectory::CleanupRoot(
    const std::set<ObjectId>& live_directories) {
  const int copy = HANDLE_EINTR(openat(directory_.GetPlatformFile(), ".",
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (copy < 0) {
    return FileStatus::kIo;
  }
  DIR* stream = fdopendir(copy);
  if (!stream) {
    close(copy);
    return FileStatus::kIo;
  }
  auto status = FileStatus::kOk;
  std::vector<ObjectId> obsolete;
  for (;;) {
    errno = 0;
    const auto* entry = readdir(stream);
    if (!entry) {
      if (errno) {
        status = FileStatus::kIo;
      }
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == ".." || name == "root") {
      continue;
    }
    ObjectId id;
    struct stat st;
    if (!ParseId(name, &id) ||
        fstatat(directory_.GetPlatformFile(), name.c_str(), &st,
                AT_SYMLINK_NOFOLLOW) != 0) {
      status = FileStatus::kIntegrity;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!live_directories.contains(id)) {
        obsolete.push_back(id);
      }
    } else if (S_ISREG(st.st_mode) && !live_directories.contains(id)) {
      if (unlinkat(directory_.GetPlatformFile(), name.c_str(), 0) != 0) {
        status = FileStatus::kIo;
        break;
      }
    } else {
      status = FileStatus::kIntegrity;
      break;
    }
  }
  closedir(stream);
  if (status != FileStatus::kOk) {
    return status;
  }
  for (const auto& id : obsolete) {
    status = RemoveChild(id);
    if (status != FileStatus::kOk) {
      return status;
    }
  }
  return directory_.Flush() ? FileStatus::kOk : FileStatus::kIo;
}

}  // namespace alloy::site_records
