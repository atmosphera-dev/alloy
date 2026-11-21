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
  // Trusted static wait-owner receipts, separate from engine-controlled lanes.
  int status_channel = -1;
  // Retained even if the outer wait owner dies after handing off admission.
  int runtime_pidfd = -1;
};

// Resolve only the bootstrap bundled beside this library, never a caller path.
std::string BundledBootstrap();
// A denied mandatory teardown operation is reported before runtime admission.
// The diagnostic describes the actual operation, not an environment marker.
bool LaunchSupervisor(Supervisor* child, std::string* unavailable, int cancel = -1);
// Close control, then reap the exact direct child. Never terminate by a numeric
// PID received from a message or a process group that could contain app work.
// Return the inner supervisor's kernel wait status from its static wait owner,
// after that owner's original pidfd exits; -1 on missing/invalid receipt. Never
// depend on the embedding app's SIGCHLD disposition or interpret ECHILD as proof.
int ReapSupervisor(Supervisor* child);
}  // namespace alloy::guest
#endif
