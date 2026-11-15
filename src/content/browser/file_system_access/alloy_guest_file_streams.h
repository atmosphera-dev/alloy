// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_STREAMS_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_STREAMS_H_
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/blob/blob.mojom-forward.h"
namespace content {
class ChromeBlobStorageContext;
class AlloyGuestFileBinding;
// Called on UI. The ordinary native Blob implementation consumes a revocable,
// backpressured app readable-data handle; no backing file or disk spill exists.
CONTENT_EXPORT void CreateAlloyGuestFileBlob(
    scoped_refptr<ChromeBlobStorageContext> context,
    scoped_refptr<AlloyGuestFileCapability> file, uint64_t size, int64_t modified_ms,
    std::string uuid, std::string mime, mojo::PendingReceiver<blink::mojom::Blob> receiver,
    base::RepeatingCallback<bool()> permission, scoped_refptr<AlloyGuestFileBinding> binding);
}  // namespace content
#endif
