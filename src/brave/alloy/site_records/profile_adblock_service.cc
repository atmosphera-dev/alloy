// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_adblock_service.h"

#include "base/functional/bind.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/pref_service_factory.h"

namespace alloy::site_records {
ProfileAdBlockService::ProfileAdBlockService(scoped_refptr<ProfileFilterPreferences> original)
    : original_(std::move(original)) {}
ProfileAdBlockService::~ProfileAdBlockService() = default;
std::unique_ptr<ProfileAdBlockService> ProfileAdBlockService::Create(
    scoped_refptr<ProfileFilterPreferences> original,std::string locale,
    component_updater::ComponentUpdateService* components,
    ProfileFilterPreferences::SubscriptionFetch fetch) {
  if(!original || !original->available() || !fetch)return nullptr;
  auto owner=std::unique_ptr<ProfileAdBlockService>(new ProfileAdBlockService(original));
  auto registry=base::MakeRefCounted<PrefRegistrySimple>();
  brave_shields::RegisterPrefsForAdBlockService(registry.get());
  PrefServiceFactory factory;factory.set_user_prefs(original);
  owner->prefs_=factory.Create(registry);
  brave_shields::AdBlockService::ProfileBackends backends;
  backends.resources=original->ResourceBackend();
  backends.dat=original->DATBackend();
  backends.subscriptions=original->SubscriptionBackend(std::move(fetch));
  backends.observe_close=base::BindRepeating(&ProfileFilterPreferences::ObserveClose,original);
  backends.close_failed=base::BindRepeating(&ProfileFilterPreferences::ConsumerFailed,original,FileStatus::kCancelled);
  owner->native_=std::make_unique<brave_shields::AdBlockService>(owner->prefs_.get(),
      std::move(locale),components,original->task_runner(),std::move(backends));
  if(!original->available())return nullptr;
  return owner;
}
brave_shields::AdBlockService* ProfileAdBlockService::Get() const {
  return original_->available() && native_ && !native_->IsClosed() ? native_.get() : nullptr;
}
PrefService* ProfileAdBlockService::prefs() const { return Get() ? prefs_.get() : nullptr; }
bool ProfileAdBlockService::Close(base::OnceClosure done) {return native_&&native_->Close(std::move(done));}
}  // namespace alloy::site_records
