// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_SITE_RECORDS_PROFILE_AUTO_LOCK_H_
#define BRAVE_ALLOY_SITE_RECORDS_PROFILE_AUTO_LOCK_H_

#include "base/functional/callback.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/unguessable_token.h"

namespace alloy::site_records {

enum class ProfileLockReason { kExplicit, kBrowserClose, kIdle, kSleep, kScreenLock, kUserSwitch };

// UI-sequence mechanism. The consumer owns profile admission/teardown and the
// meaning of a locked tab. No browser default idle duration is selected here.
class ProfileAutoLock final {
 public:
  using RequestLock = base::RepeatingCallback<void(ProfileLockReason)>;
  // Empty means locked/transition; a new token is a fresh admitted unlock.
  ProfileAutoLock(base::RepeatingCallback<base::UnguessableToken()> generation, RequestLock request);
  ~ProfileAutoLock();
  void SetIdleTimeout(base::TimeDelta timeout);
  void UserActivity();
  void Signal(ProfileLockReason reason);

 private:
  void Tick();
  const base::RepeatingCallback<base::UnguessableToken()> generation_;
  const RequestLock request_;
  base::TimeDelta timeout_;
  base::TimeTicks last_activity_;
  base::UnguessableToken observed_generation_;
  base::RepeatingTimer timer_;
};

}  // namespace alloy::site_records
#endif
