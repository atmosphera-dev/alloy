// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/pref_cache_clear.h"

#include "brave/alloy/site_records/authenticated_objects.h"

namespace alloy::site_records {
namespace {
void ClearValue(base::Value& value) {
  if (value.is_string()) Cleanse(base::as_writable_byte_span(value.GetString()));
  else if (value.is_blob()) Cleanse(value.GetBlob());
  else if (value.is_dict()) ClearPrefCache(value.GetDict());
  else if (value.is_list()) for (auto& entry : value.GetList()) ClearValue(entry);
  value = base::Value();
}
}  // namespace
void ClearPrefCache(PrefValueMap& values) {
  for (auto& [key, value] : values) ClearValue(value);
  values.Clear();
}
void ClearPrefCache(base::DictValue& values) {
  for (auto entry : values) ClearValue(entry.second);
  values.clear();
}
}  // namespace alloy::site_records
