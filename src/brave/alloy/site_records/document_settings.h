// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_DOCUMENT_SETTINGS_H_
#define BRAVE_ALLOY_SITE_RECORDS_DOCUMENT_SETTINGS_H_

#include <map>
#include <memory>

#include "base/functional/callback.h"
#include "components/content_settings/core/browser/content_settings_observer.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/keyed_service/core/keyed_service.h"

class PrefService;

namespace alloy::site_records {

// A native settings shard whose decisions and notifications have one owner.
// Configuration stays live through PrefService; user decisions never enter it.
class DocumentSettings final : public content_settings::Observer {
 public:
  using Changed = base::RepeatingCallback<void(const ContentSettingsPattern&,
                                               const ContentSettingsPattern&,
                                               ContentSettingsTypeSet)>;
  using ServiceFactory = base::OnceCallback<std::unique_ptr<KeyedService>(
      HostContentSettingsMap*)>;

  DocumentSettings(PrefService* configuration, Changed changed);
  ~DocumentSettings() override;
  DocumentSettings(const DocumentSettings&) = delete;
  DocumentSettings& operator=(const DocumentSettings&) = delete;

  HostContentSettingsMap& settings() { return *settings_; }
  // Key is a browser-owned type tag, never renderer data. The native service
  // stays unique within this document and shuts down before its settings map.
  // Closing admission during shutdown also prevents reentrant recreation.
  KeyedService* GetOrCreateService(const void* key, ServiceFactory factory);

 private:
  void OnContentSettingChanged(const ContentSettingsPattern& primary,
                               const ContentSettingsPattern& secondary,
                               ContentSettingsTypeSet types) override;

  const scoped_refptr<HostContentSettingsMap> settings_;
  const Changed changed_;
  std::map<const void*, std::unique_ptr<KeyedService>> services_;
  bool shutting_down_ = false;
};

}  // namespace alloy::site_records
#endif  // BRAVE_ALLOY_SITE_RECORDS_DOCUMENT_SETTINGS_H_
