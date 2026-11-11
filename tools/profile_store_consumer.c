// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Native consumer of the generated experimental C ABI, before CefInitialize.
#define CEF_API_VERSION CEF_EXPERIMENTAL
#include "include/capi/alloy/alloy_profile_capi.h"
#include "include/cef_api_hash.h"
#include "include/internal/cef_string.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static void check(int value, const char* label) {
  if (!value) { fprintf(stderr, "FAIL %s\n", label); ++failures; }
}
static cef_string_t string(const char* value) {
  cef_string_t result = {0};
  check(cef_string_utf8_to_utf16(value, strlen(value), &result), "utf8");
  return result;
}
static void drop(alloy_profile_t* profile) {
  if (profile) profile->base.release(&profile->base);
}
typedef struct {
  alloy_key_source_t api;
  int references;
  int calls;
  int wrong;
  int cancel;
  alloy_profile_t* profile;
} source_t;
static void CEF_CALLBACK source_add(cef_base_ref_counted_t* base) { ++((source_t*)base)->references; }
static int CEF_CALLBACK source_release(cef_base_ref_counted_t* base) { return --((source_t*)base)->references == 0; }
static int CEF_CALLBACK source_one(cef_base_ref_counted_t* base) { return ((source_t*)base)->references == 1; }
static int CEF_CALLBACK source_any(cef_base_ref_counted_t* base) { return ((source_t*)base)->references > 0; }
static alloy_key_source_t* pass_source(source_t* source) {
  // Transfer one counted reference per refptr argument, retaining the caller's.
  source->api.base.add_ref(&source->api.base); return &source->api;
}
static int CEF_CALLBACK derive(alloy_key_source_t* api, const void* salt,
                               size_t salt_size, void* key, size_t key_size) {
  source_t* source = (source_t*)api; ++source->calls;
  check(salt_size == 32 && key_size == 32 && salt && key, "source-buffers");
  if (salt_size != 32 || key_size != 32 || !salt || !key) return 0;
  if (source->profile) {
    check(source->profile->get_state(source->profile) == ALLOY_PROFILE_TRANSITION, "source-reentrant-state");
    if (source->cancel) check(!source->profile->lock(source->profile), "source-cancel-not-acknowledged");
  }
  const uint8_t* input = (const uint8_t*)salt; uint8_t* output = (uint8_t*)key;
  for (size_t i = 0; i < 32; ++i) output[i] = input[i] ^ (source->wrong ? 0x6d : 0x93);
  return 1;
}
int main(int argc, char** argv) {
  if (argc != 2 && argc != 3 && argc != 4) return 2;
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  check(hash && strcmp(hash, CEF_API_HASH_PLATFORM) == 0, "experimental-api-hash");
  if (failures) return 1;
  if (argc == 4) {
    if (strcmp(argv[1], "--keyring")) return 2;
    cef_string_t path = string(argv[3]);
    int result = ALLOY_KEYRING_FAILED;
    if (!strcmp(argv[2], "create-locked") || !strcmp(argv[2], "create-unavailable")) {
      alloy_profile_t* refused = alloy_create_profile_with_keyring(&path, &result);
      check(refused == NULL, "keyring-create-refused");
      check(result == (!strcmp(argv[2], "create-locked") ? ALLOY_KEYRING_LOCKED :
                        ALLOY_KEYRING_UNAVAILABLE), "keyring-create-error-result");
      drop(refused); cef_string_clear(&path);
      if (failures) return 1;
      printf("PASS profile-api-keyring-%s\n", argv[2]); return 0;
    }
    const int create = !strcmp(argv[2], "create");
    alloy_profile_t* profile = create ? alloy_create_profile_with_keyring(&path, &result)
                                     : alloy_open_profile(&path);
    check(profile != NULL, "keyring-open-create"); if (!profile) return 1;
    if (create) {
      check(result == ALLOY_KEYRING_OK, "keyring-created-result");
      check(profile->lock(profile), "keyring-created-lock");
    }
    result = profile->unlock_with_keyring(profile);
    const int expected = !strcmp(argv[2], "missing") ? ALLOY_KEYRING_MISSING :
        !strcmp(argv[2], "locked") ? ALLOY_KEYRING_LOCKED :
        !strcmp(argv[2], "unavailable") ? ALLOY_KEYRING_UNAVAILABLE : ALLOY_KEYRING_OK;
    check(result == expected, "keyring-result");
    check(profile->get_state(profile) == (expected == ALLOY_KEYRING_OK ?
        ALLOY_PROFILE_UNLOCKED : ALLOY_PROFILE_LOCKED), "keyring-state");
    check(profile->lock(profile), "keyring-lock"); drop(profile); cef_string_clear(&path);
    if (failures) return 1;
    printf("PASS profile-api-keyring-%s\n", argv[2]); return 0;
  }
  if (argc == 3) {
    if (strcmp(argv[1], "--corrupt") != 0) return 2;
    cef_string_t path = string(argv[2]), password = string("PUBLIC_API_PASSWORD_23fc75a9");
    alloy_profile_t* profile = alloy_open_profile(&path);
    check(profile != NULL, "corrupt-profile-bootstrap-open"); if (!profile) return 1;
    check(!profile->unlock_with_password(profile, &password), "corrupt-root-refused");
    check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "corrupt-root-stays-locked");
    drop(profile); cef_string_clear(&path); cef_string_clear(&password);
    if (failures) return 1; puts("PASS profile-api-corruption"); return 0;
  }
  char path_buffer[4096];
  if (snprintf(path_buffer, sizeof(path_buffer), "%s/password-profile", argv[1]) >= (int)sizeof(path_buffer)) return 2;
  cef_string_t path = string(path_buffer), password = string("PUBLIC_API_PASSWORD_23fc75a9");
  cef_string_t wrong = string("WRONG_API_PASSWORD_5a2e");
  alloy_profile_t* profile = alloy_create_profile(&path, &password);
  check(profile != NULL, "create-password"); if (!profile) return 1;
  check(profile->get_state(profile) == ALLOY_PROFILE_UNLOCKED, "created-unlocked");
  int64_t decay_period = -1;
  int decay_clock = -1;
  check(profile->get_decay_settings(profile, &decay_period, &decay_clock) && decay_period == 0 &&
        decay_clock == ALLOY_DECAY_SINCE_LAST_VISIT, "decay-no-default");
  check(profile->set_decay_settings(profile, 6, ALLOY_DECAY_SINCE_LAST_VISIT), "decay-durable-set");
  check(!profile->set_decay_settings(profile, -1, ALLOY_DECAY_SINCE_LAST_VISIT), "decay-negative-refused");
  check(!profile->set_decay_settings(profile, INT64_MAX, ALLOY_DECAY_SINCE_LAST_VISIT), "decay-overflow-refused");
  check(profile->lock(profile), "password-lock");
  decay_period = 77; decay_clock = 88;
  check(!profile->get_decay_settings(profile, &decay_period, &decay_clock) && decay_period == 77 && decay_clock == 88,
        "locked-decay-no-read-or-output-change");
  check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "password-locked");
  check(!profile->unlock_with_password(profile, &wrong), "wrong-password-refused");
  check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "wrong-password-stays-locked"); drop(profile);
  profile = alloy_open_profile(&path); check(profile != NULL, "open-existing-profile"); if (!profile) return 1;
  check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "reopen-locked");
  check(profile->unlock_with_password(profile, &password), "password-unlock-after-reopen");
  check(profile->get_state(profile) == ALLOY_PROFILE_UNLOCKED, "password-unlocked");
  check(profile->get_decay_settings(profile, &decay_period, &decay_clock) && decay_period == 6 &&
        decay_clock == ALLOY_DECAY_SINCE_LAST_VISIT, "decay-cold-policy");
  check(profile->set_decay_settings(profile, 0, ALLOY_DECAY_SINCE_LAST_VISIT), "decay-disable");
  check(profile->lock(profile), "password-lock-after-reopen"); drop(profile);
  alloy_profile_t* duplicate = alloy_create_profile(&path, &password);
  check(duplicate == NULL, "existing-path-refused"); drop(duplicate);
  cef_string_clear(&path); cef_string_clear(&password); cef_string_clear(&wrong);
  if (failures) return 1; puts("PASS profile-api-password");
  if (snprintf(path_buffer, sizeof(path_buffer), "%s/external-profile", argv[1]) >= (int)sizeof(path_buffer)) return 2;
  path = string(path_buffer); source_t source = {0}; source.references = 1;
  source.api.base.size = sizeof(source.api); source.api.base.add_ref = source_add;
  source.api.base.release = source_release; source.api.base.has_one_ref = source_one;
  source.api.base.has_at_least_one_ref = source_any; source.api.derive_key = derive;
  profile = alloy_create_profile_with_key_source(&path, pass_source(&source));
  check(profile != NULL, "create-external-source"); if (!profile) return 1;
  check(source.calls == 1 && source.references == 1, "source-not-retained-after-create");
  check(profile->lock(profile), "external-lock"); source.profile = profile; source.cancel = 1;
  check(!profile->unlock_with_key_source(profile, pass_source(&source)), "external-cancel-unlock");
  check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "cancel-completed-locked"); source.cancel = 0;
  check(profile->unlock_with_key_source(profile, pass_source(&source)), "external-unlock");
  check(profile->get_state(profile) == ALLOY_PROFILE_UNLOCKED, "external-unlocked");
  check(profile->lock(profile), "external-lock-again"); source.wrong = 1;
  check(!profile->unlock_with_key_source(profile, pass_source(&source)), "wrong-external-key-refused");
  check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "wrong-external-key-locked");
  source.wrong = 0; source.profile = NULL; drop(profile);
  profile = alloy_open_profile(&path); check(profile != NULL, "external-reopen"); if (!profile) return 1;
  source.profile = profile;
  check(profile->unlock_with_key_source(profile, pass_source(&source)), "external-reopen-unlock");
  drop(profile); source.profile = NULL;
  profile = alloy_open_profile(&path); check(profile != NULL, "last-reference-locks");
  if (profile) check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "last-reference-wiped-state"); drop(profile);
  check(source.references == 1, "source-not-retained-after-unlock");
  check(source.api.base.release(&source.api.base), "source-final-release"); cef_string_clear(&path);
  if (failures) return 1;
  puts("PASS profile-api-external"); puts("PASS profile-api-cancel"); puts("PASS profile-api-refcounts"); return 0;
}
