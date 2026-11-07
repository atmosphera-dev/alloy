// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/native_profile_configuration.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "components/prefs/wrap_with_prefix_pref_store.h"

namespace alloy::site_records {
namespace {
const char kNativeConfiguration = 0;
class Reference final : public base::SupportsUserData::Data {
 public:
  explicit Reference(base::WeakPtr<NativeProfileConfiguration> value)
      : value(std::move(value)) {}
  const base::WeakPtr<NativeProfileConfiguration> value;
};
class NamespaceView final : public WrapWithPrefixPrefStore {
 public:
  NamespaceView(scoped_refptr<PersistentPrefStore> parent, std::string_view prefix)
      : WrapWithPrefixPrefStore(parent, prefix), parent_(std::move(parent)) {}
  void CommitPendingWrite(base::OnceClosure reply, base::OnceClosure done) override {
    parent_->CommitPendingWrite(std::move(reply), std::move(done));
  }
 private:
  ~NamespaceView() override = default;
  const scoped_refptr<PersistentPrefStore> parent_;
};
}  // namespace

NativeProfileConfiguration::NativeProfileConfiguration(
    RecordCapability profile_settings, Bindings bindings,
    base::OnceClosure release_owner)
    : release_owner_(std::move(release_owner)),
      profile_settings_(std::move(profile_settings)), bindings_(std::move(bindings)) {
  CHECK(bindings_.prefs);
  CHECK(bindings_.admitted);
  CHECK(bindings_.begin_construction);
  CHECK(bindings_.construction_finished);
  CHECK(bindings_.finish_teardown);
  CHECK(profile_settings_.mode() == StorageMode::kEncrypted);
}
NativeProfileConfiguration::~NativeProfileConfiguration() {
  std::move(bindings_.finish_teardown).Run();
}
bool NativeProfileConfiguration::SharesOwnerWith(
    const NativeProfileConfiguration& other) const {
  return profile_settings_.profile() == other.profile_settings_.profile() &&
      profile_settings_.generation() == other.profile_settings_.generation();
}
void NativeProfileConfiguration::SetConsumerData(
    const void* key, std::unique_ptr<base::SupportsUserData::Data> data) {
  CHECK(!construction_started_);
  CHECK(data && !consumer_data_.GetUserData(key));
  consumer_data_.SetUserData(key, std::move(data));
}
const base::SupportsUserData::Data* NativeProfileConfiguration::GetConsumerData(
    const void* key) const {
  return consumer_data_.GetUserData(key);
}

bool NativeProfileConfiguration::admitted() const {
  return profile_settings_.Allows(StorePurpose::kProfileSettings,
                                  StorageAccess::kReadRecordFiles) &&
      bindings_.admitted.Run();
}
void NativeProfileConfiguration::Attach(base::SupportsUserData& context) {
  CHECK(construction_started_); CHECK(!Get(context));
  RequestContextPolicy::AttachEncrypted(context, profile_settings_);
  ServiceCreationGate::Attach(context, service_creation_gate());
  context.SetUserData(&kNativeConfiguration,
      std::make_unique<Reference>(weak_factory_.GetWeakPtr()));
  if (bindings_.attach_consumer) std::move(bindings_.attach_consumer).Run(context);
}
NativeProfileConfiguration* NativeProfileConfiguration::Get(
    const base::SupportsUserData& context) {
  const auto* reference = static_cast<const Reference*>(context.GetUserData(&kNativeConfiguration));
  return reference ? reference->value.get() : nullptr;
}
bool NativeProfileConfiguration::BeginConstruction() {
  if (construction_started_ || !bindings_.begin_construction ||
      !profile_settings_.Allows(StorePurpose::kProfileSettings, StorageAccess::kReadRecordFiles)) return false;
  construction_started_ = std::move(bindings_.begin_construction).Run(base::BindOnce(
      &NativeProfileConfiguration::RequestTeardown, weak_factory_.GetWeakPtr()));
  return construction_started_;
}
void NativeProfileConfiguration::SetTeardownHandler(base::OnceClosure teardown) {
  CHECK(!teardown_); CHECK(teardown);
  teardown_ = std::move(teardown);
  if (teardown_requested_) RequestTeardown();
}
void NativeProfileConfiguration::RequestTeardown() {
  teardown_requested_ = true;
  if (teardown_) std::move(teardown_).Run();
}
void NativeProfileConfiguration::ConstructionFinished() {
  CHECK(construction_started_);
  bindings_.construction_finished.Run();
}
ServiceCreationGate::Callback NativeProfileConfiguration::service_creation_gate() const {
  return base::BindRepeating([](base::WeakPtr<NativeProfileConfiguration> owner) {
    return owner && owner->admitted();
  }, weak_factory_.GetWeakPtr());
}
scoped_refptr<PersistentPrefStore> NativeProfilePrefsForNamespace(
    const NativeProfileConfiguration& configuration, std::string_view prefix) {
  return base::MakeRefCounted<NamespaceView>(configuration.prefs(), prefix);
}
}  // namespace alloy::site_records
