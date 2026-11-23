// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "brave/alloy/guest/linux_lock/pinned_spawn.h"
#include "brave/alloy/guest/linux_lock/scratch_mount.h"
#include "base/process/alloy_guest_runtime.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/sched.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum setup_operation { RESET_SIGNALS, PARENT_DEATH, PARENT_ALIVE, SETGROUPS_MAP,
                       UID_MAP, GID_MAP, PRIVATE_SESSION,
                       PRIVATE_MOUNTS, PRIVATE_PROC, PRIVATE_SCRATCH, FD_REMAP, SIGNAL_MASK, FIXED_EXEC };

static void child_error(int fd, int error, enum setup_operation operation) {
  const int packet[] = {error, operation};
  ssize_t written;
  do { written = write(fd, packet, sizeof(packet)); } while (written < 0 && errno == EINTR);
  (void)written;
  _exit(125);
}

static int write_map(const char* path, const char* text) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return errno;
  size_t count = strlen(text);
  ssize_t written;
  do { written = write(fd, text, count); } while (written < 0 && errno == EINTR);
  int error = written == (ssize_t)count ? 0 : written < 0 ? errno : EIO;
  if (close(fd) && !error) error = errno;
  return error;
}

int alloy_spawn_pinned(const char* path, char* const argv[], char* const envp[],
                       const struct alloy_lock_fd_map* maps, size_t count,
                       int enclosure, struct alloy_pinned_child* child) {
  *child = (struct alloy_pinned_child){-1, -1, -1, "spawn descriptor validation"};
  int maximum = ALLOY_GUEST_RESOURCE_FD;
  if (count > SIZE_MAX / sizeof(int)) return EOVERFLOW;
  for (size_t i = 0; i < count; ++i) {
    if (maps[i].target < 0 || maps[i].target == INT_MAX) return EINVAL;
    if (maps[i].target > maximum) maximum = maps[i].target;
  }
  child->operation = "spawn descriptor allocation";
  int* copies = count ? malloc(count * sizeof(int)) : NULL;
  if (count && !copies) return ENOMEM;
  size_t copied = 0;
  int error = 0, errors[2] = {-1, -1}, parent = -1;
  for (; copied < count; ++copied) {
    child->operation = "fcntl(spawn descriptor copy)";
    copies[copied] = fcntl(maps[copied].source, F_DUPFD_CLOEXEC, maximum + 1);
    if (copies[copied] < 0) { error = errno; goto done; }
  }
  child->operation = "pipe2(exec error lane)";
  if (pipe2(errors, O_CLOEXEC)) { error = errno; goto done; }
  // The exec-error writer must not be overwritten by any remap.
  child->operation = "fcntl(exec error writer)";
  int writer = fcntl(errors[1], F_DUPFD_CLOEXEC, maximum + 1);
  if (writer < 0) { error = errno; goto done; }
  close(errors[1]);
  errors[1] = writer;
  char uid_map[96], gid_map[96];
  if (enclosure) {
    snprintf(uid_map, sizeof(uid_map), "%u %u 1\n", (unsigned)getuid(), (unsigned)getuid());
    snprintf(gid_map, sizeof(gid_map), "%u %u 1\n", (unsigned)getgid(), (unsigned)getgid());
    child->operation = "pidfd_open(parent liveness)";
    int original = (int)syscall(SYS_pidfd_open, getpid(), 0);
    if (original < 0) { error = errno; goto done; }
    child->operation = "fcntl(parent liveness copy)";
    parent = fcntl(original, F_DUPFD_CLOEXEC, maximum + 1);
    error = parent < 0 ? errno : 0;
    close(original);
    if (error) goto done;
  }
  sigset_t blocked, original_mask;
  sigfillset(&blocked);
  child->operation = "sigprocmask(spawn signal block)";
  if (sigprocmask(SIG_SETMASK, &blocked, &original_mask)) { error = errno; goto done; }
  int pidfd = -1;
  struct clone_args arguments = {0};
  arguments.flags = CLONE_PIDFD | (enclosure ? CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS : 0);
  arguments.pidfd = (uint64_t)(uintptr_t)&pidfd;
  arguments.exit_signal = SIGCHLD;
  // VER0 works with Linux 5.3; no glibc clone3 wrapper/new libc requirement.
  child->operation = enclosure ? "clone3(CLONE_PIDFD|CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNS)" :
                                 "clone3(CLONE_PIDFD)";
  pid_t process = (pid_t)syscall(SYS_clone3, &arguments, CLONE_ARGS_SIZE_VER0);
  if (!process) {
    // No malloc, C++, pthread atfork callbacks or inherited app handlers here.
    close(errors[0]);
    struct sigaction action = {0};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    for (int number = 1; number < NSIG; ++number) {
      if (number == SIGKILL || number == SIGSTOP || number == 32 || number == 33) continue;
      if (sigaction(number, &action, NULL)) child_error(writer, errno, RESET_SIGNALS);
    }
    if (prctl(PR_SET_PDEATHSIG, SIGKILL)) child_error(writer, errno, PARENT_DEATH);
    if (enclosure) {
      struct pollfd alive = {parent, POLLIN, 0};
      int observed;
      do { observed = poll(&alive, 1, 0); } while (observed < 0 && errno == EINTR);
      if (observed < 0 || alive.revents) child_error(writer, observed < 0 ? errno : ECANCELED, PARENT_ALIVE);
      int setup = write_map("/proc/self/setgroups", "deny\n");
      if (setup) child_error(writer, setup, SETGROUPS_MAP);
      setup = write_map("/proc/self/uid_map", uid_map);
      if (setup) child_error(writer, setup, UID_MAP);
      setup = write_map("/proc/self/gid_map", gid_map);
      if (setup) child_error(writer, setup, GID_MAP);
      // A real CEF run proved that its namespace-local process handles must
      // resolve through the same PID namespace's procfs (including the native
      // renderer sandbox preflight). Configure only that private proc mount,
      // while this new user namespace still has its setup capabilities. The
      // fixed exec drops them; the existing Landlock policy remains unchanged.
      if (syscall(SYS_setsid) < 0) child_error(writer, errno, PRIVATE_SESSION);
      if (syscall(SYS_mount, NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
        child_error(writer, errno, PRIVATE_MOUNTS);
      if (syscall(SYS_mount, "proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL))
        child_error(writer, errno, PRIVATE_PROC);
      if (alloy_lock_mount_scratch()) child_error(writer, errno, PRIVATE_SCRATCH);
      if (dup2(parent, ALLOY_GUEST_PARENT_FD) < 0) child_error(writer, errno, FD_REMAP);
      close(parent);
    }
    for (size_t i = 0; i < count; ++i) {
      if (dup2(copies[i], maps[i].target) < 0) child_error(writer, errno, FD_REMAP);
      close(copies[i]);
    }
    sigemptyset(&blocked);
    if (sigprocmask(SIG_SETMASK, &blocked, NULL)) child_error(writer, errno, SIGNAL_MASK);
    execve(path, argv, envp);
    child_error(writer, errno, FIXED_EXEC);
  }
  error = process < 0 ? errno : 0;
  int restored = sigprocmask(SIG_SETMASK, &original_mask, NULL);
  if (process > 0) {
    // Even a restore error must return custody, never abandon a created child.
    child->process = process;
    child->pidfd = pidfd;
    child->exec_error = errors[0];
    errors[0] = -1;
    if (restored) { error = errno; child->operation = "sigprocmask(parent signal restore)"; }
  }
done:
  for (size_t i = 0; i < copied; ++i) close(copies[i]);
  free(copies);
  if (errors[0] >= 0) close(errors[0]);
  if (errors[1] >= 0) close(errors[1]);
  if (parent >= 0) close(parent);
  return error;
}

int alloy_pinned_exec_result(int error_fd, const char** operation) {
  int packet[2] = {0, 0};
  ssize_t count;
  do { count = read(error_fd, packet, sizeof(packet)); } while (count < 0 && errno == EINTR);
  if (count == 0) return 0;
  static const char* const names[] = {"sigaction reset", "PR_SET_PDEATHSIG", "parent pidfd liveness",
      "PID enclosure /proc/self/setgroups", "PID enclosure /proc/self/uid_map",
      "PID enclosure /proc/self/gid_map", "setsid(private guest session)",
      "mount(MS_PRIVATE)", "mount(private procfs)", "mount(private scratch tmpfs)",
      "descriptor remap", "signal mask reset", "fixed bootstrap exec"};
  if (count != sizeof(packet) || packet[0] <= 0 || packet[1] < 0 || packet[1] > FIXED_EXEC)
    return count < 0 ? errno : EPROTO;
  if (operation) *operation = names[packet[1]];
  return packet[0];
}

int alloy_pinned_signal(int pidfd, int signal_number) {
  int result;
  do { result = (int)syscall(SYS_pidfd_send_signal, pidfd, signal_number, NULL, 0); }
  while (result < 0 && errno == EINTR);
  return result == 0 || errno == ESRCH ? 0 : errno;
}

int alloy_pinned_exited(int pidfd, int blocking) {
  struct pollfd event = {pidfd, POLLIN, 0};
  int result;
  do { result = poll(&event, 1, blocking ? -1 : 0); } while (result < 0 && errno == EINTR);
  if (result < 0) return -errno;
  if (event.revents & (POLLERR | POLLNVAL)) return -EIO;
  return (event.revents & POLLIN) != 0;
}

int alloy_pinned_wait(int pidfd, int* status) {
  siginfo_t info = {0};
  int result;
  do { result = waitid((idtype_t)3, (id_t)pidfd, &info, WEXITED); }
  while (result < 0 && errno == EINTR);
  if (result < 0) return errno;
  if (info.si_code == CLD_EXITED) *status = info.si_status << 8;
  else if (info.si_code == CLD_KILLED || info.si_code == CLD_DUMPED)
    *status = info.si_status | (info.si_code == CLD_DUMPED ? 128 : 0);
  else return EPROTO;
  return 0;
}
