// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BASE_PROCESS_ALLOY_GUEST_RUNTIME_H_
#define BASE_PROCESS_ALLOY_GUEST_RUNTIME_H_

// Linux-internal inheritance wiring, not an app ABI or serialized identities.
// These descriptors are transferred capabilities. Chromium launch mappings must
// preserve the registration capability; a zygote fork gets a new liveness pair.
#define ALLOY_GUEST_REGISTRATION_FD 196
#define ALLOY_GUEST_LIVENESS_FD 197
#define ALLOY_GUEST_CONTROL_FD 198
#define ALLOY_GUEST_FACTS_FD 199
#define ALLOY_GUEST_RESOURCE_FD 200
#define ALLOY_GUEST_REGISTRATION_MAGIC 0x41475231u
#define ALLOY_GUEST_FACTS_MAGIC 0x41474231u

#ifdef __cplusplus
extern "C" {
#endif

struct alloy_guest_bootstrap_facts {
  unsigned int magic;
  int process;
  unsigned int before_engine;
  unsigned int handles_closed;
  unsigned int inherited_nnp;
  unsigned int owns_browser_control;
};

// Only the static bootstrap uses pre_engine=1. Registration succeeds only after
// the supervisor has obtained the kernel sender identity and opened its pidfd.
int alloy_guest_register_runtime(int pre_engine);

// No-op outside explicitly launched guests. The marker selects the code path,
// not authority: the inherited socket, sealed facts and live supervisor handshake
// establish that. Calls on a forked child register a fresh runtime identity.
void alloy_guest_initialize_runtime(void);
int alloy_guest_is_runtime(void);
// Async-signal-safe child side of a native fork. Raw clone routes call this
// explicitly; pthread_atfork handles ordinary forks. No browser peer endpoint
// survives in a probe/helper/zygote descendant and falsely holds it alive.
void alloy_guest_close_fork_control(void);

#ifdef __cplusplus
}
#endif
#endif  // BASE_PROCESS_ALLOY_GUEST_RUNTIME_H_
