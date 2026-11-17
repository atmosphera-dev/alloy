// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "brave/alloy/guest/linux_lock/protection.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int alloy_lock_writer_paths(void) {
  if (!alloy_lock_scratch_valid()) return 0;
  int root = alloy_lock_scratch_fd();
  char link[64], path[PATH_MAX];
  snprintf(link, sizeof(link), "/proc/self/fd/%d", root);
  ssize_t size = readlink(link, path, sizeof(path) - 1);
  if (size <= 0 || size >= (ssize_t)sizeof(path) - 1) return 0;
  path[size] = 0;
  const char* directories[] = {"home", "config", "cache", "data", "state",
      "runtime", "tmp", "cef", "home/.pki", "home/.pki/nssdb", "data/pki",
      "data/pki/nssdb", "cache/fontconfig"};
  for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
    if (mkdirat(root, directories[i], 0700) && errno != EEXIST) return 0;
    int fd = openat(root, directories[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_uid != getuid() ||
        (st.st_mode & 0777) != 0700) {
      if (fd >= 0) close(fd);
      return 0;
    }
    close(fd);
  }
  const char* names[] = {"HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME",
      "XDG_STATE_HOME", "XDG_RUNTIME_DIR", "TMPDIR", "ALLOY_GUEST_CEF_ROOT"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    char value[PATH_MAX];
    if (snprintf(value, sizeof(value), "%s/%s", path, directories[i]) >= (int)sizeof(value) ||
        setenv(names[i], value, 1)) return 0;
  }
  // Fontconfig keeps system read-only font discovery, with a single RAM cache
  // owner. NSS's old HOME and new XDG destinations are both inside this root.
  char config[PATH_MAX], body[PATH_MAX + 512];
  if (snprintf(config, sizeof(config), "%s/fontconfig.conf", path) >= (int)sizeof(config)) return 0;
  int length = snprintf(body, sizeof(body),
      "<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
      "<fontconfig><include ignore_missing=\"yes\">/etc/fonts/fonts.conf</include>"
      "<reset-dirs/><dir>/usr/share/fonts</dir><dir>/usr/local/share/fonts</dir>"
      "<cachedir>%s/cache/fontconfig</cachedir></fontconfig>\n", path);
  int fd = openat(root, "fontconfig.conf", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0 && errno != EEXIST) return 0;
  if (fd >= 0) {
    int ok = length > 0 && length < (int)sizeof(body) && write(fd, body, length) == length;
    if (close(fd) || !ok) return 0;
  }
  return !setenv("FONTCONFIG_FILE", config, 1) &&
      !setenv("GSETTINGS_BACKEND", "memory", 1) && !setenv("GIO_USE_VFS", "local", 1) &&
      !setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", 1) &&
      !setenv("DBUS_SYSTEM_BUS_ADDRESS", "disabled:", 1) && !setenv("NO_AT_BRIDGE", "1", 1) &&
      !setenv("MESA_SHADER_CACHE_DISABLE", "true", 1) && !setenv("__GL_SHADER_DISK_CACHE", "0", 1);
}
