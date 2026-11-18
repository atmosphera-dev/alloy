// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_ALLOY_GUEST_NAMESPACE_LOCK_FLATPAK_ENTRY_H_
#define BRAVE_ALLOY_GUEST_NAMESPACE_LOCK_FLATPAK_ENTRY_H_
#ifdef __cplusplus
extern "C" {
#endif
// Static bootstrap/package plumbing only; never a caller-selected executable.
int alloy_flatpak_detect(const char* bootstrap);
int alloy_flatpak_preserves(int fd);
int alloy_flatpak_engine_environment(const char* bootstrap, int child);
int alloy_flatpak_prepare_scratch(void);
#ifdef __cplusplus
}
#endif
#endif
