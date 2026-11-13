// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_CLIENT_H_
#define BRAVE_ALLOY_GUEST_SESSION_CLIENT_H_
#include <string>
#include "brave/alloy/guest/session/session.h"
#include "cef/include/alloy/alloy_guest_session.h"

namespace alloy::guest {
struct Settings {
  std::string mode;
  std::string network;
  Protection required = Protection::kUnavailable;
  bool developmental = false;
  std::string data_directory;
};
bool ParseProtection(const std::string& value, Protection* protection);
std::string ProtectionName(Protection protection);
bool ValidateSettings(const Settings& settings);
std::string BuildIdentity();
std::string ApiIdentity();
}  // namespace alloy::guest
#endif
