// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Exercise the pinned adapter's real message owner without a browser or bus.
#include "dbus/bus_writable_message.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int Copies(int original) {
  struct stat identity;
  assert(!fstat(original, &identity));
  DIR* directory = opendir("/proc/self/fd");
  assert(directory);
  int copies = 0;
  while (auto* entry = readdir(directory)) {
    char* end;
    long fd = strtol(entry->d_name, &end, 10);
    if (*end || end == entry->d_name || fd < 0) continue;
    struct stat value;
    if (!fstat(fd, &value) && value.st_dev == identity.st_dev && value.st_ino == identity.st_ino)
      ++copies;
  }
  closedir(directory);
  return copies;
}

int main() {
  const char* temporary = getenv("TMPDIR");
  assert(temporary);
  std::string name = std::string(temporary) + "/alloy-message-fd-XXXXXX";
  int fd = mkstemp(name.data());
  assert(fd >= 0 && !unlink(name.c_str()));
  assert(Copies(fd) == 1);
  {
    zypak::dbus::MethodCall call(
        {"org.example.Alloy", "/org/example/Alloy", "org.example.Alloy"}, "Test");
    DBusMessageIter writer;
    dbus_message_iter_init_append(call.message(), &writer);
    assert(dbus_message_iter_append_basic(&writer, DBUS_TYPE_UNIX_FD, &fd));
    assert(Copies(fd) == 2);
    auto copy = call;
    assert(Copies(fd) == 2);
  }
  int retained = Copies(fd) - 1;
  printf("ZYPAK_MESSAGE_RETAINED_FDS=%d\n", retained);
  close(fd);
  return retained ? 1 : 0;
}
