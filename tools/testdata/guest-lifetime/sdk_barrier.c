// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// App-only test interposition. Production launch sanitization does not pass
// LD_PRELOAD or these fixture inputs into any guest process.
#define _GNU_SOURCE
#include "base/process/alloy_guest_runtime.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

static atomic_int intercepted;

static int stopped_probe(struct msghdr* message, char* observed_state) {
  struct cmsghdr* header = CMSG_FIRSTHDR(message);
  if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
      header->cmsg_len != CMSG_LEN(sizeof(int))) return 0;
  int handle;
  memcpy(&handle, CMSG_DATA(header), sizeof(handle));
  char path[128], line[512];
  snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", handle);
  FILE* input = fopen(path, "r");
  if (!input) return 0;
  int pid = -1;
  while (fgets(line, sizeof(line), input)) {
    if (sscanf(line, "Pid:\t%d", &pid) == 1) break;
  }
  fclose(input);
  if (pid <= 1 || syscall(SYS_pidfd_send_signal, handle, SIGSTOP, NULL, 0)) return 0;
  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  char last_state = '?';
  // Bounded fixture observation only, never a product retirement deadline.
  for (int attempt = 0; attempt < 1000; ++attempt) {
    input = fopen(path, "r");
    if (!input) return 0;
    int stopped = 0;
    while (fgets(line, sizeof(line), input)) {
      if (!strncmp(line, "State:\t", 7)) last_state = line[7];
      if (last_state == 'T' || last_state == 't') { stopped = 1; break; }
    }
    fclose(input);
    if (stopped) { *observed_state = last_state; return pid; }
    poll(NULL, 0, 10);
  }
  fprintf(stderr, "SDK_PROBE_STOP_OBSERVATION pid=%d state=%c\n", pid, last_state);
  return 0;
}

ssize_t recvmsg(int fd, struct msghdr* message, int flags) {
  ssize_t (*receive)(int, struct msghdr*, int) = dlsym(RTLD_NEXT, "recvmsg");
  if (!receive) { errno = ENOSYS; return -1; }
  ssize_t count = receive(fd, message, flags);
  const char* mode = getenv("ALLOY_FIXTURE_CUSTODY_BARRIER");
  const char* path = getenv("ALLOY_FIXTURE_CUSTODY_GRANT");
  if (!mode || !path || count != sizeof(struct alloy_guest_wait_status) ||
      message->msg_iovlen != 1 || message->msg_iov[0].iov_len < sizeof(struct alloy_guest_wait_status))
    return count;
  struct alloy_guest_wait_status packet;
  memcpy(&packet, message->msg_iov[0].iov_base, sizeof(packet));
  int probe = !strcmp(mode, "probe");
  if (packet.magic != ALLOY_GUEST_WAIT_STATUS_MAGIC || packet.error ||
      packet.phase != (probe ? ALLOY_GUEST_WAIT_PROBE : ALLOY_GUEST_WAIT_READY) ||
      atomic_exchange(&intercepted, 1)) return count;
  char observed_state = '?';
  int probe_pid = probe ? stopped_probe(message, &observed_state) : 0;
  if (probe && !probe_pid) { errno = EIO; return -1; }
  char marker[256];
  if (probe)
    snprintf(marker, sizeof(marker), "GUEST_EVENT {\"name\":\"driver\",\"event\":\"probe-stopped\",\"pid\":%d,\"state\":\"%c\"}\n",
             probe_pid, observed_state);
  else
    snprintf(marker, sizeof(marker), "GUEST_EVENT {\"name\":\"driver\",\"event\":\"ready-barrier\"}\n");
  size_t length = strlen(marker);
  ssize_t sent;
  do { sent = write(STDOUT_FILENO, marker, length); } while (sent < 0 && errno == EINTR);
  if (sent != (ssize_t)length) { errno = EIO; return -1; }
  int grant;
  do { grant = open(path, O_RDONLY | O_CLOEXEC); } while (grant < 0 && errno == EINTR);
  if (grant < 0) return -1;
  char byte = 0;
  ssize_t received;
  do { received = read(grant, &byte, 1); } while (received < 0 && errno == EINTR);
  close(grant);
  if (received != 1 || byte != 'G') { errno = EPROTO; return -1; }
  return count;
}
