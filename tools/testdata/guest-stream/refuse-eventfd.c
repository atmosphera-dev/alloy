// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Test-consumer startup fault, never injected into an admitted guest process.
#include <errno.h>
#include <sys/eventfd.h>
#include <unistd.h>
int eventfd(unsigned int value, int flags) {
  (void)value;
  (void)flags;
  const char marker[] = "M3_EVENTFD_REFUSED\n";
  (void)write(STDERR_FILENO, marker, sizeof(marker) - 1);
  errno = EMFILE;
  return -1;
}
