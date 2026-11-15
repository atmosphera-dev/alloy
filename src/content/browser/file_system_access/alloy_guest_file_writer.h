// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_WRITER_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_WRITER_H_
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
#include "content/browser/file_system_access/file_system_access_lock_manager.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "third_party/blink/public/mojom/file_system_access/file_system_access_file_writer.mojom.h"
namespace content {
class AlloyGuestFileBinding;
CONTENT_EXPORT blink::mojom::FileSystemAccessErrorPtr AlloyGuestFileError(
    const AlloyGuestFileCapability::Result& result);
CONTENT_EXPORT mojo::PendingRemote<blink::mojom::FileSystemAccessFileWriter>
CreateAlloyGuestFileWriter(scoped_refptr<AlloyGuestFileCapability> transaction,
    base::RepeatingCallback<bool()> permission,
    scoped_refptr<FileSystemAccessLockManager::LockHandle> lock, bool auto_close,
    scoped_refptr<AlloyGuestFileBinding> binding);
}  // namespace content
#endif
