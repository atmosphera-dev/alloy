// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CEF_LIBCEF_BROWSER_ALLOY_PROFILE_CONFIGURATION_H_
#define CEF_LIBCEF_BROWSER_ALLOY_PROFILE_CONFIGURATION_H_
#include "brave/alloy/site_records/native_profile_configuration.h"
#include "cef/include/alloy/alloy_profile.h"
namespace alloy {
using NativeProfileConfigurationCallback = base::OnceCallback<void(
    alloy_site_settings_result_t,
    std::unique_ptr<site_records::NativeProfileConfiguration>)>;
// Original-generation input, prepared before native construction. The concrete
// input owns its private data; this interface has no site-setting dependency.
void PrepareNativeProfileConfiguration(CefRefPtr<AlloyProfile> profile,
                                      NativeProfileConfigurationCallback callback);
}  // namespace alloy
#endif
