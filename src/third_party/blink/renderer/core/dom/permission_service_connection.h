// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_DOM_PERMISSION_SERVICE_CONNECTION_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_DOM_PERMISSION_SERVICE_CONNECTION_H_

#include "base/functional/callback.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/renderer/core/core_export.h"

namespace blink {
class ExecutionContext;
using PermissionServiceBinder = base::RepeatingCallback<void(
    mojo::PendingReceiver<mojom::blink::PermissionService>)>;

// The connection owns replies; the injected binder retains native browser
// authority and execution-context admission. Client closure ends its lifetime.
CORE_EXPORT void BindPermissionServiceConnection(
    PermissionServiceBinder binder,
    mojo::PendingReceiver<mojom::blink::PermissionService> receiver,
    scoped_refptr<base::SequencedTaskRunner> task_runner);

CORE_EXPORT void ConnectToPermissionService(
    ExecutionContext* execution_context,
    mojo::PendingReceiver<mojom::blink::PermissionService> receiver);
}  // namespace blink
#endif
