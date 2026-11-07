<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Alloy

Alloy is a privacy-first browser engine for embedding in applications. It is
built from [Chromium](https://www.chromium.org/),
[Brave](https://github.com/brave/brave-core) and the
[Chromium Embedded Framework](https://github.com/chromiumembedded/cef) (CEF).

Alloy powers Wakaba, a browser for kids, but is not specific to it: Alloy
provides the mechanisms, and the embedding application chooses the policy.

## What it does

- **Nothing phones home.** A network guard allows only the traffic that pages
  themselves make. Browser background traffic is blocked.
- **No logging in product builds.** A blocked request fails with an error the
  embedder can show at that moment; nothing is recorded. Request logs exist
  only in verification builds, for the test harness.
- **Bundled blocking lists.** Filter lists from `wakaba-lists` ship inside the
  build at a pinned commit. Nothing is downloaded at runtime.
- **Three profile modes** per request context:
  - *stateless*: nothing is stored, not even in memory;
  - *memory-only*: nothing is written to disk;
  - *encrypted*: stored on disk, encrypted, and unlocked through a key
    source (password or OS keyring).
- **Site records** (in progress): all state of a site lives in one encrypted
  record per registrable domain, which the user can see and delete, and which
  decays when unused.

## Status

Early development. Not released; the `Alloy*` API is experimental and will
change.

| # | Area | State |
|---|---|---|
| 1 | Network guard, no-logging product builds, bundled lists | Done |
| 2 | Stateless, memory-only and encrypted contexts; password and OS-keyring key sources | Done |
| 3 | Site records: site settings, embedded-frame state | In progress |
| 4 | Decay, auto-lock, complete API and docs | Planned |

Limits:

- Linux x64 only.
- Off-screen rendering (OSR) and Alloy-style windows only. Chrome-style
  windows and `chrome://settings` are not supported.

Upstream versions are pinned in [`pins.json`](pins.json).

## Repository layout

| # | Path | Contents |
|---|---|---|
| 1 | [`patches/`](patches) | Patches to Chromium, brave-core and CEF; `series.json` sets their order |
| 2 | [`src/`](src) | Alloy's own sources, overlaid onto the upstream checkout |
| 3 | [`tools/`](tools) | Checkout, apply, build and test tools |
| 4 | [`harness/`](harness) | Phone-home regression harness (verification builds only) |
| 5 | [`docs/`](docs) | Design notes |

## Building

See [`tools/README.md`](tools/README.md). The tools currently accept only the
project's own build workspace: `/home/builder/work/alloy` on the host
`wakaba-build`, with this repository delivered to `repo/` and the pinned
`wakaba-lists` commit to `lists/`. From that workspace:

```sh
export TMPDIR=/home/builder/work/tmp
python3 repo/tools/alloy_checkout.py --root /home/builder/work/alloy/src
python3 repo/tools/alloy_apply.py --root src --lists lists --report evidence/apply.json
python3 repo/tools/alloy_build.py --config release   # product build
python3 repo/tools/smoke.py --out src/out/Release_Alloy
python3 repo/tools/alloy_build.py --config verify    # verification build
python3 repo/tools/smoke.py --out src/out/Verify_Alloy
```

A full build takes several hours; later builds are incremental.

## License

Alloy's own material is licensed under
[LGPL-3.0-or-later](LICENSE.md). Patches keep their upstream licenses:
Chromium and CEF under BSD-3-Clause, brave-core under MPL-2.0. See
[`LICENSE.md`](LICENSE.md) and [`REUSE.toml`](REUSE.toml).
