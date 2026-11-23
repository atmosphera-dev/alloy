// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_SUPERVISOR_H_
#define BRAVE_ALLOY_GUEST_SESSION_SUPERVISOR_H_
#include <atomic>
#include <functional>
#include <string>

namespace alloy::guest {
enum class LaunchOutcome { kReady, kCancelled, kOwnerLost, kUnavailable, kFailed };
enum class CustodyInstall { kInstalled, kCancelled, kFailed };

// One move-only custody result, including failures after process creation.
// Only its worker mutates these handles; other threads retain private mirrors.
struct Supervisor {
  Supervisor();
  Supervisor(const Supervisor&) = delete;
  Supervisor& operator=(const Supervisor&) = delete;
  ~Supervisor();
  int process = -1;
  int pidfd = -1;
  int channel = -1;
  int resources = -1;
  int status_channel = -1;
  int exec_error = -1;
  int runtime_pidfd = -1;
  int probe_pidfd = -1;
  int mirror_pidfd = -1;
  int mirror_status = -1;
  Supervisor* fork_next = nullptr;
  bool finished = false;
  std::atomic<bool> forced{false};
  std::atomic<bool> cancelled{false};
  int cleanup = 0;
  int receipt_status = -1;
};

std::string BundledBootstrap();
// Serialize descriptor creation/retirement with the embedding app's fork.
class SupervisorForkLock {
 public:
  SupervisorForkLock();
  ~SupervisorForkLock();
};
// install runs immediately after atomic creation, before any grant. It must
// retain the original init capability before another thread can cancel startup.
LaunchOutcome StartSupervisor(Supervisor* child, std::string* unavailable,
                              const std::function<CustodyInstall(Supervisor*)>& install);
bool AdmitSupervisor(int status_channel);
// Standalone internal/test consumer. The SDK uses Start plus its locked Admit.
bool LaunchSupervisor(Supervisor* child);
// forced permits a missing receipt only after a deliberate init SIGKILL and its
// kernel exit barrier. ECHILD is accepted only at the app boundary, never inside
// the static custody tree. Every other reap/poll/receipt error is retained.
int ReapSupervisor(Supervisor* child);
}  // namespace alloy::guest
#endif
