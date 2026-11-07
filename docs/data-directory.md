<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Embedder data directory

Alloy ships no filter lists or dictionaries and downloads none. The embedding
application chooses, distributes and updates this policy input independently
of the engine. Wakaba's `wakaba-lists` repository is one such input, not an
Alloy dependency.

Pass `--alloy-data-dir=/absolute/path` on the browser command line before
`CefInitialize`, or append it in `CefApp::OnBeforeCommandLineProcessing` for
the browser process. Validation uses the final browser command line after that
callback. If CEF's `command_line_args_disabled` setting is enabled, supply the
switch in the callback. Subprocesses do not need the switch.

With no switch, Alloy starts without component filter lists, local data,
QueryFilter or a Hunspell dictionary. There is no fallback beside `libcef.so`,
in the profile, or in the working directory. An explicitly empty, relative,
missing, non-directory, unreadable or unsearchable path stops startup with a nonzero result
before any page loads. A supplied directory must stay readable and unchanged
for the process lifetime. Alloy treats its files as read-only application data.

## Layout

Paths below are relative to the supplied directory. The layout and file formats
match the pinned Brave/Chromium providers:

| # | Path | Contract |
|---|---|---|
| 1 | `lists/catalog/list_catalog.json` | Brave catalog array; each entry supplies its UUID, title, default-enabled policy, permission mask and `list_text_component` public key/component ID |
| 2 | `lists/resources/resources.json` | Brave adblock resource array; `[]` is valid when no scriptlets/resources are used |
| 3 | `lists/components/<crx id>/list.txt` | Adblock text for each enabled catalog component, addressed by the CRX ID derived from its public key |
| 4 | `lists/local-data/1/query-filter.json` | QueryFilter rule array with `include`, `exclude` and `params` fields |
| 5 | `lists/local-data/1/` | Other Brave local-data files, such as `debounce.json`, `clean-urls.json`, `clean-urls-permissions.json`, `https-upgrade-exceptions-list.txt`, `request-otr.json` and `webcompat-exceptions.json`, for the corresponding consumers |
| 6 | `dictionaries/<language-version>.bdic` | Chromium Hunspell dictionaries, for example `en-US-10-2.bdic` for the pinned English provider |

Startup validates the directory itself, not each optional payload. Missing or
invalid individual files retain their native provider's unavailable-data
behavior; they never trigger downloads. Only supplied catalog entries are
registered. Dictionary contents are not copied into the profile.
The embedder owns licensing and distribution of its selected data.

## Compiled-filter cache

Without the switch, Alloy does not read Brave's compiled DAT caches, including
in a profile that previously used lists. Existing cache files are left intact.

With a directory, Brave's existing DAT cache behavior is retained. When its
default-enabled `AdblockDATCache` feature is on and the process permits disk
storage, the service reads `adblock_cache/engine0.dat` and `engine1.dat` beneath
the initial profile directory in its user-data root at startup. A nonempty
cache accepted by adblock-rust's deserializer is loaded immediately, before
catalog/resource delivery. Missing
or rejected caches fall back to the filter providers. Successful filter-set
builds write new compiled caches; later provider changes can rebuild them.

The pinned implementation does not bind those cache files to the supplied
directory, component IDs/versions or hashes of `list.txt`. Reusing a user-data
root can therefore reuse an older compiled list even if the input directory
has changed. This switch adds no new cache-validity or invalidation scheme.
Sources: `ad_block_dat_cache_manager.cc` (`MaybeReadCachedDATFiles`),
`ad_block_service.cc` (`OnReadCachedDATFiles`, `OnDATLoaded`, `OnEngineLoaded`),
`ad_block_engine.cc` (`OnDATLoaded`) and `ad_block_filters_provider_manager.cc`
(`MaybeConsumeEngineStartupChangeNotification`) under Brave's shields component.

## Test input

`tools/testdata/alloy-data/` is a small fixture with three synthetic tracker
rules, one `fbclid` QueryFilter rule, an empty resource set and the unmodified
Chromium English dictionary with its upstream license. It is neither a build
dependency nor a production list selection.

On the approved NUC, from `/home/builder/work/alloy`:

```sh
export TMPDIR=/home/builder/work/tmp
python3 repo/tools/probe_blocking.py --out src/out/Verify_Alloy \
  --alloy-data-dir="$PWD/repo/tools/testdata/alloy-data"
# No switch: tracker requests are not blocked and fbclid stays in the URL.
python3 repo/tools/probe_blocking.py --out src/out/Verify_Alloy
# Invalid input: nonzero startup, with no fixture page requested.
python3 repo/tools/probe_blocking.py --out src/out/Verify_Alloy \
  --alloy-data-dir=relative-data --expect-startup-failure
# Warm profile: first populate with lists, then restart without them.
python3 repo/tools/probe_blocking.py --out src/out/Verify_Alloy \
  --alloy-data-dir="$PWD/repo/tools/testdata/alloy-data" \
  --profile="$PWD/evidence/datadir-warm-profile"
python3 repo/tools/probe_blocking.py --out src/out/Verify_Alloy \
  --profile="$PWD/evidence/datadir-warm-profile"
```
