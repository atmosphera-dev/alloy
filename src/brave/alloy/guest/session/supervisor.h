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
bool LaunchSupervisor(Supervisor* child);
// Close control, then reap the exact direct child. Never terminate by a numeric
// PID received from a message or a process group that could contain app work.
void ReapSupervisor(Supervisor* child);
}  // namespace alloy::guest
#endif
