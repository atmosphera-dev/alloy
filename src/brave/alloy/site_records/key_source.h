// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_KEY_SOURCE_H_
#define BRAVE_ALLOY_SITE_RECORDS_KEY_SOURCE_H_

#include <vector>

#include "brave/alloy/site_records/authenticated_objects.h"

namespace alloy::site_records {

enum class KeySourceKind : uint32_t { kPassword = 1, kSecretService = 2, kExternal = 3 };

// Public bootstrap material, not private profile metadata. Format 1 fixes the
// Argon2id v1.3 parameters at 64 MiB / 3 passes / 4 lanes. Untrusted envelopes
// cannot select an arbitrarily expensive or weaker KDF. Other sources use the
// same random 256-bit salt as their opaque lookup/derivation input.
struct KeySourceParameters {
  KeySourceKind kind = KeySourceKind::kPassword;
  ObjectId salt{};
};

class KeySource {
 public:
  virtual ~KeySource() = default;
  virtual KeySourceKind kind() const = 0;
  // Called outside the profile lifecycle mutex. The output is an operation-
  // scoped KEK; neither the profile nor this interface retains a KEK on disk.
  virtual FileStatus Derive(const KeySourceParameters& parameters,
                            SecretKey* key) = 0;
};

class PasswordKeySource final : public KeySource {
 public:
  explicit PasswordKeySource(base::span<const uint8_t> password);
  ~PasswordKeySource() override;
  KeySourceKind kind() const override;
  FileStatus Derive(const KeySourceParameters& parameters,
                    SecretKey* key) override;
  bool PasswordClearedForTesting() const;

 private:
  std::vector<uint8_t> password_;
  bool consumed_ = false;
};

// Canonical versioned source descriptor. Decode validates before any KDF or
// provider callback. The descriptor is also bound into the key envelope AAD.
std::vector<uint8_t> EncodeKeySource(const KeySourceParameters& parameters);
FileStatus DecodeKeySource(base::span<const uint8_t> encoded,
                            KeySourceParameters* parameters);

}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_KEY_SOURCE_H_
