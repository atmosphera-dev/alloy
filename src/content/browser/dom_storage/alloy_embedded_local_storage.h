// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_DOM_STORAGE_ALLOY_EMBEDDED_LOCAL_STORAGE_H_
#define CONTENT_BROWSER_DOM_STORAGE_ALLOY_EMBEDDED_LOCAL_STORAGE_H_

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "components/services/storage/public/mojom/session_storage_control.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom-forward.h"

namespace alloy::site_records {
class EmbeddedState;
}
namespace blink {
class StorageKey;
}
namespace content {
class StoragePartitionImpl;

// The caller has already validated native frame/process and StorageKey access.
// No fallback to the partition's real data is allowed if this scope is closed.
void BindAlloyEmbeddedLocalStorage(
    StoragePartitionImpl& partition,
    scoped_refptr<alloy::site_records::EmbeddedState> owner,
    const blink::StorageKey& storage_key,
    mojo::PendingReceiver<blink::mojom::StorageArea> receiver);

// Browser-only facade: namespace coordination spans the real controller and
// scope-owned memory controllers, never a renderer-selected backing path.
class AlloySessionStorageRouter : public storage::mojom::SessionStorageControl {
 public:
  virtual storage::mojom::SessionStorageControl& GetRealControlForTesting() = 0;
  virtual void BindTemporary(
      scoped_refptr<alloy::site_records::EmbeddedState> owner,
      const blink::StorageKey& storage_key,
      const std::string& namespace_id,
      mojo::PendingReceiver<blink::mojom::StorageArea> receiver) = 0;
};

std::unique_ptr<AlloySessionStorageRouter> CreateAlloySessionStorageRouter(
    StoragePartitionImpl& partition,
    storage::mojom::SessionStorageControl& real_control);
}  // namespace content

#endif  // CONTENT_BROWSER_DOM_STORAGE_ALLOY_EMBEDDED_LOCAL_STORAGE_H_
