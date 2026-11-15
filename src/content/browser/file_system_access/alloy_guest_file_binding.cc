// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "content/browser/file_system_access/alloy_guest_file_binding.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
namespace content {
AlloyGuestFileBinding::AlloyGuestFileBinding(RenderFrameHost* frame)
    : base::RefCountedDeleteOnSequence<AlloyGuestFileBinding>(base::SequencedTaskRunner::GetCurrentDefault()),
      document_(frame->GetWeakDocumentPtr()) {}
AlloyGuestFileBinding::~AlloyGuestFileBinding() = default;
scoped_refptr<AlloyGuestFileBinding> AlloyGuestFileBinding::Create(RenderFrameHost* frame,
    scoped_refptr<AlloyGuestFileCapability> source, scoped_refptr<AlloyGuestFileBinding> issuer) {
  if (!frame || !source) return nullptr;
  if (issuer && issuer->Admitted() && issuer->original() == frame) return issuer;
  auto* bridge = AlloyGuestIntegration::Get(frame);
  if (!bridge) return nullptr;
  auto binding = base::WrapRefCounted(new AlloyGuestFileBinding(frame));
  binding->source_ = source; binding->issuer_ = issuer;
  auto retire = base::BindRepeating([](base::WeakPtr<AlloyGuestFileBinding> weak) {
    if (weak) { auto retained = base::WrapRefCounted(weak.get()); retained->Retire(); }
  }, binding->weak_.GetWeakPtr());
  binding->admitted_ = bridge->CaptureLifetime(frame, base::BindOnce(retire));
  if (!binding->Admitted()) { binding->Retire(); return nullptr; }
  binding->source_retired_ = source->OnRetired(base::BindOnce(retire));
  if (issuer) binding->issuer_retired_ = issuer->OnRetired(base::BindOnce(retire));
  return binding;
}
bool AlloyGuestFileBinding::Admitted() const {
  return live_ && source_ && source_->Admitted() && (!issuer_ || issuer_->Admitted()) &&
      admitted_ && admitted_.Run();
}
void AlloyGuestFileBinding::Retire() {
  if (!live_) return;
  auto retained = base::WrapRefCounted(this);
  live_ = false; source_retired_ = {}; issuer_retired_ = {};
  retired_.Notify(); admitted_.Reset(); issuer_.reset(); source_.reset();
}
base::CallbackListSubscription AlloyGuestFileBinding::OnRetired(base::OnceClosure callback) {
  if (!Admitted()) { std::move(callback).Run(); return {}; }
  return retired_.Add(std::move(callback));
}
}
