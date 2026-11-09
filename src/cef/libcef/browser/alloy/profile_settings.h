// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CEF_LIBCEF_BROWSER_ALLOY_PROFILE_SETTINGS_H_
#define CEF_LIBCEF_BROWSER_ALLOY_PROFILE_SETTINGS_H_

#include "brave/alloy/site_records/site_pref_store.h"
#include "cef/libcef/browser/alloy/profile_configuration.h"

class Profile;
namespace content { class BrowserContext; }
namespace alloy {
// Native composition uses the exact projection used by the public API. UI-only;
// explicit profile required; the caller initializes via ReadPrefsAsync and joins
// its native service teardown to the profile/record lifecycle. No global fallback.
scoped_refptr<site_records::SitePrefStore> GetProfilePrefStore(CefRefPtr<AlloyProfile> profile);
// Bind the native embedded-state consumer before its first document. The
// callback reads the original context's existing settings, never a copied list.
void BindNativeEmbedListSettings(Profile& profile);
// Trusted CEF input, never DOM events or renderer messages. The original native
// configuration injects the activity callback; no global/parent profile lookup.
void NotifyProfileUserActivity(content::BrowserContext* context);
bool ProfileNavigationAllowed(content::BrowserContext& context);
bool ProfileNavigationAllowed(const site_records::NativeProfileConfiguration& configuration);
// Browser UI initialization adopts all live pre-CefInitialize profile lifetimes.
void InitializeProfileAutoLock();
// Shutdown adopts even profiles just registered on another embedder thread,
// then joins every key/cache/page teardown before CEF stops its task runners.
void LockProfilesForBrowserClose();
// Per-profile test seam, invoked on the embedder thread after standalone lock
// admission. Lets lifecycle tests control overlap without filesystem timing.
void SetProfileStandaloneLockHookForTesting(CefRefPtr<AlloyProfile> profile,
                                           base::RepeatingClosure hook);
}  // namespace alloy
#endif
