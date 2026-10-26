// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "third_party/blink/renderer/core/dom/permission_service_connection.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "third_party/blink/public/mojom/permissions/permission_status.mojom-blink.h"
#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"

namespace blink {
namespace {
using Service = mojom::blink::PermissionService;
using Descriptor = mojom::blink::PermissionDescriptorPtr;
using Control = mojom::blink::EmbeddedPermissionRequestDescriptorPtr;
using Reply = mojom::blink::PermissionStatusWithDetailsPtr;

Reply CancelledReply() {
  auto reply = mojom::blink::PermissionStatusWithDetails::New();
  reply->status = mojom::blink::PermissionStatus::DENIED;
  return reply;
}

Vector<Reply> CancelledReplies(wtf_size_t count) {
  Vector<Reply> replies;
  replies.ReserveInitialCapacity(count);
  for (wtf_size_t i = 0; i < count; ++i) {
    replies.push_back(CancelledReply());
  }
  return replies;
}

// The browser remains the authority. This connection owns response obligations
// even when that browser's document receiver disappears before IPC dispatch.
// Like native PendingRequest cancellation, failed transport can only deny.
class PermissionServiceConnection final : public Service {
 public:
  PermissionServiceConnection(
      PermissionServiceBinder binder,
      scoped_refptr<base::SequencedTaskRunner> task_runner)
      : binder_(std::move(binder)), task_runner_(std::move(task_runner)) {}

  void HasPermission(Descriptor permission,
                     HasPermissionCallback callback) override {
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        std::move(callback), CancelledReply());
    Browser()->HasPermission(std::move(permission), std::move(reply));
  }

  void RequestPermission(Descriptor permission,
                         RequestPermissionCallback callback) override {
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        std::move(callback), CancelledReply());
    Browser()->RequestPermission(std::move(permission), std::move(reply));
  }

  void RequestPermissions(Vector<Descriptor> permissions,
                          RequestPermissionsCallback callback) override {
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        std::move(callback), CancelledReplies(permissions.size()));
    Browser()->RequestPermissions(std::move(permissions), std::move(reply));
  }

  void RevokePermission(Descriptor permission,
                        RevokePermissionCallback callback) override {
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        std::move(callback), CancelledReply());
    Browser()->RevokePermission(std::move(permission), std::move(reply));
  }

  void RequestPageEmbeddedPermission(
      Vector<Descriptor> permissions,
      Control descriptor,
      RequestPageEmbeddedPermissionCallback callback) override {
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
        std::move(callback),
        mojom::blink::EmbeddedPermissionControlResult::kDenied);
    Browser()->RequestPageEmbeddedPermission(
        std::move(permissions), std::move(descriptor), std::move(reply));
  }

  void RegisterPageEmbeddedPermissionControl(
      Vector<Descriptor> permissions,
      Control descriptor,
      mojo::PendingRemote<mojom::blink::EmbeddedPermissionControlClient> client)
      override {
    Browser()->RegisterPageEmbeddedPermissionControl(
        std::move(permissions), std::move(descriptor), std::move(client));
  }

  void AddPermissionObserver(
      Descriptor permission,
      Reply last_known_status,
      mojo::PendingRemote<mojom::blink::PermissionObserver> observer) override {
    Browser()->AddPermissionObserver(std::move(permission),
                                     std::move(last_known_status),
                                     std::move(observer));
  }

  void AddPageEmbeddedPermissionObserver(
      Descriptor permission,
      mojom::blink::PermissionStatus last_known_status,
      mojo::PendingRemote<mojom::blink::PermissionObserver> observer) override {
    Browser()->AddPageEmbeddedPermissionObserver(
        std::move(permission), last_known_status, std::move(observer));
  }

  void NotifyEventListener(Descriptor permission,
                           const String& event_type,
                           bool is_added) override {
    Browser()->NotifyEventListener(std::move(permission), event_type, is_added);
  }

 private:
  Service* Browser() {
    if (browser_.is_bound() && !browser_.is_connected()) {
      browser_.reset();
    }
    if (!browser_.is_bound()) {
      auto endpoint = browser_.BindNewPipeAndPassReceiver(task_runner_);
      const uint64_t generation = ++generation_;
      browser_.set_disconnect_handler(base::BindOnce(
          &PermissionServiceConnection::BrowserDisconnected,
          weak_factory_.GetWeakPtr(), generation));
      binder_.Run(std::move(endpoint));
    }
    return browser_.get();
  }

  void BrowserDisconnected(uint64_t generation) {
    if (generation == generation_) {
      browser_.reset();
    }
  }

  const PermissionServiceBinder binder_;
  const scoped_refptr<base::SequencedTaskRunner> task_runner_;
  mojo::Remote<Service> browser_;
  uint64_t generation_ = 0;
  base::WeakPtrFactory<PermissionServiceConnection> weak_factory_{this};
};
}  // namespace

void BindPermissionServiceConnection(
    PermissionServiceBinder binder,
    mojo::PendingReceiver<mojom::blink::PermissionService> receiver,
    scoped_refptr<base::SequencedTaskRunner> task_runner) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<PermissionServiceConnection>(std::move(binder),
                                                   task_runner),
      std::move(receiver), task_runner);
}

void ConnectToPermissionService(
    ExecutionContext* execution_context,
    mojo::PendingReceiver<mojom::blink::PermissionService> receiver) {
  BindPermissionServiceConnection(
      BindRepeating(
          [](ExecutionContext* context,
             mojo::PendingReceiver<mojom::blink::PermissionService> endpoint) {
            if (context && !context->IsContextDestroyed()) {
              context->GetBrowserInterfaceBroker().GetInterface(
                  std::move(endpoint));
            }
          },
          WrapWeakPersistent(execution_context)),
      std::move(receiver),
      execution_context->GetTaskRunner(TaskType::kPermission));
}
}  // namespace blink
