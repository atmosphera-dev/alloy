// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_SESSION_CONTROL_QUEUE_H_
#define BRAVE_ALLOY_GUEST_SESSION_CONTROL_QUEUE_H_

#include <deque>
#include <utility>
#include "base/run_loop.h"
#include "brave/alloy/guest/protocol/session.h"

namespace alloy::guest {
// One external-message-pump batch at a time. A slow peer stops further native
// pumping and command admission, not just socket draining. There is no queue
// quota or overflow failure. Callbacks enqueue without doing blocking I/O.
class ControlQueue {
 public:
  bool CanProduce() const { return pending_.empty(); }
  void Pump() {
    if (!CanProduce())
      return;
    // CEF's public pump uses RunUntilIdle too, but cannot yield when an event
    // is queued. Own this run level so the first output ends the native batch.
    base::RunLoop loop;
    pump_ = &loop;
    loop.RunUntilIdle();
    pump_ = nullptr;
  }
  bool empty() const { return pending_.empty(); }
  size_t size() const { return pending_.size(); }
  Envelope& front() { return pending_.front(); }
  void pop_front() { pending_.pop_front(); }
  void Push(Envelope envelope, bool sending_front = false) {
    if (pump_)
      pump_->Quit();
    auto first = pending_.begin();
    if (sending_front && first != pending_.end())
      ++first;  // Never alter the logical frame already being sent.
    if (envelope.message == Message::kDocumentStarted) {
      for (auto it = first; it != pending_.end();) {
        if (it->message == Message::kTitle && it->owner.view == envelope.owner.view)
          it = pending_.erase(it);
        else
          ++it;
      }
    }
    if (envelope.message == Message::kTitle) {
      for (auto it = first; it != pending_.end(); ++it) {
        if (it->message == Message::kTitle && it->owner == envelope.owner) {
          it->payload = std::move(envelope.payload);
          return;
        }
      }
    }
    pending_.push_back(std::move(envelope));
  }
 private:
  base::RunLoop* pump_ = nullptr;
  std::deque<Envelope> pending_;
};
}  // namespace alloy::guest
#endif
