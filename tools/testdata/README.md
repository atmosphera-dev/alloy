<!-- SPDX-FileCopyrightText: 2026 Alex Indigo -->
<!-- SPDX-License-Identifier: LGPL-3.0-or-later -->
# Embedder input for boundary tests

`alloy-data/` is test-only input, never a build dependency or packaged payload.
Its synthetic rules exercise three tracker scripts and `fbclid` removal. The
catalog shape and default component public key follow Brave's list catalog.
`resources.json` is an empty valid resource set; no scriptlets are needed.

The unmodified `en-US-10-2.bdic` comes from Chromium's
`chromium/deps/hunspell_dictionaries` (English Hunspell 2020.12.07), copied from
`wakaba-lists/data/dictionaries/`. Its full upstream notices and redistribution
terms are in `LICENSES/LicenseRef-Chromium-English-Hunspell.txt` at the repo root.
The source URL is
`https://chromium.googlesource.com/chromium/deps/hunspell_dictionaries/+/refs/heads/main/en-US-10-2.bdic`.
