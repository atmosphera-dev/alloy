// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/site_profile_configuration.h"

#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"

namespace alloy::site_records {
namespace {
const char kSiteProfileSettings = 0;
class SiteOwner final : public base::SupportsUserData::Data {
 public:
  explicit SiteOwner(scoped_refptr<SitePrefStore> settings)
      : settings(std::move(settings)) {}
  const scoped_refptr<SitePrefStore> settings;
};
}  // namespace

std::unique_ptr<NativeProfileConfiguration> MakeSiteProfileConfiguration(
    RecordCapability original, scoped_refptr<NativePrefView> view,
    scoped_refptr<SitePrefStore> settings, base::OnceClosure release_owner,
    base::OnceCallback<void(base::SupportsUserData&)> attach_consumer,
    base::RepeatingCallback<bool()> consumer_admitted) {
  CHECK(view && settings && view->source_owner() == settings.get());
  NativeProfileConfiguration::Bindings bindings;
  bindings.prefs = view;
  bindings.record_working_state_admitted = base::BindRepeating(
      &SitePrefStore::RecordWorkingStateAdmitted, settings);
  bindings.admitted = base::BindRepeating([](base::WeakPtr<NativePrefView> view,
      base::RepeatingCallback<bool()> consumer_admitted) {
    return view && view->CanStartServiceConstruction() &&
        (!consumer_admitted || consumer_admitted.Run());
  }, view->GetWeakPtr(), std::move(consumer_admitted));
  bindings.begin_construction = base::BindOnce(
      [](scoped_refptr<NativePrefView> view, base::OnceClosure teardown) {
        return view->BeginConstruction(std::move(teardown));
      }, view);
  bindings.construction_finished = base::BindRepeating(
      &NativePrefView::ConstructionFinished, view);
  bindings.finish_teardown = base::BindOnce(&NativePrefView::FinishTeardown, view);
  bindings.attach_consumer = std::move(attach_consumer);
  auto input = std::make_unique<NativeProfileConfiguration>(
      std::move(original), std::move(bindings), std::move(release_owner));
  input->SetConsumerData(&kSiteProfileSettings, std::make_unique<SiteOwner>(std::move(settings)));
  return input;
}

scoped_refptr<SitePrefStore> GetSiteProfileSettings(
    const NativeProfileConfiguration& configuration) {
  const auto* owner = static_cast<const SiteOwner*>(configuration.GetConsumerData(&kSiteProfileSettings));
  return owner ? owner->settings : nullptr;
}

scoped_refptr<PersistentPrefStore> SiteProfilePrefsForNamespace(
    const NativeProfileConfiguration& configuration, std::string_view prefix) {
  CHECK(GetSiteProfileSettings(configuration));
  return NativeProfilePrefsForNamespace(configuration, prefix);
}

std::optional<RecordValueStoreBinding> SiteProfileManagedOrigin(
    const NativeProfileConfiguration& configuration, const url::Origin& origin) {
  auto settings = GetSiteProfileSettings(configuration);
  return configuration.admitted() && settings ? settings->BindManagedOrigin(origin) : std::nullopt;
}
}  // namespace alloy::site_records
