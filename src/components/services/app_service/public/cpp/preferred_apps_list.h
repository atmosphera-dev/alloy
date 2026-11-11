// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_LIST_H_
#define COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_LIST_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/services/app_service/public/cpp/intent.h"
#include "components/services/app_service/public/cpp/intent_filter.h"
#include "components/services/app_service/public/cpp/preferred_app.h"
#include "components/services/app_service/public/cpp/preferred_apps_list_handle.h"

class GURL;
namespace apps {
class PreferredAppsList : public PreferredAppsListHandle {
 public:
  class Delegate {
   public:
    virtual bool QueryConflict(const std::string& first_app_id,
        const IntentFilterPtr& first_filter, const std::string& second_app_id,
        const IntentFilterPtr& second_filter) = 0;
    virtual bool IsWebAppInExtendedScope(const GURL& url, const std::string& app_id) const = 0;
   protected:
    virtual ~Delegate() = default;
  };
  explicit PreferredAppsList(Delegate* delegate);
  ~PreferredAppsList();
  PreferredAppsList(const PreferredAppsList&) = delete;
  PreferredAppsList& operator=(const PreferredAppsList&) = delete;
  void Init();
  void Init(PreferredApps preferred_apps);
  // Install before notifications. Removed supported-link app IDs get false;
  // nested changes drain current removals without delivering stale grants.
  void Replace(PreferredApps preferred_apps);
  // A revoked/error read view never exposes cached grants. Retained data is
  // not discarded and can be inspected by its owning persistence controller.
  void SetAvailable(bool available);
  void SetAvailabilityCallback(base::RepeatingCallback<bool()> admitted);
  PreferredApps GetPendingValue() const;
  void SetLongestPrefixMatchEnabled(bool enabled) {
    if (longest_prefix_match_enabled_ != enabled) {
      longest_prefix_match_enabled_ = enabled;
      ++revision_;
    }
  }
  ReplacedAppPreferences AddPreferredApp(const std::string& app_id, const IntentFilterPtr& intent_filter);
  IntentFilters DeletePreferredApp(const std::string& app_id, const IntentFilterPtr& intent_filter);
  IntentFilters DeleteAppId(const std::string& app_id);
  IntentFilters DeleteSupportedLinks(const std::string& app_id);
  bool IsInitialized() const override;
  size_t GetEntrySize() const override;
  PreferredApps GetValue() const override;
  const PreferredApps& GetReference() const override;
  bool IsPreferredAppForSupportedLinks(const std::string& app_id) const override;
  std::optional<std::string> FindPreferredAppForUrl(const GURL& url) const override;
  std::optional<std::string> FindPreferredAppForIntent(const IntentPtr& intent) const override;
  base::flat_set<std::string> FindPreferredAppsForFilters(std::optional<std::string> app_id,
                                                        const IntentFilters& filters) const override;

 private:
  bool EntryExists(const std::string& app_id, const IntentFilterPtr& filter);
  bool Available() const;
  base::flat_set<std::string> Supported() const;
  void Notify(std::vector<std::pair<std::string, bool>> changes, bool initialized);
  PreferredApps preferred_apps_;
  const PreferredApps unavailable_;
  raw_ptr<Delegate> delegate_ = nullptr;
  bool longest_prefix_match_enabled_ = false;
  bool initialized_ = false;
  bool available_ = true;
  base::RepeatingCallback<bool()> admitted_;
  uint64_t revision_ = 0;
  struct Notification {
    std::string id;
    bool value;
    uint64_t revision;
  };
  std::deque<Notification> pending_notifications_;
  bool pending_initialization_ = false;
  bool notifying_ = false;
  mutable base::WeakPtrFactory<PreferredAppsList> weak_ptr_factory_{this};
};
}  // namespace apps
#endif  // COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_LIST_H_
