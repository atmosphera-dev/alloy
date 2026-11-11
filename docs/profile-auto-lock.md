<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Experimental profile auto-lock

Alloy enforces encrypted-profile locking. The embedder supplies policy through
`AlloyProfile::SetAutoLock(idle_milliseconds, handler)` on browser UI after
`CefInitialize`. Zero disables idle locking only; a negative duration is refused.
No Wakaba idle duration or persisted default is built into Alloy.

CEF initialization adopts profiles created/opened before initialization too.
`CefShutdown` joins profile locking even when the embedder retains the profile
or never created a request context. Closing an individual tab is not quitting
the browser. Abrupt process death relies on OS process teardown, not a callback.

## Triggers and availability

| # | Trigger | Mechanism |
|---|---|---|
| 1 | Browser close | CEF shutdown joins the profile lifecycle before its runners stop |
| 2 | Idle | Monotonic engine timer; trusted CEF key, mouse and touch input resets it |
| 3 | Sleep | System-bus logind Manager `PrepareForSleep(true)`, including its initial property |
| 4 | Screen lock | Own logind Session `Lock` and `LockedHint`, plus session-bus ScreenSaver `ActiveChanged(true)` |
| 5 | User switch | Own logind Session `Active=false` or removal |

Alloy resolves its own session with `GetSessionByPID`, never the arbitrary
currently active session. It subscribes before reading initial properties.
The existing Linux ScreenSaver service names from Chromium's idle implementation
are supported. These mechanisms do not start a service or install a daemon.
Resume, unlock and becoming active never unlock a profile.

`OnLockTriggersChanged(mask)` reports availability: each reason's bit is
`1 << reason`. Engine idle and browser close are always available. Missing
Linux providers are not interpreted as user activity or an unlocked desktop.
The embedder chooses its unsupported-provider policy and may supply trusted
sleep/screen-lock/user-switch events with `NotifyLockSignal`. UI input outside
CEF surfaces can call `NotifyUserActivity`; DOM events/background work do not
reset the timer. The input timer measures activity delivered to this profile,
not activity in another application or an unavailable OS-wide idle oracle.

## Lock completion and tab UI

Automatic, supplied and explicit runtime locks share the original profile's
key/native-cache teardown barrier. Navigation admission closes first, including
retained contexts and prepared configurations. Entry is safe inside observer and
embedder callbacks: native destruction runs in a posted task after the triggering
callback returns. Repeated UI lock calls join the transition and each accepted
callback receives its completed result. The generation's WebContents and
observers are destroyed while native services remain admitted. Native profiles
finish their teardown notifications, completed writes are flushed, and only then
are service admission, plaintext caches and keys closed. Pending unlocks are
cancelled, and completion waits for their transient keys.
Public context access goes through CEF's initialization check, which rejects the
same pending generation immediately. This refuses filter/preference writes,
embed-list changes and embedded-state configuration without closing native
services used during teardown. Profile setters and record deletion reject the
pending transition before obtaining their settings projection. Queued APIs may
return true for delivering a CANCELLED reply; that is not mutation admission.
`OnLocked(reason, result)` is a transient UI callback, not a stored event log.
Only OK acknowledges completed teardown. Failed operations are not represented
as successful locks. The ordinary explicit `LockAsync` completion remains.

All original CEF browsers are retired. The handler may close its application's
UI tabs, or retain their UI shells and show its own locked page in a separate
stateless context. It cannot keep using the retired encrypted context or delay
key wiping. Unlock requires the selected key source again and a fresh context;
old contexts remain revoked. The handler may reenter or release its owners.
An embedder handler holding its profile should clear that reference or replace
the handler with NULL when releasing it, as with other reference-counted CEF
callbacks. CEF shutdown releases the handler after its final callback.

## Verification

On `alloy-do-build`, workspace `/home/builder/work/alloy-b`, with
`TMPDIR=/home/builder/work/alloy-b/tmp`:

```sh
python3 repo/tools/profile_probe.py --out src/out/Verify_Alloy --check autolock
python3 repo/tools/profile_probe.py --out src/out/Release_Alloy --check autolock
```

The public SDK consumer uses a private loopback network namespace and owned
D-Bus service doubles, not a real suspend or desktop lock. It exercises actual
production logind/screensaver subscriptions, native input idle reset, key/read
refusal, original page/worker process retirement, both tab UI choices, explicit
unlock/readback and browser-close locking of retained standalone profiles.
Explicit lock and browser close also run while a server holds a navigation:
the completed native preference is read back after unlocking or reopening,
and the retired context cannot admit another document or native preferences.
Callback-lock cases also attempt filter/preference replacements, both embed-list
setters and record deletion before the posted teardown. Refusals are checked
immediately or through their cancellation callbacks, and reopening verifies that
only the previously accepted filters, preference and site record persisted.
`alloy_site_records_unittests` also covers timer lifetime and D-Bus parsing,
initial state, provider absence/loss and session ownership. Physical desktop
locker integration and a real suspend are outside this fixture.
