// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PREF_CACHE_CLEAR_H_
#define BRAVE_ALLOY_SITE_RECORDS_PREF_CACHE_CLEAR_H_

#include "components/prefs/pref_value_map.h"

namespace alloy::site_records {
// Clear owned decoded payload buffers before releasing the preference tree.
// Container keys are released with their containers; this is not a claim about
// allocator history or copies an external caller already received.
void ClearPrefCache(PrefValueMap& values);
void ClearPrefCache(base::DictValue& values);
}  // namespace alloy::site_records
#endif
