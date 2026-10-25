// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef INCLUDE_V8_ALLOY_CACHE_CONTROL_H_
#define INCLUDE_V8_ALLOY_CACHE_CONTROL_H_

#include "v8-forward.h"
#include "v8config.h"

namespace v8 {

// Internal Alloy engine boundary, not part of the CEF-compatible public API.
// Permanently refuses script/eval/regexp cache admission in this isolate.
// Call on its owning thread before accepting document code. Debugger cache
// enable/disable operations cannot re-enable admission afterward.
V8_EXPORT void AlloyDisableCompilationCaches(Isolate* isolate);

}  // namespace v8

#endif  // INCLUDE_V8_ALLOY_CACHE_CONTROL_H_
