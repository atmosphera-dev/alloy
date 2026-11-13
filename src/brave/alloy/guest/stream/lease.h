// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_STREAM_LEASE_H_
#define BRAVE_ALLOY_GUEST_STREAM_LEASE_H_
#include <functional>
#include "brave/alloy/guest/stream/frame_pool.h"
#include "cef/include/alloy/alloy_guest_stream.h"
namespace alloy::guest::stream {
CefRefPtr<AlloyGuestFrame> ImportFrame(FrameInfo info, base::ScopedFD handle,
                                      std::function<void()> release);
}  // namespace alloy::guest::stream
#endif
