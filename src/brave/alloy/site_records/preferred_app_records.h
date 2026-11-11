// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PREFERRED_APP_RECORDS_H_
#define BRAVE_ALLOY_SITE_RECORDS_PREFERRED_APP_RECORDS_H_

#include "base/values.h"
#include "brave/alloy/site_records/authenticated_objects.h"
#include "components/services/app_service/public/cpp/preferred_app.h"

namespace alloy::site_records {

// Lossless native entries -> owner fragments -> native entries. Opaque join IDs
// and ordinals live only inside encrypted shards; no global URL/grant index.
FileStatus EncodePreferredAppRecords(const apps::PreferredApps& entries,
                                     base::ListValue* fragments);
FileStatus DecodePreferredAppRecords(const base::ListValue& fragments,
                                     apps::PreferredApps* entries);
// Empty owner means explicit non-site/global dispatch configuration. A bounded
// authority is assigned through native AuthorityView and R16, never a string
// search for URLs or a renderer-selected filesystem path.
FileStatus PreferredAppRecordOwner(const base::Value& fragment,
                                   std::string* owner);

}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_PREFERRED_APP_RECORDS_H_
