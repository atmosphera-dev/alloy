// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/document_settings.h"

#include <utility>

#include "base/check.h"
#include "base/memory/ref_counted.h"

namespace alloy::site_records {

DocumentSettings::DocumentSettings(PrefService* configuration, Changed changed)
    : settings_(base::MakeRefCounted<HostContentSettingsMap>(configuration,
                                                             true,
                                                             false,
                                                             false,
                                                             false)),
      changed_(std::move(changed)) {
  settings_->EnableDocumentPermissionState();
  settings_->AddObserver(this);
}

DocumentSettings::~DocumentSettings() {
  shutting_down_ = true;
  settings_->RemoveObserver(this);
  for (auto& [key, service] : services_) {
    service->Shutdown();
  }
  services_.clear();
  settings_->ShutdownOnUIThread();
}

KeyedService* DocumentSettings::GetOrCreateService(const void* key,
                                                   ServiceFactory factory) {
  if (shutting_down_) {
    return nullptr;
  }
  CHECK(key);
  if (auto found = services_.find(key); found != services_.end()) {
    return found->second.get();
  }
  auto service = std::move(factory).Run(settings_.get());
  CHECK(service);
  auto* result = service.get();
  services_.emplace(key, std::move(service));
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
