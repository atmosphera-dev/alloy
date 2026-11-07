// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_BUNDLED_DATA_H_
#define BRAVE_ALLOY_BUNDLED_DATA_H_

#include <optional>

#include "base/command_line.h"
#include "base/files/file_path.h"

namespace alloy {

inline constexpr char kDataDirectorySwitch[] = "alloy-data-dir";

// Browser-process input, validated at CEF startup before any consumers run.
// An absent switch never falls back to the module, profile or working directory.
inline std::optional<base::FilePath> DataDirectory() {
  const auto* command_line = base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(kDataDirectorySwitch)) {
    return std::nullopt;
  }
  return command_line->GetSwitchValuePath(kDataDirectorySwitch);
}

}  // namespace alloy

#endif  // BRAVE_ALLOY_BUNDLED_DATA_H_
