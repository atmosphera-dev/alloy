// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/public/browser/alloy_document_identity.h"

#include <memory>

#include "brave/alloy/site_records/request_context_policy.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/alloy_document_state.h"
#include "content/public/browser/document_user_data.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"

namespace content {
namespace {
const char kNavigationNoiseKey = 0;
class NavigationNoise final : public base::SupportsUserData::Data {
 public:
  const base::Token token = base::Token::CreateRandom();
};

class DocumentNoise final : public DocumentUserData<DocumentNoise> {
 public:
  ~DocumentNoise() override = default;
  base::Token token = base::Token::CreateRandom();
 private:
  explicit DocumentNoise(RenderFrameHost* frame) : DocumentUserData(frame) {}
  friend class DocumentUserData<DocumentNoise>;
  DOCUMENT_USER_DATA_KEY_DECL();
};
DOCUMENT_USER_DATA_KEY_IMPL(DocumentNoise);
}  // namespace

base::Token AlloyDocumentNoiseIdentity(RenderFrameHost* frame) {
  if (!AlloyDocumentStateAdmitted(frame) ||
      !AlloyDocumentStateAdmitted(frame->GetOutermostMainFrame())) return {};
  return DocumentNoise::GetOrCreateForCurrentDocument(
             frame->GetOutermostMainFrame())->token;
}

base::Token AlloyDocumentNoiseIdentityForNavigation(
    NavigationHandle* navigation) {
  if (auto* parent = navigation->GetParentFrameOrOuterDocument()) {
    return AlloyDocumentNoiseIdentity(parent);
  }
  auto* data = static_cast<NavigationNoise*>(
      navigation->GetUserData(&kNavigationNoiseKey));
  if (!data) {
    auto owned = std::make_unique<NavigationNoise>();
    data = owned.get();
    navigation->SetUserData(&kNavigationNoiseKey, std::move(owned));
  }
  return data->token;
}

void AlloyCommitDocumentNoiseIdentity(RenderFrameHost* frame,
                                      NavigationHandle* navigation) {
  if (!AlloyDocumentStateAdmitted(frame)) return;
  if (!frame->GetParentOrOuterDocument() &&
      alloy::site_records::RequestContextPolicy::IsStateless(
          *frame->GetBrowserContext())) {
    DocumentNoise::GetOrCreateForCurrentDocument(frame)->token =
        AlloyDocumentNoiseIdentityForNavigation(navigation);
  }
}
}  // namespace content
