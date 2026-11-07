// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_LOAD_EPOCH_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_BROWSER_AD_BLOCK_LOAD_EPOCH_H_
#include "base/synchronization/lock.h"
namespace brave_shields {
// Compile outside this fence; publish only while the original source epoch is
// current. Revocation serializes with engine publication and durable DAT writes.
class AdBlockLoadEpoch final {
 public:
  bool current() const { base::AutoLock hold(lock_);return current_; }
  void Revoke() { base::AutoLock hold(lock_);current_=false; }
  template<class F> bool Publish(F&& publish) {
    base::AutoLock hold(lock_);if(!current_)return false;publish();return true;
  }
 private:
  mutable base::Lock lock_;
  bool current_ GUARDED_BY(lock_)=true;
};
}  // namespace brave_shields
#endif
