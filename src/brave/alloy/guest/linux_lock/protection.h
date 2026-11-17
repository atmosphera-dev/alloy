// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_LINUX_LOCK_PROTECTION_H_
#define BRAVE_ALLOY_GUEST_LINUX_LOCK_PROTECTION_H_

// C linkage keeps the single-threaded, static pre-engine entry independent of
// Chromium, libcef, toolkit initialization and the C++ runtime.
#ifdef __cplusplus
extern "C" {
#endif
int alloy_lock_prepare_scratch(void);
int alloy_lock_scratch_fd(void);
int alloy_lock_scratch_valid(void);
int alloy_lock_drop_setup_capability(void);
void alloy_lock_remove_scratch(void);
int alloy_lock_writer_paths(void);
int alloy_lock_enforce(void);
int alloy_lock_preserves(int fd);

#ifdef __cplusplus
}
#include <array>
#include <cstdint>
#include <optional>
#include <string>
namespace alloy::guest {
// Empty only when the current execution profile permits the required
// native-selected user/PID/network namespace operations.
// The short-lived child does not initialize Chromium or admit page work.
std::string LinuxUserNamespaceUnavailableReason();
struct LinuxSandboxFacts {
  bool observed = false;
  bool browser_nnp = false;
  int browser_seccomp = 0;
  bool renderer_nnp = false;
  int renderer_seccomp = 0;
  uint64_t renderer_capabilities = 0;
  int native_flags = 0;
  std::string route;
};
// These are report values, not session policy or a second extension dispatcher.
std::optional<std::array<std::string, 5>> LinuxProtectionFields(
    int abi, const LinuxSandboxFacts& sandbox);
}  // namespace alloy::guest
#endif
#endif
