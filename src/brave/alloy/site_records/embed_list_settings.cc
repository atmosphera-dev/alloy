// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/embed_list_settings.h"

#include <algorithm>
#include <set>

#include "brave/alloy/site_records/record_capability.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "url/origin.h"

namespace alloy::site_records::embed_list {
namespace {
std::string Domain(const GURL& url) {
  return RecordDomainForPrincipal(url, url::Origin::Create(url));
}
}  // namespace

std::optional<Replacement> Canonicalize(const GURL& outer,
                                      const std::vector<GURL>& embedded) {
  Replacement result;
  result.outer = Domain(outer);
  if (result.outer.empty()) return std::nullopt;
  std::set<std::string> seen;
  for (const auto& site : embedded) {
    auto domain = Domain(site);
    if (domain.empty()) return std::nullopt;
    if (seen.insert(domain).second) result.embedded.Append(std::move(domain));
  }
  return result;
}

bool Read(const base::Value* value, const GURL& outer,
          std::vector<std::string>* domains) {
  const auto owner = Domain(outer);
  if (!domains || owner.empty()) return false;
  std::vector<std::string> result;
  if (value) {
    if (!value->is_dict()) return false;
    if (const auto* entry = value->GetDict().Find(owner)) {
      if (!entry->is_list()) return false;
      for (const auto& embedded : entry->GetList()) {
        if (!embedded.is_string()) return false;
        const auto domain = Domain(ContentSettingsPattern::FromString(
            embedded.GetString()).ToRepresentativeUrl());
        if (domain.empty()) return false;
        if (std::ranges::find(result, domain) == result.end()) result.push_back(domain);
      }
    }
  }
  *domains = std::move(result);
  return true;
}

bool Contains(const base::Value* value, const GURL& outer, const GURL& embedded) {
  const auto domain = Domain(embedded);
  std::vector<std::string> domains;
  return !domain.empty() && Read(value, outer, &domains) &&
      std::ranges::find(domains, domain) != domains.end();
}
}  // namespace alloy::site_records::embed_list
