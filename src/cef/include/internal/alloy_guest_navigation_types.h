// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_INCLUDE_INTERNAL_ALLOY_GUEST_NAVIGATION_TYPES_H_
#define ALLOY_INCLUDE_INTERNAL_ALLOY_GUEST_NAVIGATION_TYPES_H_
#pragma once

// Guest navigation outcomes outside Chromium's native network error range.
// Ordinary app/user cancellation remains ERR_ABORTED (-3).
#define ALLOY_GUEST_NAVIGATION_RENDERER_CRASHED (-10000)

#endif  // ALLOY_INCLUDE_INTERNAL_ALLOY_GUEST_NAVIGATION_TYPES_H_
