// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/alloy/storage_policy.h"

#include <atomic>

#include "base/check_op.h"

namespace base::alloy {
namespace {
enum class DiskPolicy { kUninitialized, kLegacyDisk, kNoDisk };
constinit std::atomic<DiskPolicy> g_disk_policy{DiskPolicy::kUninitialized};
}  // namespace

void InitializeStoragePolicy(bool no_disk) {
  const auto desired = no_disk ? DiskPolicy::kNoDisk : DiskPolicy::kLegacyDisk;
  auto expected = DiskPolicy::kUninitialized;
  if (!g_disk_policy.compare_exchange_strong(expected, desired)) {
    CHECK_EQ(expected, desired);
  }
}

bool IsNoDiskProcess() {
  return g_disk_policy.load() == DiskPolicy::kNoDisk;
}

bool AllowsGpuDiskCaches() {
  return false;
}

}  // namespace base::alloy
