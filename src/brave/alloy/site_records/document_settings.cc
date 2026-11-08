// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/document_settings.h"

#include <utility>

#include "base/check.h"
#include "base/memory/ref_counted.h"

namespace alloy::site_records {

struct DocumentSettings::State : public base::RefCounted<State> {
  explicit State(PrefService* configuration)
      : map(base::MakeRefCounted<HostContentSettingsMap>(configuration,
                                                         true,
                                                         false,
                                                         false,
                                                         false,
                                                         true)) {
    map->EnableDocumentPermissionState();
  }
  const scoped_refptr<HostContentSettingsMap> map;
  std::map<const void*, std::unique_ptr<KeyedService>> services;
  bool closing = false;

 private:
  friend class base::RefCounted<State>;
  ~State() { map->ShutdownOnUIThread(); }
};

DocumentSettings::DocumentSettings(PrefService* configuration, Changed changed)
    : state_(base::MakeRefCounted<State>(configuration)),
      changed_(std::move(changed)) {
  state_->map->AddObserver(this);
}

DocumentSettings::~DocumentSettings() {
  Shutdown();
}

void DocumentSettings::Shutdown() {
  auto state = state_;
  if (state->closing) {
    return;
  }
  state->closing = true;
  state->map->RemoveObserver(this);
  auto services = std::move(state->services);
  state->services.clear();
  for (auto& [key, service] : services) {
    if (!service) {
      continue;
    }
    service->Shutdown();
  }
  services.clear();
}

HostContentSettingsMap& DocumentSettings::settings() {
  return *state_->map;
}

KeyedService* DocumentSettings::GetOrCreateService(const void* key,
                                                   ServiceFactory factory) {
  // A native constructor may synchronously retire this entire wrapper. Keep
  // the native map alive through rejected-client shutdown, then close it.
  auto state = state_;
  if (state->closing) {
    return nullptr;
  }
  CHECK(key);
  auto [slot, created] = state->services.try_emplace(key, nullptr);
  if (!created) {
    return slot->second.get();
  }
  auto service = std::move(factory).Run(state->map.get());
  if (state->closing) {
    if (service) {
      service->Shutdown();
    }
    return nullptr;
  }
  CHECK(service);
  auto* result = service.get();
  slot->second = std::move(service);
  return result;
}

void DocumentSettings::OnContentSettingChanged(
    const ContentSettingsPattern& primary,
    const ContentSettingsPattern& secondary,
    ContentSettingsTypeSet types) {
  auto changed = changed_;
  changed.Run(primary, secondary, types);
}

}  // namespace alloy::site_records
