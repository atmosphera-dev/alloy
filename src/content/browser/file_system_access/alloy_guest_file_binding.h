// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_BINDING_H_
#define CONTENT_BROWSER_FILE_SYSTEM_ACCESS_ALLOY_GUEST_FILE_BINDING_H_
#include "content/browser/file_system_access/alloy_guest_file_capability.h"
namespace content {
// Operation lifetime only. Native grants continue to decide permissions. This
// retains the issuing binding even when selected source data has a wider life.
class CONTENT_EXPORT AlloyGuestFileBinding final
    : public base::RefCountedDeleteOnSequence<AlloyGuestFileBinding> {
 public:
  static scoped_refptr<AlloyGuestFileBinding> Create(RenderFrameHost* frame,
      scoped_refptr<AlloyGuestFileCapability> source,
      scoped_refptr<AlloyGuestFileBinding> issuer = nullptr);
  bool Admitted() const;
  RenderFrameHost* original() const { return document_.AsRenderFrameHostIfValid(); }
  void Retire();
  base::CallbackListSubscription OnRetired(base::OnceClosure callback);
 private:
  friend class base::RefCountedDeleteOnSequence<AlloyGuestFileBinding>;
  friend class base::DeleteHelper<AlloyGuestFileBinding>;
  explicit AlloyGuestFileBinding(RenderFrameHost* frame);
  ~AlloyGuestFileBinding();
  WeakDocumentPtr document_;
  scoped_refptr<AlloyGuestFileCapability> source_;
  scoped_refptr<AlloyGuestFileBinding> issuer_;
  base::RepeatingCallback<bool()> admitted_;
  base::CallbackListSubscription source_retired_, issuer_retired_;
  base::OnceClosureList retired_;
  bool live_ = true;
  base::WeakPtrFactory<AlloyGuestFileBinding> weak_{this};
};
}
#endif
