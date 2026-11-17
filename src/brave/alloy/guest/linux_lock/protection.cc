// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/linux_lock/protection.h"

#include <cerrno>
#include <fcntl.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace alloy::guest {
std::string LinuxUserNamespaceUnavailableReason() {
  // Match NamespaceSandbox::LaunchProcessWithOptions on this pin: USER is
  // mandatory, while PID/NET are selected when their proc namespace entries
  // exist. Do not invent a stricter kernel configuration requirement.
  if (access("/proc/self/ns/user", F_OK))
    return "userns-unavailable:kernel-user-namespace-entry-unavailable";
  int namespace_types = 0;
  if (!access("/proc/self/ns/pid", F_OK)) namespace_types |= CLONE_NEWPID;
  if (!access("/proc/self/ns/net", F_OK)) namespace_types |= CLONE_NEWNET;
  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC))
    return "userns-probe-pipe-errno-" + std::to_string(errno);
  pid_t child = fork();
  if (!child) {
    close(descriptors[0]);
    // Ubuntu can allow NEWUSER while transitioning the child into AppArmor's
    // unprivileged_userns profile, which then denies namespace CAP_SYS_ADMIN.
    // Probe the PID/network operations the native route actually needs too;
    // accepting a syscall's existence alone is not sandbox availability.
    int result[2] = {unshare(CLONE_NEWUSER) ? errno : 0, 1};
    if (!result[0]) {
      result[1] = 2;
      if (namespace_types && unshare(namespace_types)) result[0] = errno;
    }
    ssize_t sent;
    do { sent = write(descriptors[1], result, sizeof(result)); } while (sent < 0 && errno == EINTR);
    _exit(sent == sizeof(result) ? 0 : 1);
  }
  close(descriptors[1]);
  int result[2] = {0, 0};
  ssize_t received;
  do { received = read(descriptors[0], result, sizeof(result)); } while (received < 0 && errno == EINTR);
  close(descriptors[0]);
  int status = 0;
  pid_t reaped = -1;
  if (child > 0) {
    do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
  }
  if (child < 0 || reaped != child || !WIFEXITED(status) || WEXITSTATUS(status) || received != sizeof(result))
    return "userns-probe-failed";
  int error = result[0];
  if (!error) return {};
  std::string reason = "userns-unavailable:unshare-errno-" + std::to_string(error);
  const char* operation = "user";
  if (result[1] == 2) {
    operation = namespace_types == (CLONE_NEWPID | CLONE_NEWNET) ? "pid-and-network"
        : (namespace_types & CLONE_NEWPID) ? "pid" : "network";
  }
  reason += ":operation=" + std::string(operation);
  if (error == EACCES) reason += ":LSM-access-denied";
  else if (error == EPERM) reason += ":kernel-or-security-policy-denied";
  else if (error == EINVAL || error == ENOSYS) reason += ":required-namespace-unsupported";
  else if (error == ENOSPC || error == EUSERS) reason += ":namespace-limit-reached";
  // Observed settings disambiguate policy/limits without claiming an errno
  // uniquely identifies AppArmor, CONFIG_USER_NS or any one kernel control.
  for (const char* path : {"/proc/sys/kernel/unprivileged_userns_clone",
                          "/proc/sys/user/max_user_namespaces",
                          "/proc/sys/kernel/apparmor_restrict_unprivileged_userns"}) {
    std::string value;
    if (base::ReadFileToString(base::FilePath(path), &value))
      reason += ";" + std::string(path) + "=" + std::string(base::TrimWhitespaceASCII(value, base::TRIM_ALL));
  }
  return reason;
}

std::optional<std::array<std::string, 5>> LinuxProtectionFields(
    int abi, const LinuxSandboxFacts& sandbox) {
  if (abi < 3 || !sandbox.observed || !sandbox.browser_nnp ||
      !sandbox.renderer_nnp || sandbox.renderer_seccomp != 2 ||
      sandbox.renderer_capabilities ||
      sandbox.route != "userns")
    return std::nullopt;

  base::DictValue coverage;
  coverage.Set("before_engine", true);
  coverage.Set("browser", "landlock");
  coverage.Set("ordinary_fork_exec", "inherited-landlock");
  coverage.Set("zygote_forks", "inherited-landlock");
  coverage.Set("runtime_supervision", "registered-native-entries-and-pidfds");
  coverage.Set("setuid_entry", "disabled-in-guest-mode");
  coverage.Set("flatpak_external_entries", "unavailable");
  coverage.Set("snap_external_entries", "unavailable");
  base::ListValue trusted;
  trusted.Append("static-unprivileged-supervisor-without-engine-payload");
  coverage.Set("trusted_setup", std::move(trusted));

  base::DictValue native;
  native.Set("route", sandbox.route);
  native.Set("zygote_status_flags", sandbox.native_flags);
  base::DictValue browser;
  browser.Set("no_new_privs", sandbox.browser_nnp);
  browser.Set("seccomp", sandbox.browser_seccomp);
  native.Set("browser", std::move(browser));
  base::DictValue renderer;
  renderer.Set("observed_after_blank_load", sandbox.observed);
  renderer.Set("no_new_privs", sandbox.renderer_nnp);
  renderer.Set("seccomp", sandbox.renderer_seccomp);
  renderer.Set("effective_capabilities_decimal", std::to_string(sandbox.renderer_capabilities));
  native.Set("renderer", std::move(renderer));
  native.Set("other_roles", "upstream-role-specific; no blanket-seccomp-claim");
  native.Set("setuid_helper", "never-used-in-guest-mode");
  native.Set("oom_adjustment", "native-truncating-writes-refused; renderer-scores-inherited");

  base::DictValue limits;
  limits.Set("backend", "landlock");
  limits.Set("abi", abi);
  limits.Set("handled", "write,truncate,create,remove,rename,link; device-node-creation-denied-even-in-RAM");
  limits.Set("procfs_exception", "WRITE_FILE across procfs, not per-PID; no truncate/create/remove grant");
  limits.Set("process_isolation", "not-isolated-from-other-same-user-processes; non-truncating-procfs-writes-can-change-their-oom-score,coredump-filter,clear_refs,sched,autogroup; private-process-view-planned-after-first-release");
  limits.Set("device_exceptions", "/dev/null,/dev/random,/dev/urandom WRITE_FILE");
  limits.Set("metadata_and_atime", "not-refused-by-Landlock");
  limits.Set("preopened_capabilities", "Landlock-does-not-revoke; bootstrap-closes-ungranted-FDs-and-replaces-stdio; exec-discards-old-mappings");
  limits.Set("setup_directory_capabilities", "supervisor-only; dropped-before-engine");
  limits.Set("ipc_device_boundary", "not-OS-enforced; connector-availability-is-separate");
  limits.Set("namespace_filesystem_and_packages", "unavailable-until-real-backend-proofs");
  limits.Set("physical_storage", "RAM-may-swap-or-hibernate; OS-audit/core/external-service-effects-outside-boundary");
  limits.Set("scratch_failure", "no-durable-fallback; loss-or-exhaustion-ends-session");
  limits.Set("joint_app_supervisor_death", "named-RAM-may-remain; never-reused; no-immediate-erasure-claim");
  limits.Set("abrupt_supervisor_death", "named-RAM-may-remain-even-if-app-survives; never-reused; cleanup-is-not-write-prevention");

  auto encoded_coverage = base::WriteJson(coverage);
  auto encoded_native = base::WriteJson(native);
  auto encoded_limits = base::WriteJson(limits);
  if (!encoded_coverage || !encoded_native || !encoded_limits) return std::nullopt;
  return std::array<std::string, 5>{"data-writes-denied", std::move(*encoded_coverage),
      "verified-tmpfs", std::move(*encoded_native), std::move(*encoded_limits)};
}
}  // namespace alloy::guest
