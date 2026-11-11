<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental profile decay

Decay uses the same whole-record deletion boundary as explicit deletion, not
cache cleanup. Policy and clocks belong to the explicit profile.

`AlloyProfile::SetDecaySettings(period_seconds, timebase)` persists policy
encrypted with an unlocked profile. Zero disables decay; a positive period
counts from the last committed top-level document visit. The one supported
timebase is `ALLOY_DECAY_SINCE_LAST_VISIT`. New Alloy profiles have decay
disabled; an embedder such as Wakaba chooses its own period, not an engine
one-week default. Invalid values preserve the old policy.

`GetDecaySettings(period_seconds, timebase)` requires an unlocked profile and
changes outputs only on success. Both are blocking background-thread calls.
Native C uses `set_decay_settings` and output pointers in `get_decay_settings`.

Unlock applies the stored policy before admitting record reads. Alloy also
enforces it periodically while unlocked, even when the embedder never opens
a settings UI. Expiry retires the complete record through the same key/native
owner barrier as explicit deletion. It does not lock the whole profile.
Locked records remain encrypted on disk until the next unlock can enforce expiry.

A committed top-level document refreshes last visit. Reads, embedded loads,
subresources, scripts and background writes do not. Settings-only records with
no visit use their creation time as the expiry baseline. Last-visit metadata
and policy remain encrypted; public listing only returns owned copies.

See [site records](site-records.md) for lifecycle and honest deletion limits,
and [auto-lock](profile-auto-lock.md) for the separate monotonic idle timer.
