// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "brave/alloy/guest/providers.h"
#include <utility>
namespace alloy::guest {
Provider MediaProvider(ProviderCall call) { return {std::move(call)}; }
}
