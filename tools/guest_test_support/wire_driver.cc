// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Wire-mismatch fixture through production custody, never a bare supervisor.
#include "brave/alloy/guest/session/supervisor.h"
#include "brave/alloy/guest/protocol/wire.h"
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static bool Send(int fd, const char* bytes, size_t size) {
  for (;;) {
    ssize_t count = send(fd, bytes, size, MSG_NOSIGNAL);
    if (count == static_cast<ssize_t>(size)) return true;
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      pollfd event = {fd, POLLOUT, 0};
      int result;
      do { result = poll(&event, 1, -1); } while (result < 0 && errno == EINTR);
      if (result > 0 && (event.revents & POLLOUT)) continue;
    }
    return false;
  }
}

int main(int argc, char** argv) {
  if (argc != 2) return 8;
  alloy::guest::Supervisor child;
  if (!alloy::guest::LaunchSupervisor(&child)) return 2;
  char packet[ALLOY_GUEST_CONTROL_CHUNK_SIZE];
  ssize_t count;
  do { count = recv(STDIN_FILENO, packet, sizeof(packet), MSG_TRUNC); }
  while (count < 0 && errno == EINTR);
  if (count <= 0 || count > static_cast<ssize_t>(sizeof(packet)) ||
      !Send(child.channel, packet, count)) return 3;
  for (;;) {
    pollfd events[] = {{child.channel, POLLIN, 0}, {child.pidfd, POLLIN, 0}};
    int observed;
    do { observed = poll(events, 2, -1); } while (observed < 0 && errno == EINTR);
    if (observed < 0) return 4;
    do { count = recv(child.channel, packet, sizeof(packet), MSG_TRUNC | MSG_DONTWAIT); }
    while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count < 0 || count > static_cast<ssize_t>(sizeof(packet))) return 5;
    if (count && !Send(STDIN_FILENO, packet, count)) return 6;
    if (!count) shutdown(STDIN_FILENO, SHUT_WR);
    break;
  }
  // A malformed envelope is rejected by the static supervisor itself. Its
  // failed cleanup is the expected observation, not a helper launch failure.
  int expected = !strcmp(argv[1], "version") ? -1 : 0;
  return alloy::guest::ReapSupervisor(&child) == expected ? 0 : 7;
}
