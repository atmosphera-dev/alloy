// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef CEF_LIBCEF_BROWSER_OSR_ALLOY_GUEST_DRAG_SEQUENCE_H_
#define CEF_LIBCEF_BROWSER_OSR_ALLOY_GUEST_DRAG_SEQUENCE_H_
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"

// Acknowledgement lifetime only. The existing OSR target owns drag data and
// Chromium's renderer still decides whether the document handles a drop.
class AlloyGuestDragSequence {
 public:
  base::OnceCallback<void(bool)> Reply() {
    ++pending_;
    const auto serial = ++latest_;
    auto reply = mojo::WrapCallbackWithDefaultInvokeIfNotRun(base::BindOnce(
        &AlloyGuestDragSequence::Acknowledged, weak_.GetWeakPtr(), serial), std::optional<bool>());
    return base::BindOnce([](base::OnceCallback<void(std::optional<bool>)> reply, bool handling) {
      std::move(reply).Run(handling);
    }, std::move(reply));
  }
  bool DeferDropIfPending(base::OnceClosure drop) {
    if (!pending_) return false;
    drop_ = std::move(drop);
    return true;
  }
  std::optional<bool> Handling() const { return handling_; }
  void Reset() {
    weak_.InvalidateWeakPtrs(); pending_ = 0; latest_ = 0;
    handling_.reset(); drop_.Reset();
  }
 private:
  void Acknowledged(uint64_t serial, std::optional<bool> handling) {
    if (!handling) { Reset(); return; }
    if (serial == latest_) handling_ = *handling;
    --pending_;
    if (!pending_ && drop_) std::move(drop_).Run();
  }
  size_t pending_ = 0;
  uint64_t latest_ = 0;
  std::optional<bool> handling_;
  base::OnceClosure drop_;
  base::WeakPtrFactory<AlloyGuestDragSequence> weak_{this};
};
#endif
