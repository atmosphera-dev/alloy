// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CHROME_BROWSER_NET_ALLOY_TRAFFIC_POLICY_H_
#define CHROME_BROWSER_NET_ALLOY_TRAFFIC_POLICY_H_

#include "chrome/browser/net/alloy_traffic_prefs.h"
#include "components/policy/core/browser/configuration_policy_handler.h"
#include "components/policy/core/browser/configuration_policy_handler_list.h"
#include "components/policy/core/common/policy_map.h"
#include "components/prefs/pref_value_map.h"

namespace alloy {
// Chromium keeps its original NetworkPredictionOptions preference mapping.
// The same policy also owns/locks Alloy's two independent traffic switches.
class NetworkPredictionTrafficPolicyHandler final
    : public policy::IntRangePolicyHandler {
 public:
  NetworkPredictionTrafficPolicyHandler()
      : IntRangePolicyHandler("NetworkPredictionOptions",
                              nullptr,
                              0,
                              3,
                              false) {}
  void ApplyPolicySettings(const policy::PolicyMap& policies,
                           PrefValueMap* prefs) override {
    const base::Value* value = policies.GetValue("NetworkPredictionOptions",
                                                 base::Value::Type::INTEGER);
    if (!value || value->GetInt() < 0 || value->GetInt() > 3) {
      return;
    }
    // 0/1 = standard (1 is the deprecated Wi-Fi value), 2 = disabled,
    // 3 = extended. Chromium's existing feature gates remain in force.
    // No policy means the user's separate preferences apply.
    const bool enabled = value->GetInt() != 2;
    prefs->SetBoolean(kDnsPrefetchEnabled, enabled);
    prefs->SetBoolean(kPreconnectEnabled, enabled);
  }
};

inline void AddTrafficPolicyHandlers(
    policy::ConfigurationPolicyHandlerList* handlers) {
  handlers->AddHandler(std::make_unique<policy::SimplePolicyHandler>(
      "AlloyFaviconsEnabled", kFaviconsEnabled, base::Value::Type::BOOLEAN));
  handlers->AddHandler(std::make_unique<policy::SimplePolicyHandler>(
      "AlloyOpenSearchEnabled", kOpenSearchEnabled,
      base::Value::Type::BOOLEAN));
  handlers->AddHandler(
      std::make_unique<NetworkPredictionTrafficPolicyHandler>());
}
}  // namespace alloy
#endif
