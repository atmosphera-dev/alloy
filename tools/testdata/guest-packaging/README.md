<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Local guest packaging fixtures

Flatpak uses zypak v2025.09, commit
`693a71c5ffa80ec9c9ce2ae03b1ccc493c698e53`, with `zypak-alloy.patch`.
The patch transfers only the existing Chromium launch manifest and an explicit
RAM scratch directory to each fixed bootstrap entry. It does not propagate a
Landlock marker into an unrelated kernel lineage. The bootstrap applies the
policy again before loading libcef.

The patch also fixes the pinned writable-message owner's extra reference,
which otherwise keeps duplicated launch/RAM descriptors open after send. The
real adapter message regression `message_fds.cc` must observe zero retained FDs
after the final message owner is destroyed, including copied owners.

Dynamic environment values go directly into the owning spawn request, not the
launcher's borrowed string-view map. `spawn_environment.cc` exercises the actual
spawn delegate under AddressSanitizer and checks exact transferred values.
Both regressions run while building the fixture, not inside installed payloads.

Install zypak's bin/ and lib/ directories under `zypak/` beside the Alloy
bootstrap and libcef. This package-specific layout is checked before engine
entry; there is no caller-selected preload or executable. The package needs
`--filesystem=xdg-run/alloy-guest:create` and the filtered Flatpak spawn portal.
The actual directory must be tmpfs. App XDG state and /var/tmp are not RAM.

Negative RAM permission variants use fresh app identities and assert carrier
absence: updating a used app can retain usable private RAM. Launch-IPC refusal
uses `flatpak run --no-session-bus` and verifies socket absence; Flatpak 1.14.6
permits portal calls even without an explicit talk policy. Failure cleanup pins
package process identities, including portal-spawned entries.

These are local verification assets, not store publication or a completed
package-coverage claim. Snap is unverified and blocked on its denied
pidfd_send_signal prerequisite; no Snap adapter or weaker lifetime fallback
is provided here.
