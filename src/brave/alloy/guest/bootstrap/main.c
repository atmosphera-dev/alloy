// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Statically linked Linux entry. No libcef, C++ runtime, toolkit or plugins.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "base/process/alloy_guest_runtime.h"
#include "brave/alloy/guest/linux_lock/protection.h"
#include "brave/alloy/guest/namespace_lock/flatpak_entry.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/memfd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

int alloy_guest_supervise(const char* bootstrap);
int alloy_guest_wait_owner(const char* bootstrap);

static int teardown_probe(void) {
  // Trusted static setup only: no registration, writer setup or engine load.
  // EOF is a cooperative escape solely for this disposable pre-admission probe.
  if (send(ALLOY_GUEST_CONTROL_FD, "P", 1, MSG_NOSIGNAL) != 1) return 125;
  char byte;
  ssize_t received;
  do { received = recv(ALLOY_GUEST_CONTROL_FD, &byte, 1, 0); }
  while (received < 0 && errno == EINTR);
  return received == 0 ? 0 : 125;
}

static int granted(int fd, int supervisor, int child, int wait_owner) {
  if (wait_owner && fd == ALLOY_GUEST_WAIT_STATUS_FD) return 1;
  if (child && alloy_flatpak_preserves(fd))
    return 1;
  if (alloy_lock_preserves(fd))
    return 1;
  if (fd == ALLOY_GUEST_REGISTRATION_FD)
    return !supervisor;
  if (fd == ALLOY_GUEST_CONTROL_FD)
    return !child;
  if (fd == ALLOY_GUEST_RESOURCE_FD)
    return !child;
  if (!child)
    return 0;
  // This is a local launch manifest of capabilities already remapped by
  // Chromium, not serialized process/resource identity in the session protocol.
  const char* list = getenv("ALLOY_GUEST_PRESERVE_FDS");
  while (list && *list) {
    char* end;
    errno = 0;
    long number = strtol(list, &end, 10);
    if (errno || end == list || number < 0 || number > INT_MAX ||
        (*end && *end != ','))
      return 0;
    if (number == fd)
      return 1;
    list = *end ? end + 1 : end;
  }
  return 0;
}

static int close_ungranted(int supervisor, int child, int wait_owner) {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory)
    return 0;
  int directory_fd = dirfd(directory);
  struct dirent* entry;
  int valid = 1;
  for (;;) {
    errno = 0;
    entry = readdir(directory);
    if (!entry) {
      valid = errno == 0;
      break;
    }
    char* end;
    errno = 0;
    long number = strtol(entry->d_name, &end, 10);
    if (errno || entry->d_name == end || *end || number < 3 ||
        number > INT_MAX || number == directory_fd)
      continue;
    if (!granted((int)number, supervisor, child, wait_owner) && close((int)number)) {
      valid = 0;
      break;
    }
  }
  return closedir(directory) == 0 && valid;
}

static int sanitize_environment(void) {
  const char* names[] = {"PATH", "LANG", "HOME", "TMPDIR", "XDG_CONFIG_HOME",
      "XDG_CACHE_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
      "ALLOY_GUEST_RUNTIME", "ALLOY_GUEST_BOOTSTRAP", "GSETTINGS_BACKEND",
      "GIO_USE_VFS", "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS", "NO_AT_BRIDGE",
       "MESA_SHADER_CACHE_DISABLE", "__GL_SHADER_DISK_CACHE",
      "ALLOY_GUEST_SCRATCH_FD", "ALLOY_GUEST_LOCK_ABI", "ALLOY_GUEST_CEF_ROOT",
      "FONTCONFIG_FILE", "ALLOY_GUEST_PRESERVE_FDS",
      // Chromium's namespace_sandbox.cc sets these on its child launch. They
      // preserve its native boot/sandbox handshake, not app-supplied authority.
      // The initial app -> supervisor allowlist never copies them from the app.
       "SBX_USER_NS", "SBX_PID_NS", "SBX_NET_NS", "SBX_D", "SBX_HELPER_PID",
       "SBX_CHROME_API_PRV"};
  char* values[sizeof(names) / sizeof(names[0])];
  memset(values, 0, sizeof(values));
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    const char* value = getenv(names[i]);
    if (value && !(values[i] = strdup(value)))
      return 0;
  }
  if (clearenv())
    return 0;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (values[i]) {
      if (setenv(names[i], values[i], 1))
        return 0;
      free(values[i]);
    }
  }
  return 1;
}

int main(int argc, char** argv) {
  unsigned int inherited_nnp = (unsigned int)prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
  int gated = argc == 3 && strcmp(argv[1], "--alloy-supervisor") == 0 &&
              strcmp(argv[2], ALLOY_GUEST_WAIT_GATE_ARGUMENT) == 0;
  int supervisor = gated || (argc == 2 && strcmp(argv[1], "--alloy-supervisor") == 0);
  int probe = argc == 2 && strcmp(argv[1], ALLOY_GUEST_TEARDOWN_PROBE_ARGUMENT) == 0;
  int wait_owner = argc == 2 && strcmp(argv[1], ALLOY_GUEST_WAIT_OWNER_ARGUMENT) == 0;
  int child = 0;
  for (int i = 1; i < argc; ++i) {
    if (strncmp(argv[i], "--type=", 7) == 0)
      child = 1;
    // Public debugging and sandbox-disable flags are never a guest fallback.
    if (strncmp(argv[i], "--remote-debugging", 18) == 0 ||
        strcmp(argv[i], "--no-sandbox") == 0 ||
        strcmp(argv[i], "--disable-gpu-sandbox") == 0)
      return 125;
  }
  char executable[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length <= 0 || (size_t)length >= sizeof(executable) - 1)
    return 125;
  executable[length] = 0;
  int flatpak = probe || wait_owner ? 0 : alloy_flatpak_detect(executable);
  if (flatpak < 0)
    return 125;
  struct rlimit no_core = {0, 0};
  if (setrlimit(RLIMIT_CORE, &no_core) ||
      !close_ungranted(supervisor || probe || wait_owner, child, wait_owner || gated) ||
      !sanitize_environment())
    return 125;
  int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null_fd < 0)
    return 125;
  for (int i = 0; i < 3; ++i) {
    if (dup2(null_fd, i) < 0)
      return 125;
  }
  if (null_fd > 2)
    close(null_fd);
  if (probe)
    return teardown_probe();
  if (wait_owner)
    return alloy_guest_wait_owner(executable);
  if (gated) {
    char grant;
    ssize_t received;
    do { received = recv(ALLOY_GUEST_WAIT_STATUS_FD, &grant, 1, MSG_TRUNC); }
    while (received < 0 && errno == EINTR);
    close(ALLOY_GUEST_WAIT_STATUS_FD);
    if (received != 1 || grant != 'G') return received == 0 ? 0 : 125;
  }
  if (supervisor)
    return alloy_guest_supervise(executable);
  if (!alloy_guest_is_runtime() || !alloy_guest_register_runtime(1))
    return 125;
  // This is supplementary to the liveness channel, never its replacement.
  // A Flatpak broker's child has namespace init as its parent even while the
  // acknowledged Alloy supervisor is live. Check that actual capability.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) || !alloy_guest_supervisor_alive())
    return 125;
  // Ordinary entries inherit both the kernel domain and managed writer paths.
  // Only initial/external entries borrow the supervisor's RAM setup descriptor.
  // Landlock itself pins its grant; keeping an open directory in the engine
  // would bypass Chromium's chroot and fail its mandatory open-directory check.
  int needs_setup = !getenv("ALLOY_GUEST_LOCK_ABI");
  if ((needs_setup && (!alloy_lock_writer_paths() || !alloy_lock_enforce())) ||
      !alloy_lock_drop_setup_capability() ||
      !alloy_flatpak_engine_environment(executable, child))
    return 125;
  struct alloy_guest_bootstrap_facts facts = {
      ALLOY_GUEST_FACTS_MAGIC, getpid(), 1, 1,
       inherited_nnp, !child, flatpak != 0};
  int facts_fd = (int)syscall(SYS_memfd_create, "alloy-bootstrap", MFD_ALLOW_SEALING);
  if (facts_fd < 0 || write(facts_fd, &facts, sizeof(facts)) != sizeof(facts) ||
      fcntl(facts_fd, F_ADD_SEALS,
            F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK) ||
      (facts_fd != ALLOY_GUEST_FACTS_FD &&
       dup2(facts_fd, ALLOY_GUEST_FACTS_FD) < 0))
    return 125;
  if (facts_fd != ALLOY_GUEST_FACTS_FD)
    close(facts_fd);
  // The private endpoint must survive this one fixed exec. The CEF runtime
  // arms it again before entering a role and marks it close-on-exec afterwards.
  if (fcntl(ALLOY_GUEST_LIVENESS_FD, F_SETFD, 0))
    return 125;
  char* separator = strrchr(executable, '/');
  if (!separator || (size_t)(separator - executable) + 20 >= sizeof(executable))
    return 125;
  strcpy(separator + 1, "alloy_guest_engine");
  argv[0] = executable;
  execv(executable, argv);
  return 125;
}
