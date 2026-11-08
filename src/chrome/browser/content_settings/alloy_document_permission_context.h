// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CHROME_BROWSER_CONTENT_SETTINGS_ALLOY_DOCUMENT_PERMISSION_CONTEXT_H_
#define CHROME_BROWSER_CONTENT_SETTINGS_ALLOY_DOCUMENT_PERMISSION_CONTEXT_H_

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "brave/alloy/site_records/request_context_policy.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/weak_document_ptr.h"

namespace alloy::site_records {

inline base::RepeatingCallback<bool()> CaptureDocumentPermissionAdmission(
    content::RenderFrameHost* frame, base::RepeatingCallback<bool()> record) {
  const auto original = frame ? frame->GetWeakDocumentPtr() : content::WeakDocumentPtr();
  return base::BindRepeating([](content::WeakDocumentPtr original,
                              base::RepeatingCallback<bool()> record) {
    return record.Run() && content::AlloyDocumentStateAdmitted(original.AsRenderFrameHostIfValid());
  }, original, std::move(record));
}

// The native factory supplies its ordinary getter. Working-state documents and
// embedded scopes construct complete native contexts with their map's owner.
template <typename T>
T* DocumentPermissionContext(content::RenderFrameHost* frame,
                             T* (*profile_getter)(Profile*)) {
  if (!frame) {
    return nullptr;
  }
  auto* profile = Profile::FromBrowserContext(frame->GetBrowserContext());
  if (RequestContextPolicy::Get(*profile) &&
      !content::AlloyDocumentStateAdmitted(frame)) {
    return nullptr;
  }
  if (!HostContentSettingsMapFactory::UsesScopedSettings(frame)) {
    return profile_getter(profile);
  }
  static const int type_key = 0;
  return static_cast<T*>(
      HostContentSettingsMapFactory::GetStatelessDocumentService(
          frame, &type_key,
          base::BindOnce(
              [](Profile* owner_profile, T* (*getter)(Profile*),
                 HostContentSettingsMap* map) -> std::unique_ptr<KeyedService> {
                auto context = std::make_unique<T>(owner_profile, map);
                context->SetDocumentScope(*getter(owner_profile));
                return context;
              },
              profile, profile_getter)));
}

}  // namespace alloy::site_records
#endif  // CHROME_BROWSER_CONTENT_SETTINGS_ALLOY_DOCUMENT_PERMISSION_CONTEXT_H_
