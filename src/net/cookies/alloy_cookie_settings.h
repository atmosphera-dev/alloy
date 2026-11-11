// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef NET_COOKIES_ALLOY_COOKIE_SETTINGS_H_
#define NET_COOKIES_ALLOY_COOKIE_SETTINGS_H_

#include <optional>

#include "net/cookies/cookie_setting_override.h"
#include "url/origin.h"

namespace net {

// Browser-issued factory authority is origin-bound, including on redirects.
// Neither the request initiator nor renderer-supplied cookie overrides mint it.
inline void ApplyAlloyCookieOrigin(
    const std::optional<url::Origin>& captured_origin,
    const GURL& destination,
    CookieSettingOverrides& overrides) {
  overrides.Remove(CookieSettingOverride::kAlloyConsideredFirstParty);
  if (captured_origin && !captured_origin->opaque() &&
      captured_origin->IsSameOriginWith(destination)) {
    overrides.Put(CookieSettingOverride::kAlloyConsideredFirstParty);
  }
}

}  // namespace net

#endif  // NET_COOKIES_ALLOY_COOKIE_SETTINGS_H_
