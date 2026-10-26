// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_STORAGE_MODE_H_
#define BRAVE_ALLOY_SITE_RECORDS_STORAGE_MODE_H_

#include <compare>

namespace alloy::site_records {

enum class StorageMode { kStateless, kMemory, kEncrypted };
enum class NetworkState { kKeep, kCut };

// Engine-created construction metadata, carried before native owners exist.
// A single value selects admission and working-network lifetime; a nullable
// value in an adapter means ordinary legacy CEF behavior, not an Alloy mode.
class ContextMode {
 public:
  static constexpr ContextMode Stateless(NetworkState network_state) {
    return ContextMode(StorageMode::kStateless, network_state);
  }
  static constexpr ContextMode Memory() {
    return ContextMode(StorageMode::kMemory, NetworkState::kKeep);
  }

  constexpr StorageMode storage_mode() const { return storage_mode_; }
  constexpr NetworkState network_state() const { return network_state_; }
  friend constexpr auto operator<=>(const ContextMode&,
                                    const ContextMode&) = default;

 private:
  constexpr ContextMode(StorageMode storage_mode, NetworkState network_state)
      : storage_mode_(storage_mode), network_state_(network_state) {}

  StorageMode storage_mode_;
  NetworkState network_state_;
};

}  // namespace alloy::site_records

#endif  // BRAVE_ALLOY_SITE_RECORDS_STORAGE_MODE_H_
