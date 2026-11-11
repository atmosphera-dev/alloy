// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_LIST_HANDLE_H_
#define COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_LIST_HANDLE_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
#include "components/services/app_service/public/cpp/intent.h"
#include "components/services/app_service/public/cpp/preferred_app.h"

class GURL;

namespace apps {

// A read-only handle to the preferred apps list set by the user. This class
// allows for observation of the list and accessor methods. To make changes to
// the list, use App Service Proxy methods.
class PreferredAppsListHandle {
 public:
  PreferredAppsListHandle();
  ~PreferredAppsListHandle();

  PreferredAppsListHandle(const PreferredAppsListHandle&) = delete;
  PreferredAppsListHandle& operator=(const PreferredAppsListHandle&) = delete;

  virtual bool IsInitialized() const = 0;
  // Get the entry size of the preferred app list.
  virtual size_t GetEntrySize() const = 0;
  // Get a copy of the preferred apps.
  virtual PreferredApps GetValue() const = 0;
  virtual const PreferredApps& GetReference() const = 0;

  virtual bool IsPreferredAppForSupportedLinks(
      const std::string& app_id) const = 0;

  // Find preferred app id for an |url|.
  virtual std::optional<std::string> FindPreferredAppForUrl(
      const GURL& url) const = 0;

  // Find preferred app id for an |intent|.
  virtual std::optional<std::string> FindPreferredAppForIntent(
      const IntentPtr& intent) const = 0;

  // Returns overlapping apps, excluding app_id itself and consulting the
  // conflict delegate when an app_id is supplied.
  virtual base::flat_set<std::string> FindPreferredAppsForFilters(
      std::optional<std::string> app_id,
      const IntentFilters& intent_filters) const = 0;

  class Observer : public base::CheckedObserver {
   public:
    virtual void OnPreferredAppChanged(const std::string& app_id,
                                       bool is_preferred_app) = 0;
    virtual void OnPreferredAppsListInitialized() {}

    // Observers must remove themselves from handle during this callback.
    virtual void OnPreferredAppsListWillBeDestroyed(
        PreferredAppsListHandle* handle) = 0;

   protected:
    ~Observer() override = default;
  };

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

 protected:
  // An observer can synchronously replace grants, revoke admission, or retire
  // the owner. Destruction notifications use this same list. The model's
  // lifetime/revision checks cancel stale grants in the notification drain.
  base::ReentrantObserverList<Observer> observers_;
};

}  // namespace apps

#endif  // COMPONENTS_SERVICES_APP_SERVICE_PUBLIC_CPP_PREFERRED_APPS_LIST_HANDLE_H_
