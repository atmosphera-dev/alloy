// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_EMBED_LIST_SETTINGS_H_
#define BRAVE_ALLOY_SITE_RECORDS_EMBED_LIST_SETTINGS_H_

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "url/gurl.h"

namespace alloy::site_records::embed_list {
inline constexpr char kPreference[] = "alloy.considered_first_party";

struct Replacement {
  std::string outer;
  base::ListValue embedded;
};

// One schema and R16 validation contract for encrypted and memory settings.
std::optional<Replacement> Canonicalize(const GURL& outer,
                                      const std::vector<GURL>& embedded);
bool Read(const base::Value* value, const GURL& outer,
          std::vector<std::string>* domains);
bool Contains(const base::Value* value, const GURL& outer, const GURL& embedded);
}  // namespace alloy::site_records::embed_list
#endif
