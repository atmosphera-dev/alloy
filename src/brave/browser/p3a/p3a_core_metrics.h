/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef BRAVE_BROWSER_P3A_P3A_CORE_METRICS_H_
#define BRAVE_BROWSER_P3A_P3A_CORE_METRICS_H_
#include "build/build_config.h"
#if BUILDFLAG(IS_ANDROID)
#error This file should only be included on desktop.
#endif
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"

class BrowserWindowInterface;
class PrefService;
class PrefRegistrySimple;
namespace brave {
class BraveWindowTracker : public BrowserCollectionObserver {
 public:
  explicit BraveWindowTracker(PrefService* local_state);
  BraveWindowTracker(const BraveWindowTracker&) = delete;
  BraveWindowTracker& operator=(const BraveWindowTracker&) = delete;
  ~BraveWindowTracker() override;
  static void CreateInstance(PrefService* local_state);
  static bool HasInstance();
  static void ClearInstance();
  static void RegisterPrefs(PrefRegistrySimple* registry);

 private:
  void OnBrowserCreated(BrowserWindowInterface* browser) override;
  void OnBrowserActivated(BrowserWindowInterface* browser) override;
};
}  // namespace brave
#endif
