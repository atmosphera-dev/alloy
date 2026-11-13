// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "include/alloy/alloy_guest_session.h"

int main(int argc, char** argv) {
  CefMainArgs args(argc, argv);
  return AlloyRunGuestHost(args);
}
