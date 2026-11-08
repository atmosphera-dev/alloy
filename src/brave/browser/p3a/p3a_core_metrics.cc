/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "brave/browser/p3a/p3a_core_metrics.h"
#include "base/time/time.h"
#include "components/prefs/pref_registry_simple.h"

namespace brave {
BraveWindowTracker::BraveWindowTracker(PrefService*) {}
BraveWindowTracker::~BraveWindowTracker() = default;
void BraveWindowTracker::CreateInstance(PrefService*) {}
bool BraveWindowTracker::HasInstance() { return false; }
void BraveWindowTracker::ClearInstance() {}
void BraveWindowTracker::RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterTimePref("core_p3a_metrics.incognito_used_timestamp", {});
  registry->RegisterBooleanPref("core_p3a_metrics.tor_used", false);
}
void BraveWindowTracker::OnBrowserCreated(BrowserWindowInterface*) {}
void BraveWindowTracker::OnBrowserActivated(BrowserWindowInterface*) {}
}  // namespace brave
