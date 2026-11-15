// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_UPLOADS_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_UPLOADS_H_
#include <map>
#include "base/functional/callback_helpers.h"
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
#include "content/public/browser/file_select_listener.h"
#include "ui/base/clipboard/file_info.h"
namespace content {
struct CONTENT_EXPORT AlloyGuestUploadFiles {
  std::vector<ui::FileInfo> files;
  std::map<base::FilePath, std::vector<std::u16string>> relative_directories;
  base::FilePath directory;
  // Native adapters disarm only when the result enters its native consumer.
  // Discarding a result must not strand successful materialization until close.
  base::ScopedClosureRunner abandon;
  void Delivered() { (void)abandon.Release(); }
};
using AlloyGuestUploadsReply = base::OnceCallback<void(std::optional<AlloyGuestUploadFiles>)>;
struct CONTENT_EXPORT AlloyGuestUploadProof {
  // UI-only canonical admission, rechecked immediately before grant delivery.
  base::RepeatingCallback<bool()> admitted;
  // Worker-only physical proof of the exact selected, sealed RAM inode.
  base::RepeatingCallback<bool()> verify;
};
CONTENT_EXPORT std::optional<AlloyGuestUploadProof> GetAlloyGuestUploadProof(
    RenderFrameHost* original, const base::FilePath& path);
// UI acquisition, any-sequence final release. Native Blob file references own
// this lease; it grants no permission and cannot extend document admission.
CONTENT_EXPORT base::OnceClosure AcquireAlloyGuestUploadConsumer(
    RenderFrameHost* original, const base::FilePath& path);
// All aliases are private, verified RAM-backed paths to sealed native memfds.
// Paths are never supplied by the app and confer no durable destination rights.
CONTENT_EXPORT void SnapshotAlloyGuestFiles(RenderFrameHost* original,
    AlloyGuestFileCapability::Entries entries, bool directory, AlloyGuestUploadsReply reply,
    base::RepeatingCallback<bool()> admitted = {});
CONTENT_EXPORT void SelectAlloyGuestUploadFiles(RenderFrameHost* original,
    const blink::mojom::FileChooserParams& params, scoped_refptr<FileSelectListener> listener);
}  // namespace content
#endif
