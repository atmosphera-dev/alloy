// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_BUNDLED_DATA_H_
#define BRAVE_ALLOY_BUNDLED_DATA_H_

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/path_service.h"

namespace alloy {

// Installed with libcef. Component consumers receive immutable release data,
// independent of user profiles, updater availability and the working directory.
inline base::FilePath BundledDataDirectory() {
  return base::PathService::CheckedGet(base::DIR_MODULE)
      .AppendASCII("alloy-data");
}

}  // namespace alloy

#endif  // BRAVE_ALLOY_BUNDLED_DATA_H_
