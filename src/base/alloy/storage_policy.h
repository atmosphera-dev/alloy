// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BASE_ALLOY_STORAGE_POLICY_H_
#define BASE_ALLOY_STORAGE_POLICY_H_

#include "base/base_export.h"

namespace base::alloy {

// Engine bootstrap policy, established before Chrome/Brave startup can open
// installation state. Child processes receive the same immutable restriction.
// Request-context storage admission remains the capability owner's concern;
// this flag never grants an individual context permission to persist anything.
inline constexpr char kNoDiskSwitch[] = "alloy-no-disk";

// Called by the native process bootstrap, before other owners initialize.
// Repeated initialization must agree; a mutable command line can never remove
// the restriction after bootstrap. The switch is transport to child bootstrap.
BASE_EXPORT void InitializeStoragePolicy(bool no_disk);
BASE_EXPORT bool IsNoDiskProcess();

}  // namespace base::alloy

#endif  // BASE_ALLOY_STORAGE_POLICY_H_
