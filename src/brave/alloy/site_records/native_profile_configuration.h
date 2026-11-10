// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_NATIVE_PROFILE_CONFIGURATION_H_
#define BRAVE_ALLOY_SITE_RECORDS_NATIVE_PROFILE_CONFIGURATION_H_

#include <memory>
#include <string_view>

#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "brave/alloy/site_records/record_capability.h"
#include "components/keyed_service/core/service_creation_gate.h"
#include "components/prefs/persistent_pref_store.h"

namespace alloy::site_records {

// Inputs chosen by the engine before native Profile construction. The native
// Profile owns this object until after its services and PrefService are gone.
// The injected release callback keeps its explicit encrypted owner alive without
// making the native component import or know its CEF/embedder consumer.
class NativeProfileConfiguration final {
 public:
  // The profile consumer owns its projection and construction/teardown barrier.
  // This common input knows neither a site-settings store nor an AdBlock owner.
  struct Bindings {
    scoped_refptr<PersistentPrefStore> prefs;
    base::RepeatingCallback<bool()> admitted;
    base::RepeatingCallback<bool(std::string_view)> record_working_state_admitted;
    base::OnceCallback<bool(base::OnceClosure)> begin_construction;
    base::RepeatingClosure construction_finished;
    base::OnceClosure finish_teardown;
    base::OnceCallback<void(base::SupportsUserData&)> attach_consumer;
  };
  NativeProfileConfiguration(RecordCapability profile_settings,
                             Bindings bindings,
                             base::OnceClosure release_owner);
  ~NativeProfileConfiguration();
  NativeProfileConfiguration(const NativeProfileConfiguration&) = delete;
  NativeProfileConfiguration& operator=(const NativeProfileConfiguration&) = delete;

  bool admitted() const;
  base::RepeatingCallback<bool(std::string_view)> record_working_state_admission() const {
    return bindings_.record_working_state_admitted;
  }
  bool BeginConstruction();
  void SetTeardownHandler(base::OnceClosure teardown);
  void ConstructionFinished();
  ServiceCreationGate::Callback service_creation_gate() const;
  const RecordCapability& original_generation() const { return profile_settings_; }
  scoped_refptr<PersistentPrefStore> prefs() const { return bindings_.prefs; }
  StorageMode mode() const { return profile_settings_.mode(); }
  bool SharesOwnerWith(const NativeProfileConfiguration& other) const;
  // Typed consumer adapters retain their own private inputs here. Only the
  // adapter knows its key/type; common lifecycle code never interprets them.
  void SetConsumerData(const void* key,
                       std::unique_ptr<base::SupportsUserData::Data> data);
  const base::SupportsUserData::Data* GetConsumerData(const void* key) const;
  // Before path intake, key selection, prefs and keyed services. Get is a weak
  // association only; the native Profile retains the sole configuration owner.
  void Attach(base::SupportsUserData& context);
  static NativeProfileConfiguration* Get(const base::SupportsUserData& context);
  base::WeakPtr<NativeProfileConfiguration> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  class ConsumerData final : public base::SupportsUserData {
   public:
    ~ConsumerData() override = default;
  };
  void RequestTeardown();
  base::ScopedClosureRunner release_owner_;
  ConsumerData consumer_data_;
  const RecordCapability profile_settings_;
  Bindings bindings_;
  bool construction_started_ = false;
  bool teardown_requested_ = false;
  base::OnceClosure teardown_;
  mutable base::WeakPtrFactory<NativeProfileConfiguration> weak_factory_{this};
};

// A native service may own a named portion of the already-selected input. This
// view changes key names only; storage/lifetime/admission stay with that input.
scoped_refptr<PersistentPrefStore> NativeProfilePrefsForNamespace(
    const NativeProfileConfiguration& configuration, std::string_view prefix);

}  // namespace alloy::site_records
#endif
