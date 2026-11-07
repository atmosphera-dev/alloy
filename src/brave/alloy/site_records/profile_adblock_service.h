// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_ADBLOCK_SERVICE_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_ADBLOCK_SERVICE_H_

#include "brave/alloy/site_records/profile_filter_preferences.h"
#include "brave/components/brave_shields/content/browser/ad_block_service.h"

namespace alloy::site_records {
// Browser-owner composition of one explicit configuration and native engine
// pair. The original profile FIFO owns both engine work and private backing.
// BrowserContext selection/retention is supplied by the browser adapter.
class ProfileAdBlockService final {
 public:
  static std::unique_ptr<ProfileAdBlockService> Create(
      scoped_refptr<ProfileFilterPreferences> original,std::string locale,
      component_updater::ComponentUpdateService* components,
      ProfileFilterPreferences::SubscriptionFetch fetch);
  ~ProfileAdBlockService();
  brave_shields::AdBlockService* Get() const;
  PrefService* prefs() const;
  bool Close(base::OnceClosure done);
 private:
  explicit ProfileAdBlockService(scoped_refptr<ProfileFilterPreferences> original);
  const scoped_refptr<ProfileFilterPreferences> original_;
  std::unique_ptr<PrefService> prefs_;
  std::unique_ptr<brave_shields::AdBlockService> native_;
};
}  // namespace alloy::site_records
#endif
