// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_PUBLIC_BROWSER_ALLOY_GUEST_INTEGRATION_H_
#define CONTENT_PUBLIC_BROWSER_ALLOY_GUEST_INTEGRATION_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/files/file_path.h"
#include "base/supports_user_data.h"
#include "base/values.h"
#include "base/time/time.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/weak_document_ptr.h"

namespace content {

// Native adapters know the original native document and their typed operation.
// The injected owner knows transport, not the adapters' permission decisions.
// Bytes are copied values, never host paths, descriptors or native pointers.
struct AlloyGuestIntegrationData {
  base::DictValue details;
  std::vector<std::string> bytes;
  // Optional adapter-local lifetime, not a permission cache. It can retire a
  // clipboard receiver, chooser or stream before its native document ends.
  base::RepeatingCallback<bool()> admitted;
  // Native adapters validate their own reply shape/action IDs before dispatch.
  base::RepeatingCallback<bool(const base::DictValue&,
                                const std::vector<std::string>&)> validate_reply;
};

class AlloyGuestIntegration : public base::SupportsUserData::Data {
 public:
  // Pinned X11 selection_owner.cc kIncrementalTransferTimeout. Also bounds
  // clipboard file selection/materialization, not general app file I/O.
  static constexpr auto kClipboardReadTimeout = base::Seconds(10);
  using Reply = base::OnceCallback<void(
      std::optional<AlloyGuestIntegrationData>)>;
  enum class Operation {
    kClipboardRead,
    kClipboardWrite,
    kClipboardSequence,
    kMenu,
    kDragSource,
    kFullscreen,
    kSelectFiles,
    kFileRead,
    kFileWrite,
    kFileTruncate,
    kFileClose,
    kDownload,
    kClipboardPermission,
  };

  // Called on the native UI sequence. Never waits for the app. Absence, native
  // document retirement, provider loss and transport failure complete denied.
  virtual void Request(RenderFrameHost* original,
                       Operation operation,
                       AlloyGuestIntegrationData data,
                       Reply reply) = 0;
  // Native queued commands/capabilities capture the same canonical authority as
  // requests. Document-only capabilities survive geometry/GPU epochs, not a
  // navigation, document replacement, provider loss or canonical revocation.
  virtual base::RepeatingCallback<bool()> CaptureAdmission(
      RenderFrameHost* original, bool document_only = false) = 0;
  // Long-lived external capabilities use the same admission plus exactly-once
  // retirement notification. The default supplies no such capability.
  virtual base::RepeatingCallback<bool()> CaptureLifetime(
      RenderFrameHost* original, base::OnceClosure retired) { return {}; }
  // A native navigation being transferred into a download may outlive its own
  // abort-without-commit. Ignore only that browser-owned navigation ID when
  // capturing; any subsequent or ancestor navigation still retires the lease.
  virtual base::RepeatingCallback<bool()> CaptureDownloadAdmission(
      RenderFrameHost* original, int64_t navigation_id) { return CaptureAdmission(original, true); }
  // Release a library-generated app file registration, never a filesystem path.
  virtual void RetireFile(const std::string& capability) {}
  // Native download-item observer outcome, independently of app sink replies.
  virtual void DownloadProgress(const std::string& identity, const std::string& state,
      int64_t received, int64_t total) {}
  // CEF's OSR callback otherwise loses the browser-owned source frame. This
  // native-only handoff never becomes a serialized frame pointer or authority.
  void BeginNativeDrag(RenderFrameHost* original) {
    drag_source_ = original ? original->GetWeakDocumentPtr() : WeakDocumentPtr();
  }
  RenderFrameHost* NativeDragSource() const {
    return drag_source_.AsRenderFrameHostIfValid();
  }

  static AlloyGuestIntegration* Get(WebContents* contents) {
    if (contents) contents = contents->GetOutermostWebContents();
    return contents ? static_cast<AlloyGuestIntegration*>(
                          contents->GetUserData(&key_)) : nullptr;
  }
  static AlloyGuestIntegration* Get(RenderFrameHost* frame) {
    return frame ? Get(WebContents::FromRenderFrameHost(frame)) : nullptr;
  }
  static void Install(WebContents* contents,
                      std::unique_ptr<AlloyGuestIntegration> integration) {
    contents->SetUserData(&key_, std::move(integration));
  }
  // Native file-grant prompt continuation. Resolves only registered selected
  // objects; the path is an internal entry identity, not filesystem authority.
  static void RequestFilePermission(RenderFrameHost* original, const base::FilePath& path,
      bool external, bool write, base::OnceCallback<void(bool)> callback);

 private:
  static inline const int key_ = 0;
  WeakDocumentPtr drag_source_;
};

}  // namespace content
#endif
