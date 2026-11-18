// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_SUPERVISOR_H_
#define BRAVE_ALLOY_GUEST_SESSION_SUPERVISOR_H_
#include <string>

namespace alloy::guest {
struct Supervisor {
  int process = -1;
  int pidfd = -1;
  int channel = -1;
  // Inherited private resource lane, bound to the same session. M3 owns its
  // capability transport; it never creates a second lifecycle/control session.
  int resources = -1;
};

// Resolve only the bootstrap bundled beside this library, never a caller path.
std::string BundledBootstrap();
// A denied mandatory teardown operation is reported before spawning anything.
// The diagnostic describes the actual operation, not an environment marker.
bool LaunchSupervisor(Supervisor* child, std::string* unavailable);
// Close control, then reap the exact direct child. Never terminate by a numeric
// PID received from a message or a process group that could contain app work.
// Return the original direct child's wait status (zero if none was launched),
// or -1 on a reaping error. A failed teardown must not become a successful close.
int ReapSupervisor(Supervisor* child);
}  // namespace alloy::guest
#endif
