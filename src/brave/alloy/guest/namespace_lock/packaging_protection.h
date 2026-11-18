// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_NAMESPACE_LOCK_PACKAGING_PROTECTION_H_
#define BRAVE_ALLOY_GUEST_NAMESPACE_LOCK_PACKAGING_PROTECTION_H_
#include "brave/alloy/guest/linux_lock/protection.h"
namespace alloy::guest {
// Same report fields as the native backend; no new public protection API.
std::optional<std::array<std::string, 5>> PackagingProtectionFields(
    int abi, const LinuxSandboxFacts& sandbox, bool flatpak_broker);
}
#endif
