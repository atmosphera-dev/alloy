// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_COMMON_ADBLOCK_SOURCE_IDENTITY_H_
#define BRAVE_COMPONENTS_BRAVE_SHIELDS_CORE_COMMON_ADBLOCK_SOURCE_IDENTITY_H_
#include <cstdint>
#include "third_party/rust/cxx/v1/cxx.h"
namespace adblock {
// Called at the parser boundary, including for empty/invalid input streams.
rust::Vec<uint8_t> hash_filter_source(rust::Slice<const uint8_t> previous,
    rust::Slice<const uint8_t> rules, uint8_t permissions);
}
#endif
