# Pinned Linux build

The same builder-owned roots are also admitted on the on-demand DigitalOcean
guest `alloy-do-build` and provisioned named guests `alloy-<task>`. Task names
are 1-48 lowercase letters, digits or hyphens, start with a letter and end with
a letter or digit. Named guests also require
`/etc/alloy-build-prerequisites-ready`, the marker created by build-image
provisioning: a root-owned regular file, not a symlink, with no group or other
write permission. A matching hostname alone is not sufficient. The existing
`wakaba-build` and `alloy-do-build` hosts remain admitted without that marker.
Each guest's persistent ext4 volume is mounted at
`/home/builder/work`; source and build outputs stay on that volume. Use
`smoke.py --out src/out/Verify_Alloy --local-fixture` there to serve the repo's
small page on loopback instead of accessing the NUC's Kiwix server. The tracker
probe uses only its existing loopback-only namespace and needs no home-network
routes, interfaces or DNS. Durable user units below apply to both machines;
the DigitalOcean guest needs linger enabled for `builder`.
Its Ubuntu image carries Chromium's per-executable AppArmor `userns` allowance
for `cefclient` and `alloy_*` consumers under `/home/builder/work/*/src/out/*/`.
The global unprivileged-userns restriction remains enabled, and the browser
keeps its sandbox.

Run these commands on the dedicated `wakaba-build` NUC, from
`/home/builder/work/alloy`, with this repository delivered to `repo/`.
`pins.json` fixes the source revisions and Node/pnpm versions. Brave's
own frozen pnpm install, no-history init, pinned depot_tools, DEPS and hooks
provision the remaining build tools. The machine needs the distribution build
prerequisites for Brave, plus `libcurl-gnutls` (the bundled Cargo runtime),
Xvfb / `xvfb-run`, and the cefclient runtime libraries.

```sh
export TMPDIR=/home/builder/work/tmp
python3 repo/tools/alloy_checkout.py --root /home/builder/work/alloy/src
python3 repo/tools/alloy_apply.py --root src --report evidence/apply.json
python3 repo/tools/alloy_build.py --config release
python3 repo/tools/smoke.py --out src/out/Release_Alloy
# Verification-only startup canary and denial log:
python3 repo/tools/alloy_build.py --config verify
python3 repo/tools/smoke.py --out src/out/Verify_Alloy
```

Alloy ships no lists or dictionaries. Apply and build consume only this repo
and the pinned engine sources. The embedder passes `--alloy-data-dir` at startup;
see [`../docs/data-directory.md`](../docs/data-directory.md) for the read-only
directory contract. Without the switch the engine starts without this data and
downloads none. `wakaba-lists` is Wakaba's input and is independent of Alloy.

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

## Blacksmith CI

`.github/workflows/build.yml` runs on Blacksmith's 32-vCPU Ubuntu 24.04 runners
for PRs to `main`, pushes to `main`, and manual dispatches. Every run applies
Alloy, builds Verify, checks page loading/CDP against a repo fixture served on
loopback, and runs the blocking probe with `tools/testdata/alloy-data`. Only a
push to `main` also builds and smokes Release. The NUC smoke default remains
the Kiwix page; `smoke.py --local-fixture` selects the loopback fixture.

The sticky disk at `/home/runner/alloy` keeps sources, apply state and outputs.
Its key includes the hash of `pins.json`, so pin changes start fresh; same-pin
checkouts skip initialization and builds are incremental. Blacksmith sticky-disk
branch protection must stay enabled: PRs read the latest trusted snapshot but
cannot save changes. Only default-branch push, dispatch or schedule jobs save
the disk. The job summary records command-step times and disk sizes; no large
artifacts are uploaded. Ubuntu 24.04 gets a per-binary AppArmor user-namespace
profile for the CI cefclient outputs; the Chromium sandbox stays enabled.
The job log records whether the sticky-disk snapshot is saved or discarded.
Provider kernels without active AppArmor must pass a native userns check.
The disposable runner also disables automatic fallback tunnels in new network
namespaces, so the blocking probe retains its exact loopback-only check.

To run CI by hand, select **Build and test on Blacksmith CI** in GitHub Actions
and choose **Run workflow**, or use
`direnv exec ~/Projects/alloy gh workflow run build.yml --ref main`.
On a Blacksmith runner, the
workflow's commands run from `/home/runner/alloy`, with the repo in `repo/` and
`TMPDIR=/home/runner/alloy/tmp`. This third workspace is accepted only with
`GITHUB_ACTIONS=true` and must be owned by the invoking user. NUC, DigitalOcean
and marker-gated named build-host checks are unchanged.

## Offline embedder-data consumers

On the same NUC, build the native spelling observer from this repository against
the current CEF headers/library, beside the release `libcef.so`:

```sh
timeout 70 gcc -std=c11 -D_GNU_SOURCE -DUSING_CEF_SHARED \
  -I /home/builder/work/alloy/src -I /home/builder/work/alloy/src/cef \
  -I /home/builder/work/alloy/src/out/Release_Alloy/gen/cef \
  repo/tools/spell_consumer.c -L /home/builder/work/alloy/src/out/Release_Alloy \
  -Wl,-rpath,/home/builder/work/alloy/src/out/Release_Alloy -lcef \
  -o src/out/Release_Alloy/alloy_spell_consumer
python3 repo/tools/probe_blocking.py --out src/out/Release_Alloy \
  --alloy-data-dir="$PWD/repo/tools/testdata/alloy-data"
python3 repo/tools/probe_blocking.py --out src/out/Release_Alloy \
  --alloy-data-dir="$PWD/repo/tools/testdata/alloy-data" \
  --spell-consumer src/out/Release_Alloy/alloy_spell_consumer
```

Both probes use a loopback-only network namespace, fresh profiles, the actual
QueryFilter parser, tracker failures and a neutral local script.
QueryFilter readiness is observed through a bounded navigation whose actual
URL loses `fbclid` and retains `keep=1`, without depending on product logs.
The probe also accepts `--out src/out/Verify_Alloy`; verification additionally
checks the parser's diagnostic rule count against the supplied JSON. The
native observer additionally reports real CEF accessibility spelling markers.
Its startup URL and profile settings are copied before CEF initialization;
callbacks never retain borrowed environment storage across Chromium startup.
The observer is test-only and writes no instrumentation into the product.

Without `--alloy-data-dir`, the blocking probe checks that tracker requests are
not blocked by lists and `fbclid` stays in the URL. `smoke.py` also defaults to
no data; it accepts the switch when testing an embedder input. The native spell
consumer forwards the supplied command line to CEF unchanged. To check startup
rejection, use `probe_blocking.py --out src/out/Verify_Alloy
--alloy-data-dir=relative-data --expect-startup-failure`, then repeat with an
absolute missing directory. Each must exit nonzero before requesting a page.
Each rejection probe first renders the fixture successfully using the same
executable, Xvfb server and network namespace, without the invalid switch.
`--profile=<absolute evidence path>` retains and reuses a profile for the
with-data then no-switch warm-profile check; both runs require normal exit.

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
Row 53 checks logical shared-proto client namespaces, allowing retained
page-download metadata (client 21), OriginTrials (client 44) and catalog metadata.
It also rejects unused clients' unique fallback directories (`image_cache`,
`Notification Scheduler`, `AutofillAiModelCache`) alongside the other mapped rows.
The fixture waits for file and WebRTC video-capability consumers to complete;
these retain their native unavailable-statistics response without a history DB.
The NUC-native
`leveldb_leveldbutil` executable dumps numeric WAL/SST files read-only after clean
shutdown; token values and origin-bearing key tails are not included in the
namespace report. Build the helper in each output before forbidding row 53:

```sh
python3 repo/tools/alloy_build.py --config release --target leveldb_leveldbutil
python3 repo/tools/alloy_build.py --config verify --target leveldb_leveldbutil
```

Unused clients' standalone fallback directories are checked by their own rows.
The retained OriginTrials selector/migration path is unchanged. Unknown key
formats or native dump errors fail the check rather than imply absence.
Row 71 shares service-worker and Cache Storage databases with retained
features and has no independently identifiable file: its owner removal requires
source verification, not a claim that the shared databases should disappear.
Similarly, the probe's bounded fixture does not activate every optional service
or prove that old files cannot reopen; factory/build coverage is verified
separately. Both product and verification builds use the same removal assertions.

## Native stateless profile probe (NUC only)

After building both configurations on the approved NUC, from
`/home/builder/work/alloy` with `TMPDIR=/home/builder/work/tmp`:

```sh
python3 repo/tools/profile_probe.py --out src/out/Release_Alloy --check stateless
python3 repo/tools/profile_probe.py --out src/out/Verify_Alloy --check stateless
```

The probe builds `profile_consumer.c` against the generated experimental Alloy
C API and runs it in a loopback-only network namespace. It needs the existing
Xvfb, GCC, Node, OpenSSL, GDB, `nm` and NSS `certutil` tools. Each configuration exercises
KEEP and CUT through native browser callbacks, fresh Back/Forward documents,
tab close/reopen, storage refusal, cache refetch, connection/TLS positive
controls and live/post-shutdown filesystem comparisons. CUT is per new
top-level document; a same-document history change retains its generation.

Media identity uses the native fake-device switch (replacing camera and
microphone) and an explicit CEF media-permission callback, without automatic
permission flags. A live fake video track and device enumeration must expose a
stable ID within one document and a different ID in the next. Tracks are stopped
before navigation; no physical capture is used by this fixture.

The consumer first creates only its explicitly persistent parent/bootstrap
browser. `profile_readiness.py`, loaded by GDB, observes returns from native
startup completion, PreferredApps' first write-completion reply, and blob-limit
setup. Those callbacks prove the traced parent startup work has completed; no
sleep or filesystem-quiet interval establishes readiness. The observer neither
calls inferior functions nor changes application state, and its own home/cache
and receipts stay outside browser-owned roots. Native parent paths are checked
after these callbacks as supporting assertions. Only then is `before-paths.json`
taken, before a native control asks the consumer to create the stateless context.
The parent browser remains alive throughout the measured interval. After the
stateless browsers close, their owned C references are released. Native
destructor-body returns for both request contexts, their browser context and
the OTR profile establish the teardown barrier, without a timer-based assumption.
Exact destructor addresses are resolved from the current output's ELF symbols;
GDB's demangled lookup can otherwise conflate destructor variants. The request-
context deleting destructor directly invokes D2, bypassing its D1 tail-jump
wrapper; the observer follows the actual body rather than requiring that wrapper.

Fixture user actions also require a native completion barrier. A DevTools gesture
notification and a file-picker request use separate IPC pipes; evaluating the
picker with `userGesture: true` does not order browser-side activation first.
The shared `profile_user_action.mjs` sequencer primes a gesture, waits for the GDB
observer's completed native activation receipt, then dispatches the action without
another gesture injection. Receipts bind the browser PID, native frame-tree node
and monotone serial; the probe also checks that its document has not changed.
All four open/save/held/replacement actions use this owner. Picker, file-byte,
permission and stale-handle assertions are unchanged.

The native suite imports that same sequencer into a Node transport adapter and
deliberately withholds the real native activation notification. It exercises the
actual `FileSystemAccessManagerImpl`, selected file and Chrome permission context.
Original-order dispatch reaches the native missing-activation denial; acknowledged
dispatch returns the selected native entry/grant. Run this deterministic regression
on the assigned NUC B workspace, from `/home/builder/work/alloy-b`:

```sh
env TMPDIR=/home/builder/work/alloy-b/tmp xvfb-run -a \
  src/out/Release_Alloy/alloy_native_grants_unittests \
  --ozone-platform=x11 --test-launcher-retry-limit=0 \
  --alloy-test-log-dir=/home/builder/work/alloy-b/evidence/gesture-regression/guard \
  --alloy-probe-tools-dir=/home/builder/work/alloy-b/repo/tools \
  --gtest_filter=FixtureGestureTest.NativeAcknowledgementOrdersGestureBeforePicker
```

Use the matching Verify output for its equivalent check. Supply
`--alloy-probe-tools-dir=<selected-workspace>/repo/tools` when running the full
native suite too; it is an explicit test-data input, not a production setting.

Evidence and failed roots are retained under `evidence/profile-stateless-*`.
The isolated test user's external OS/NSS certificate input is created before
either page or no-page run and must remain byte-identical. All raw inventories
include every path. Live and post-shutdown inventories must add **zero** paths
relative to the same process's pre-context inventory, including type changes;
there are no path exclusions or filename transformations in either configuration.
The checked `final-paths.json` is captured after this native stateless destruction
while the persistent parent remains alive. The parent then closes and CEF shuts
down normally; `process-final-paths.json` and its full change report retain those
later parent-lifecycle writes as item-3 evidence. The native inferior's actual
exit status is checked independently of GDB's status.
The duration-matched blank run and its complete raw inventories/change report
remain evidence controls, not the cross-process filesystem pass criterion.

This item-2 check measures what the stateless context writes. Its explicitly
persistent parent's own startup writes (including PreferredApps/blob_storage)
precede that measurement; its close/destructor writes (Session Storage,
Preferences/Secure Preferences, and HttpServerProperties' unconditional
destructor write to Network Persistent State) are reported after it. Item 3's
no-disk/bootstrap check is **absolute zero**
when only stateless/memory contexts exist, not this pre-context baseline.

## Absolute-zero kernel observer (NUC only)

`profile_fs_observer.py` provides a notification-only kernel filesystem collector
for the no-disk fixtures. The controller starts it with `sudo -n`, waits for its
ready receipt **before native exec**, and stops it only after native consumers
and descendants have exited. Closing the observer releases its marks; it changes
no permissions, mount, audit, sysctl or system configuration.

The collector uses `FAN_MARK_FILESYSTEM` with `FAN_CREATE | FAN_MODIFY |
FAN_MOVED_TO | FAN_DELETE | FAN_ATTRIB | FAN_ONDIR` and
`FAN_REPORT_DFID_NAME | FAN_REPORT_FID | FAN_REPORT_PIDFD`. Filesystem marks cover
all mount aliases, including paths outside the CEF/XDG roots. On the dedicated
NUC these include the workspace/root filesystem, `/dev/shm`, `/tmp`, `/run`, the
user runtime tmpfs and devtmpfs. Mount identities and each coverage decision are
recorded. No test workload writes to `/tmp`.

Kernel control interfaces are identified separately from ordinary storage
filesystems. The NUC's unsupported hugetlbfs must have an empty root owned by
root with no builder/ACL-mask write permission; its permission/entry metadata
must remain unchanged. A writable or changed unsupported filesystem fails
coverage. The collector refuses changed mount coverage, queue overflow,
malformed/version-mismatched data, unsupported information records and required
mark failures.

The caller supplies completed native process accounting, including forks and
detached descendants. Any attributed native mutation fails no-disk verification;
an event whose process cannot be attributed also fails. Live identities are
checked against kernel pidfds. Seeded parent files additionally get per-inode
`FAN_OPEN | FAN_ACCESS` marks, so actual parent-state intake fails independently
of the diagnostic syscall decoder. Native pressure controls correlate real spill
files with the kernel's opaque file handles (`name_to_handle_at`).

On the NUC's kernel, `PIDFD_GET_INFO` with `PIDFD_INFO_EXIT` retains a valid
cgroup identity after reaping. This allows attribution to a different,
pre-existing workload even when its `/proc` entry has disappeared. Returned
validity flags are checked; the controller's group and unknown groups do not
qualify. Native task IDs/ancestry always take precedence, so a native mutation
cannot be hidden by a different cgroup value. This identity query changes no
cgroup membership or configuration.

Native stdout/stderr must be pipes: the controller, not the browser process,
owns evidence-file writes. Auxiliary tracers remain identifiable with
`waitid(WNOWAIT)` until their last notifications are drained, then are reaped.
`profile_test_display.py` owns an authenticated Xvfb workload separately from
the controller/browser, so late display helpers have a kernel-identifiable
fixture owner after reaping. It passes DISPLAY/XAUTHORITY through a pipe; the
browser remains in the fixture's loopback-only network namespace. The display
workload shuts down through its owned bridge, including Xvfb's normal cleanup.
There are no log-path or cache-path exclusions. `fanotify-ready.json` preserves
coverage and initial process identities; `fanotify-result.json` preserves raw
events, native mutations, parent reads and attribution failures. Failed runs
retain notifications without a passing attribution verdict.

Strace remains a secondary writer-location diagnostic and process-accounting
source. Its filesystem decoding is not the create/modify pass criterion; no
unresolved syscall is accepted as filesystem evidence. Raw live/final root and
seed-byte inventories, actual consumer readbacks and normal exit are additional
gates. Fanotify does not promise mmap/msync modification notifications; the
inventories and native backing-owner checks remain necessary alongside it.

## Second owned build workspace

Tools select their workspace from the delivered repository's location: either
`/home/builder/work/alloy/repo/` (the original default) or
`/home/builder/work/alloy-b/repo/`. There is no arbitrary-root override. Both
roots must belong to `builder` on an admitted build host (see above); symlinked
workspace directories, foreign roots and the spike checkout are refused before
taking locks or writing.
`--root` and `--out` must select that repository's own source/output tree.
All pin, local-edit, full-apply and publication-journal checks remain in force.

The second workspace owns its `repo/`, `src/`, `apply-state/`, `logs/`,
`evidence/`, `support/` and `tmp/`. Its temporary directory is
`/home/builder/work/alloy-b/tmp`; the original workspace keeps
`/home/builder/work/tmp` for compatibility. For example, on the NUC:

```sh
cd /home/builder/work/alloy-b
export TMPDIR=/home/builder/work/alloy-b/tmp
python3 repo/tools/alloy_apply.py --root src --report evidence/apply.json
python3 repo/tools/alloy_build.py --config release
python3 repo/tools/alloy_build.py --config verify
python3 repo/tools/probe_blocking.py --out src/out/Release_Alloy
```

Use durable units named `alloy-b-…`, with WorkingDirectory, log paths and TMPDIR
all inside `alloy-b`. A source copy must include its matching `apply-state/`;
never hard-link it to another workspace. Regenerate GN outputs in the new root
with `alloy_build.py`, and audit copied symlinks/generated absolute paths before
using the outputs. Do not run checkout init/sync on an already-composed copy.

Offline blocking, worker and inventory probes create anonymous `unshare --net`
namespaces with only loopback and no routes. Their loopback ports are private to
each invocation and do not collide across workspaces. Routed harness operations
use B-only namespace/veth/proxy names and subnets `10.77.3.0/30` and
`10.77.4.0/30`, with the B nft table `alloy_b_harness`. They serialize against
the original lane using `/home/builder/work/alloy/harness-run.lock` and
`/home/builder/work/alloy/controller.lock`; these are the only shared locks.
B's anonymous offline probes also hold both locks around their privileged
namespace lifetime. Lock acquisition waits at most 120 seconds per lock, within
each command's overall timeout, and fails safely if the network owner stays busy.
Standalone protected-smoke runners must hold both shared locks throughout setup,
consumers and cleanup, and set `workspace` in their harness config to
`/home/builder/work/alloy-b`. Source/build/publication locks remain workspace-local.

Workspace contract checks (including original-root compatibility without writing
to the original workspace) run on the NUC:

```sh
timeout 60 env TMPDIR=/home/builder/work/alloy-b/tmp PYTHONDONTWRITEBYTECODE=1 \
  python3 -m unittest discover -s repo/tools/tests -v
```
