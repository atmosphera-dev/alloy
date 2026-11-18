// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "brave/alloy/guest/namespace_lock/flatpak_entry.h"
#include "brave/alloy/guest/linux_lock/protection.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int enabled;

static int sibling(char* path, size_t capacity, const char* bootstrap,
                   const char* leaf) {
  const char* separator = strrchr(bootstrap, '/');
  if (!separator) return 0;
  int length = snprintf(path, capacity, "%.*s/zypak/%s",
                        (int)(separator - bootstrap), bootstrap, leaf);
  return length > 0 && (size_t)length < capacity;
}

int alloy_flatpak_detect(const char* bootstrap) {
  if (access("/.flatpak-info", F_OK)) return errno == ENOENT ? 0 : -1;
  if (strncmp(bootstrap, "/app/", 5)) return -1;
  char path[PATH_MAX];
  const char* programs[] = {"bin/zypak-sandbox", "bin/zypak-helper"};
  const char* libraries[] = {"lib/libzypak-preload-host.so",
      "lib/libzypak-preload-host-spawn-strategy.so",
      "lib/libzypak-preload-host-spawn-strategy-close.so",
      "lib/libzypak-preload-child.so",
      "lib/libzypak-preload-child-spawn-strategy.so"};
  for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i)
    if (!sibling(path, sizeof(path), bootstrap, programs[i]) || access(path, X_OK)) return -1;
  for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i)
    if (!sibling(path, sizeof(path), bootstrap, libraries[i]) || access(path, R_OK)) return -1;
  enabled = 1;
  return 1;
}

int alloy_flatpak_preserves(int fd) {
  if (!enabled) return 0;
  const char* value = getenv("SBX_D");
  if (!value || !*value) return 0;
  char* end;
  errno = 0;
  long parsed = strtol(value, &end, 10);
  if (errno || *end || parsed < 3 || parsed > INT_MAX || parsed != fd) return 0;
  int type = 0;
  socklen_t size = sizeof(type);
  return !getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) && type == SOCK_STREAM;
}

int alloy_flatpak_prepare_scratch(void) {
  if (!enabled) return alloy_lock_prepare_scratch();
  char parent[128];
  // This exact package permission is a host-visible, shared runtime tmpfs. The
  // lock still checks backing and pins a fresh private session directory. No
  // HOME/XDG app data, /var/tmp or disk-possible alternative is accepted.
  int length = snprintf(parent, sizeof(parent), "/run/user/%lu/alloy-guest",
                        (unsigned long)getuid());
  return length > 0 && (size_t)length < sizeof(parent) &&
      alloy_lock_prepare_scratch_in(parent);
}

int alloy_flatpak_engine_environment(const char* bootstrap, int child) {
  if (!enabled) return 1;
  char bin[PATH_MAX], lib[PATH_MAX], path[PATH_MAX + 32], loader[PATH_MAX + 16],
       preload[PATH_MAX * 4];
  if (!sibling(bin, sizeof(bin), bootstrap, "bin") ||
      !sibling(lib, sizeof(lib), bootstrap, "lib")) return 0;
  int length = snprintf(path, sizeof(path), "%s:/app/bin:/usr/bin:/bin", bin);
  if (length <= 0 || (size_t)length >= sizeof(path)) return 0;
  const char* separator = strrchr(bootstrap, '/');
  if (!separator) return 0;
  length = snprintf(loader, sizeof(loader), "%.*s:/app/lib",
                     (int)(separator - bootstrap), bootstrap);
  if (length <= 0 || (size_t)length >= sizeof(loader)) return 0;
  if (child) {
    length = snprintf(preload, sizeof(preload),
        "%s/libzypak-preload-child.so:%s/libzypak-preload-child-spawn-strategy.so", lib, lib);
  } else {
    char cef[PATH_MAX];
    length = snprintf(cef, sizeof(cef), "%.*s/libcef.so", (int)(separator - bootstrap), bootstrap);
    if (length <= 0 || (size_t)length >= sizeof(cef)) return 0;
    length = snprintf(preload, sizeof(preload),
        "%s/libzypak-preload-host.so:%s/libzypak-preload-host-spawn-strategy.so:%s:%s/libzypak-preload-host-spawn-strategy-close.so",
        lib, lib, cef, lib);
  }
  return length > 0 && (size_t)length < sizeof(preload) &&
      !setenv("PATH", path, 1) && !setenv("LD_LIBRARY_PATH", loader, 1) &&
      !setenv("ZYPAK_BIN", bin, 1) &&
      !setenv("ZYPAK_LIB", lib, 1) && !setenv("ZYPAK_ZYGOTE_STRATEGY_SPAWN", "1", 1) &&
      !setenv("LD_PRELOAD", preload, 1) &&
      // Only the browser's pinned packaging bridge uses this filtered endpoint;
      // native guest integrations still have no desktop-service fallback.
      (child || !setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/flatpak/bus", 1));
}
