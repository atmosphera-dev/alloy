// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef COMPONENTS_CONTENT_SETTINGS_CORE_BROWSER_OWNED_SETTINGS_REGISTRIES_H_
#define COMPONENTS_CONTENT_SETTINGS_CORE_BROWSER_OWNED_SETTINGS_REGISTRIES_H_

#include "components/content_settings/core/browser/content_settings_registry.h"
#include "components/content_settings/core/browser/permission_settings_registry.h"
#include "components/content_settings/core/browser/website_settings_registry.h"

namespace content_settings {
// A scoped native schema graph for profile operations before CefInitialize.
// The dependency-injected native constructors run the SAME registration code
// (including Brave's registrations), without touching any global LazyInstance
// or requiring a process AtExitManager. Destruction is in dependency order.
class OwnedSettingsRegistries final {
 public:
  OwnedSettingsRegistries()
      : permissions_(&website_), content_(&permissions_, &website_) {}
  ~OwnedSettingsRegistries() = default;
  OwnedSettingsRegistries(const OwnedSettingsRegistries&) = delete;
  OwnedSettingsRegistries& operator=(const OwnedSettingsRegistries&) = delete;

  const WebsiteSettingsRegistry& website() const { return website_; }
  const PermissionSettingsRegistry& permissions() const { return permissions_; }
  const ContentSettingsRegistry& content() const { return content_; }

 private:
  WebsiteSettingsRegistry website_;
  PermissionSettingsRegistry permissions_;
  ContentSettingsRegistry content_;
};
}  // namespace content_settings
#endif
