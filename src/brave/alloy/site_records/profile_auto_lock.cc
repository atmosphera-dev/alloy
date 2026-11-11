// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/site_records/profile_auto_lock.h"

#include <utility>
#include "base/functional/bind.h"

namespace alloy::site_records {

ProfileAutoLock::ProfileAutoLock(base::RepeatingCallback<base::UnguessableToken()> generation, RequestLock request)
    : generation_(std::move(generation)), request_(std::move(request)),
      last_activity_(base::TimeTicks::Now()), observed_generation_(generation_.Run()) {}
ProfileAutoLock::~ProfileAutoLock() = default;
void ProfileAutoLock::SetIdleTimeout(base::TimeDelta timeout) {
  timeout_ = timeout;
  UserActivity();
  if (timeout_.is_positive()) timer_.Start(FROM_HERE, base::Milliseconds(100), this, &ProfileAutoLock::Tick);
  else timer_.Stop();
}
void ProfileAutoLock::UserActivity() { last_activity_ = base::TimeTicks::Now(); }
void ProfileAutoLock::Signal(ProfileLockReason reason) { request_.Run(reason); }
void ProfileAutoLock::Tick() {
  const auto generation = generation_.Run();
  const auto now = base::TimeTicks::Now();
  if (!generation.is_empty() && generation != observed_generation_) last_activity_ = now;
  observed_generation_ = generation;
  if (!generation.is_empty() && timeout_.is_positive() && now - last_activity_ >= timeout_)
    request_.Run(ProfileLockReason::kIdle);
}

}  // namespace alloy::site_records
