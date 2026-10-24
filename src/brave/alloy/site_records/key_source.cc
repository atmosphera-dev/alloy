// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/key_source.h"

#include <algorithm>
#include <cstdlib>
#include <limits>

#include "argon2.h"
#include "third_party/boringssl/src/include/openssl/crypto.h"

namespace alloy::site_records {
namespace {
constexpr uint32_t kMemoryKiB = 65536;
constexpr uint32_t kPasses = 3;
constexpr uint32_t kLanes = 4;

void Append(std::vector<uint8_t>* bytes, uint32_t value) {
  for (int shift : {24, 16, 8, 0}) {
    bytes->push_back(value >> shift);
  }
}

// Argon2's public allocator protocol supplies the exact allocation length to
// free. Always clear that arena independently of the library's global flag.
int Allocate(uint8_t** memory, size_t size) {
  *memory = static_cast<uint8_t*>(std::malloc(size));
  return *memory ? 0 : -1;
}
void Free(uint8_t* memory, size_t size) {
  OPENSSL_cleanse(memory, size);
  std::free(memory);
}
}  // namespace

PasswordKeySource::PasswordKeySource(base::span<const uint8_t> password)
    : password_(password.begin(), password.end()) {}
PasswordKeySource::~PasswordKeySource() { Cleanse(password_); }
KeySourceKind PasswordKeySource::kind() const { return KeySourceKind::kPassword; }

FileStatus PasswordKeySource::Derive(const KeySourceParameters& parameters,
                                      SecretKey* key) {
  key->Clear();
  if (consumed_ || parameters.kind != kind() ||
      password_.size() > std::numeric_limits<uint32_t>::max()) {
    Cleanse(password_);
    consumed_ = true;
    return FileStatus::kDenied;
  }
  consumed_ = true;
  ObjectId output{};
  auto salt = parameters.salt;
  argon2_context context{};
  context.out = output.data();
  context.outlen = output.size();
  context.pwd = password_.data();
  context.pwdlen = password_.size();
  context.salt = salt.data();
  context.saltlen = salt.size();
  context.t_cost = kPasses;
  context.m_cost = kMemoryKiB;
  context.lanes = kLanes;
  context.threads = 1;  // The pinned reference target is ARGON2_NO_THREADS.
  context.version = ARGON2_VERSION_13;
  context.allocate_cbk = Allocate;
  context.free_cbk = Free;
  context.flags = ARGON2_FLAG_CLEAR_PASSWORD;
  const int result = argon2id_ctx(&context);
  Cleanse(password_);  // Includes allocation/validation failure paths.
  if (result == ARGON2_OK) {
    *key = SecretKey(output);
  }
  Cleanse(output);
  return result == ARGON2_OK ? FileStatus::kOk : FileStatus::kIo;
}

bool PasswordKeySource::PasswordClearedForTesting() const {
  return consumed_ && std::ranges::all_of(password_, [](uint8_t b) { return b == 0; });
}

std::vector<uint8_t> EncodeKeySource(const KeySourceParameters& parameters) {
  std::vector<uint8_t> encoded;
  Append(&encoded, 1);
  Append(&encoded, static_cast<uint32_t>(parameters.kind));
  const bool password = parameters.kind == KeySourceKind::kPassword;
  Append(&encoded, password ? ARGON2_VERSION_13 : 0);
  Append(&encoded, password ? kMemoryKiB : 0);
  Append(&encoded, password ? kPasses : 0);
  Append(&encoded, password ? kLanes : 0);
  encoded.insert(encoded.end(), parameters.salt.begin(), parameters.salt.end());
  return encoded;
}

FileStatus DecodeKeySource(base::span<const uint8_t> encoded,
                            KeySourceParameters* parameters) {
  if (encoded.size() != 56) {
    return FileStatus::kIntegrity;
  }
  uint32_t kind = 0;
  for (uint8_t b : encoded.subspan<4, 4>()) {
    kind = (kind << 8) | b;
  }
  if (kind < 1 || kind > 3) {
    return FileStatus::kIntegrity;
  }
  parameters->kind = static_cast<KeySourceKind>(kind);
  base::span(parameters->salt).copy_from(encoded.last<32>());
  // All parameters, including the version and reserved fields, must match the
  // selected format. Validate a cleartext descriptor before expensive work.
  return std::ranges::equal(encoded, EncodeKeySource(*parameters))
             ? FileStatus::kOk : FileStatus::kIntegrity;
}
}  // namespace alloy::site_records
