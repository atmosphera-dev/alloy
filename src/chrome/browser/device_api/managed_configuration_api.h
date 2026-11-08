// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_DEVICE_API_MANAGED_CONFIGURATION_API_H_
#define CHROME_BROWSER_DEVICE_API_MANAGED_CONFIGURATION_API_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/sequence_checker.h"
#include "base/unguessable_token.h"
#include "chrome/browser/device_api/managed_configuration_store.h"
#include "components/keyed_service/core/keyed_service.h"
#include "components/prefs/pref_change_registrar.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"
#include "url/origin.h"

class Profile;
class PrefService;
namespace user_prefs { class PrefRegistrySyncable; }

// Owns native origin cache bindings and their asynchronous refresh lifetimes.
// Browser/Profile composition supplies storage admission and network factories;
// no completion is allowed to recapture a newer origin/record authority.
class ManagedConfigurationAPI : public KeyedService {
 public:
  class Observer : public base::CheckedObserver {
   public:
    virtual void OnManagedConfigurationChanged() = 0;
    virtual const url::Origin& GetOrigin() const = 0;
  };
  using StoreFactory = base::RepeatingCallback<std::optional<ManagedConfigurationStore::Options>(const url::Origin&)>;
  using LoaderGetter = base::RepeatingCallback<network::mojom::URLLoaderFactory*()>;

  static const char kOriginKey[];
  static const char kManagedConfigurationUrlKey[];
  static const char kManagedConfigurationHashKey[];
  explicit ManagedConfigurationAPI(Profile* profile);
  ManagedConfigurationAPI(Profile* profile, LoaderGetter loaders);
  ManagedConfigurationAPI(PrefService* prefs, StoreFactory stores, LoaderGetter loaders);
  ~ManagedConfigurationAPI() override;
  ManagedConfigurationAPI(const ManagedConfigurationAPI&) = delete;
  ManagedConfigurationAPI& operator=(const ManagedConfigurationAPI&) = delete;
  static void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

  void GetOriginPolicyConfiguration(const url::Origin& origin,
      const std::vector<std::string>& keys,
      base::OnceCallback<void(std::optional<base::DictValue>)> callback);
  void GetOriginPolicyVersion(const url::Origin& origin,
      base::OnceCallback<void(ManagedConfigurationStore::Version)> callback);
  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);
  bool CanHaveManagedStore(const url::Origin& origin);
  const std::set<url::Origin>& GetManagedOrigins() const;
  // Engine deletion removes admission before calling this. Explicit refresh
  // after a fresh browser-owned admission may acquire a new binding.
  void RevokeOrigin(const url::Origin& origin);
  void RefreshOrigin(const url::Origin& origin);

 private:
  struct Entry;
  using Configuration = std::pair<std::string, std::string>;
  using Policies = std::map<url::Origin, std::optional<Configuration>>;
  Policies ReadPolicies() const;
  Entry* Active(const url::Origin& origin, base::UnguessableToken revision);
  Entry* Bind(const url::Origin& origin);
  void OnConfigurationPolicyChanged();
  void UpdateStoredDataForOrigin(const url::Origin& origin, const std::string& url,
                                 const std::string& hash);
  void OnVersion(const url::Origin& origin, base::UnguessableToken revision,
                  ManagedConfigurationStore::Version version);
  void DecodeData(const url::Origin& origin, base::UnguessableToken revision,
                   std::optional<std::string> data);
  void OnCommitted(const url::Origin& origin, base::UnguessableToken revision,
                    ManagedConfigurationStore::UpdateResult result);
  void PromoteObservers();

  const raw_ptr<PrefService> prefs_;
  const StoreFactory stores_;
  const LoaderGetter loaders_;
  std::map<url::Origin, std::unique_ptr<Entry>> entries_;
  std::map<url::Origin, base::ObserverList<Observer>> observers_;
  std::set<url::Origin> managed_origins_;
  std::set<raw_ptr<Observer, SetExperimental>> unmanaged_observers_;
  PrefChangeRegistrar pref_change_registrar_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<ManagedConfigurationAPI> weak_ptr_factory_{this};
};

#endif  // CHROME_BROWSER_DEVICE_API_MANAGED_CONFIGURATION_API_H_
