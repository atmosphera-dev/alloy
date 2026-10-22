# Pinned Linux build

Run these commands on the dedicated `wakaba-build` NUC, from
`/home/builder/work/alloy`, with this repository delivered to `repo/`.
`pins.json` fixes the source revisions, the `wakaba_lists` data commit and the
Node/pnpm versions. Brave's
own frozen pnpm install, no-history init, pinned depot_tools, DEPS and hooks
provision the remaining build tools. The machine needs the distribution build
prerequisites for Brave, plus `libcurl-gnutls` (the bundled Cargo runtime),
Xvfb / `xvfb-run`, and the cefclient runtime libraries.

```sh
export TMPDIR=/home/builder/work/tmp
python3 repo/tools/alloy_checkout.py --root /home/builder/work/alloy/src
python3 repo/tools/alloy_apply.py --root src --lists lists --report evidence/apply.json
python3 repo/tools/alloy_build.py --config release
python3 repo/tools/smoke.py --out src/out/Release_Alloy
# Verification-only startup canary and denial log:
python3 repo/tools/alloy_build.py --config verify
python3 repo/tools/smoke.py --out src/out/Verify_Alloy
```

Deliver the separate `wakaba-lists` repository at exactly the `wakaba_lists`
commit in `pins.json` to `/home/builder/work/alloy/lists/`, using a checkout or
`git archive` of that commit. Delivery writes the full commit hash followed by a
newline to `lists/wakaba-lists.commit` (outside `data/`). This marker records the
archive's source identity; the delivery must come from the pinned commit.
Full apply refuses a missing/mismatched marker or missing `data/` before
publishing source changes. `--lists` defaults to that NUC `lists/` directory,
including when `prune_origin.py` delegates to the applier. `--cef-only` does not
consume list data.

The list update tools and upstream data live in `wakaba-lists`, not this code
repository. The applier installs `lists/data/**` at `src/brave/alloy/data/`
through the same local-edit-safe publication journal as source overlays.
Builds package the files selected by `data/files.gni`, with their exact bytes,
beside `libcef.so` in `alloy-data/`; they never fetch lists. All assembled
`lists/components/*/list.txt` files are shipped. Their original `lists/sources/`
copies remain in `wakaba-lists` as build inputs and are excluded from packaging.
To refresh release data, run `tools/update_lists.sh` in the lists
repository, review and commit its changes, then bump Alloy's `wakaba_lists` pin
and deliver the new archive and matching marker.

Checkout and full builds take long enough to require a durable owner. For
example, launch the build with a fresh unit/log name and inspect that same unit
until it exits:

```sh
systemd-run --user --unit=alloy-01-build-01 \
  --property=Type=exec --property=RemainAfterExit=yes \
  --property=RuntimeMaxSec=36000 --property=TimeoutStopSec=120 \
  --property=WorkingDirectory=/home/builder/work/alloy \
  --property=StandardOutput=append:/home/builder/work/alloy/logs/build-01.log \
  --property=StandardError=append:/home/builder/work/alloy/logs/build-01.log \
  --setenv=TMPDIR=/home/builder/work/tmp \
  /usr/bin/python3 repo/tools/alloy_build.py
```

The build helper preserves Brave's Python overrides, Rust environment and
offline Siso source redirection through **autoninja**. It prepares CEF's
translated files and generates `out/Release_Alloy` or `out/Verify_Alloy`; it does not rerun the CEF
hook's patcher. Plain Ninja or a ccache command prefix bypasses the spike's
working source-redirection route. The checked-in args reproduce the spike's
nonofficial Static baseline (no services key), GTK/sysroot, zero symbol levels
and original feature imports.

`--config release` (the default) copies `tools/args/release.gn` verbatim to
`args.gn`. `--config verify` copies `tools/args/verify.gn`, which imports its
own copy of the committed product args. The helper also copies the committed
Static import into each output directory; neither configuration imports the
other output's mutable args. Every invocation replaces hand-edited args with
the selected committed configuration. There is no command-line flag overlay.

Product builds compile out the guard log's path creation, sandbox FD opener,
JSON formatting, asynchronous writer and diagnostic fallback. Classification
and denial stay active. Verification enables `alloy_enable_guard_canary` and
`alloy_enable_guard_log`. Socket class marking is added separately in phase 2.
Build and run the guard consumer tests in **both** configurations:

```sh
python3 repo/tools/alloy_build.py --config release --target alloy_guard_unittests
src/out/Release_Alloy/alloy_guard_unittests --test-launcher-retry-limit=0
python3 repo/tools/alloy_build.py --config verify --target alloy_guard_unittests
src/out/Verify_Alloy/alloy_guard_unittests --test-launcher-retry-limit=0
```

Logging assertions run in verification builds; product tests exercise the same
blocking and allowed-page/DNS consumers and assert that no guard file exists.

`alloy_enable_diagnostic_logging` also defaults to false and is enabled only in
verification args. Product Chromium/CEF logging has no file destination and
ignores attempts to enable it through switches or CEF settings; non-fatal
messages have no stderr or trace fallback. Console callbacks remain transient
embedder events, but returning false cannot route them into Chromium logging.
LevelDB's optional diagnostic `LOG` writer shares the build gate; database
contents and write-ahead journals retain their normal behavior.

The same build gate makes file-based NetLog, SSL-key logging, startup/background
tracing, CEF tracing and local crash reporting unavailable in product builds.
`chrome://net-export` is unavailable. `CefBeginTracing` and `CefEndTracing`
return false without writing a supplied or temporary file. Recording owners
select memory backing for CDP trace/resource streams until the embedder
reads/closes them; explicit functional exports retain the file-backed stream
default. File tracing and DevTools media recording requests are rejected.
The external Perfetto system backend and background/field tracing are disabled
in product; the custom backend remains available for memory-only CDP traces.
WebRTC diagnostic event/text/RTP/audio/data recordings
are disabled, while the minimal live connection state for power management is
retained without URLs, configuration, timestamps or diagnostic history.
V8 diagnostic output flags use its native read-only compile-time representation
in product builds, including logfile/profiler/compiler graph/dump paths;
verification retains the upstream mutable diagnostics. Normal optimization
inputs and all public CEF headers/ABI are retained.
Reporting/NEL emits its live observer notifications in product, then drops the
report instead of retaining it for retry/delivery or history queries. Its
endpoint/policy configuration remains functional state. Verification retains
the normal report queue.

Persistent BrowserMetrics histograms (including spare files), UKM/UMA databases,
deferred performance-observer reports/buffers and SCT auditing report queues
also share the diagnostic build gate. Product cannot create or reopen these
recording stores; unsupported operations still complete their callbacks.
Verification retains their original storage behavior. Ordinary web performance
observation and transient embedder events remain usable. Browsing-state stores
such as History, Top Sites, Favicons and Site Characteristics are separate from
this diagnostic-removal work and retain their existing behavior.

`alloy_apply.py --cef-only` measures the uncomposed CEF stage: 117 entries,
86 clean / 21 hunk-offset / 10 conflict. Full apply requires the exported
composition in `patches/series.json` and verifies the reconstructed source bytes.
The CEF patcher runs in a target-only mirror against preserved Brave baseline
bytes. Only changed outputs are published to the live source tree. Baselines,
blobs and the publication journal are outside `src`, in `apply-state/`.
Unchanged reruns touch zero source files. Changed patches are replayed from
the saved baseline and only changed files are replaced; local edits to managed
files cause an error and are preserved. After an interrupted publication,
rerunning resumes only if every file still matches its recorded old or new
bytes. Overlay files under the repository's `src/` are applied after patches.

Full apply validates the original CEF-on-Brave composition before running
`prune_origin.py` in the same baseline mirror. Pruning resolves the pinned
Origin conditions to the stripped feature set, removes Origin's purchase and
policy product, and sets the product name to Alloy. Its per-file changes and
hashes appear in the apply report. Source paths are discovered from the pinned
Brave index so discovery remains stable after deletion. Empty product
directories are removed; unmanaged files are refused and preserved. Patches
whose series entry has `"stage": "after-prune"` run after this transformation,
without changing the validated composition provenance.

`python3 repo/tools/prune_origin.py --root src` publishes through the applier
and audits the result; `--audit` only checks, with a nonzero exit for outstanding
changes. Both use the same transform and never write around apply-state's
ownership or local-edit checks.

Do not run init/sync on a composed tree or remove its apply state. A pin change
needs a fresh owned workspace. The tools deliberately reject the spike tree
and other destination roots.

The smoke test starts a fresh OSR cefclient under Xvfb, checks
`Browser.getVersion`, then observes the Kiwix Internet page's real title node
using `DOM.getDocument`, `DOM.querySelector` and `DOM.getOuterHTML`. Node's
built-in WebSocket supplies CDP without an extra package. Logs and protocol
proof are retained under `logs/smoke-*/`; startup and all protocol waits are
bounded. This proves page loading/CDP, not default filter readiness or a
no-phone-home outcome.

## Offline bundled-data consumers

On the same NUC, build the native spelling observer from this repository against
the current CEF headers/library, beside the release `libcef.so` and `alloy-data/`:

```sh
timeout 70 gcc -std=c11 -D_GNU_SOURCE -DUSING_CEF_SHARED \
  -I /home/builder/work/alloy/src -I /home/builder/work/alloy/src/cef \
  -I /home/builder/work/alloy/src/out/Release_Alloy/gen/cef \
  repo/tools/spell_consumer.c -L /home/builder/work/alloy/src/out/Release_Alloy \
  -Wl,-rpath,/home/builder/work/alloy/src/out/Release_Alloy -lcef \
  -o src/out/Release_Alloy/alloy_spell_consumer
python3 repo/tools/probe_blocking.py --out src/out/Release_Alloy
python3 repo/tools/probe_blocking.py --out src/out/Release_Alloy \
  --spell-consumer src/out/Release_Alloy/alloy_spell_consumer
```

Both probes use a loopback-only network namespace, fresh profiles, the actual
bundled QueryFilter parser, tracker failures and a neutral local script.
QueryFilter readiness is observed through a bounded navigation whose actual
URL loses `fbclid` and retains `keep=1`, without depending on product logs.
The probe also accepts `--out src/out/Verify_Alloy`; verification additionally
checks the parser's diagnostic rule count against the bundled JSON. The
native observer additionally reports real CEF accessibility spelling markers.
Its startup URL and profile settings are copied before CEF initialization;
callbacks never retain borrowed environment storage across Chromium startup.
The observer is test-only and writes no instrumentation into the product.

## Store inventory (NUC only)

```sh
export TMPDIR=/home/builder/work/tmp
python3 repo/tools/store_inventory_probe.py --out src/out/Release_Alloy
python3 repo/tools/store_inventory_probe.py --out src/out/Verify_Alloy --forbid-rows 27,88,93,10
```

The inventory launches a sandboxed OSR cefclient under Xvfb in a loopback-only
network namespace, using fresh persistent roots. Its local fixture exercises
cookies, local/session storage, IndexedDB, Cache Storage, a service worker,
a cross-site frame and an actual downloaded file. CDP observes the fixture and
the downloaded bytes before the tool inventories live roots, closes the browser
normally, waits for all owned children, and inventories again. It preserves all
roots and evidence under `evidence/store-inventory-<id>/`; it never clears a store
to establish absence. `live-paths.json` and `final-paths.json` include every path,
including directories, symlinks and unclassified stores.

`--forbid-rows` accepts comma-separated store-map removal row numbers. It fails
if a selected row has matching native paths (including directories and SQLite
sidecars) in either inventory, and otherwise prints `PASS store-absent <row>`.
Without the option, present stores are reported without failing. Row 10 checks
only session **restore copies**, not the live Session Storage database; row 81
checks discovered Web Data persistence, not explicit search settings. Preference
selectors check nonempty retained activity in shared Preferences files for rows
36/39/40/77/80. Row 78 checks the derived segmentation databases individually;
verification-only `segmentation_platform/ukm_db` diagnostics belong to row 61.
Row 71 shares service-worker and Cache Storage databases with retained
features and has no independently identifiable file: its owner removal requires
source verification, not a claim that the shared databases should disappear.
Similarly, the probe's bounded fixture does not activate every optional service
or prove that old files cannot reopen; factory/build coverage is verified
separately. Both product and verification builds use the same removal assertions.
