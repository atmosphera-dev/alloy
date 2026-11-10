// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Generated C ABI consumer. No engine headers or internal record filesystem.
#define CEF_API_VERSION CEF_EXPERIMENTAL
#include "include/capi/alloy/alloy_profile_capi.h"
#include "include/capi/cef_app_capi.h"
#include "include/cef_api_hash.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Abort the SDK fixture on a missing boundary outcome, retaining its diagnostic reason.
static void check(int ok, const char* why) {
  if (!ok) { fprintf(stderr, "FAIL records-c %s\n", why); abort(); }
}
/// Convert borrowed UTF-8 to a caller-owned CEF string that the fixture must clear.
static cef_string_t text(const char* value) {
  cef_string_t result = {0};
  check(cef_string_utf8_to_utf16(value, strlen(value), &result), "UTF-8 input");
  return result;
}
// Every refptr argument transfers one counted reference, including callbacks.
#define OWNER(name, type) \
  static struct { type api; atomic_int refs; } name; \
  static void CEF_CALLBACK name##_add(cef_base_ref_counted_t* b) { (void)b; ++name.refs; } \
  static int CEF_CALLBACK name##_release(cef_base_ref_counted_t* b) { (void)b; return --name.refs == 0; } \
  static int CEF_CALLBACK name##_one(cef_base_ref_counted_t* b) { (void)b; return name.refs == 1; } \
  static int CEF_CALLBACK name##_any(cef_base_ref_counted_t* b) { (void)b; return name.refs > 0; }
#define INIT(name) do { \
  name.refs = 1; name.api.base.size = sizeof(name.api); \
  name.api.base.add_ref = name##_add; name.api.base.release = name##_release; \
  name.api.base.has_one_ref = name##_one; name.api.base.has_at_least_one_ref = name##_any; \
} while (0)
#define PASS(name) (name##_add(&name.api.base), &name.api)
OWNER(app, cef_app_t)
OWNER(process, cef_browser_process_handler_t)
OWNER(settings_reply, alloy_site_settings_callback_t)
OWNER(records_reply, alloy_site_records_callback_t)
OWNER(lock_reply, alloy_profile_lock_callback_t)
OWNER(deletion_reply, alloy_record_deletion_callback_t)

static alloy_profile_t* profile;
static alloy_site_record_t* retained;
static int stage;
static cef_string_t site;

/// Queue a generated-C record callback with one transferred counted reference.
static void read_records(void) {
  check(profile->list_site_records(profile, PASS(records_reply)), "list queued");
}
/// Verify lock completion before exercising cancelled enumeration on the retained profile.
static void CEF_CALLBACK locked(alloy_profile_lock_callback_t* self,
                                alloy_site_settings_result_t result) {
  (void)self;
  check(result == ALLOY_SETTINGS_OK, "lock acknowledged");
  check(profile->get_state(profile) == ALLOY_PROFILE_LOCKED, "locked state");
  stage = 3; read_records();
}
/// Check delete-all acknowledgement and immediately enumerate its protected empty catalogue.
static void CEF_CALLBACK deleted(alloy_record_deletion_callback_t* self,
    alloy_site_settings_result_t result, int files_pending) {
  (void)self;
  check(result == ALLOY_SETTINGS_OK && !files_pending, "delete-all acknowledged");
  stage = 2; read_records();
}
/// Drive empty, populated, deleted and locked catalogues while checking snapshot ownership.
static void CEF_CALLBACK listed(alloy_site_records_callback_t* self,
    alloy_site_settings_result_t result, size_t count, alloy_site_record_t* const* records) {
  (void)self;
  if (stage == 0) {
    check(result == ALLOY_SETTINGS_OK && count == 0, "empty catalogue excludes profile shard");
    stage = 1;
    cef_string_list_t embeds = cef_string_list_alloc();
    cef_string_t embedded = text("https://embedded.test");
    cef_string_list_append(embeds, &embedded);
    check(profile->set_first_party_embeds(profile, &site, embeds, PASS(settings_reply)), "setting queued");
    cef_string_clear(&embedded); cef_string_list_free(embeds);
  } else if (stage == 1) {
    check(result == ALLOY_SETTINGS_OK && count == 1, "one site record");
    retained = records[0]; // Take the counted reference transferred by CEF.
    cef_string_userfree_t domain = retained->get_domain(retained);
    cef_string_t expected = text("alpha.test");
    check(domain && cef_string_utf16_cmp(domain, &expected) == 0, "canonical domain");
    check(retained->get_last_visit(retained) == 0, "settings-only record never visited");
    cef_string_userfree_free(domain); cef_string_clear(&expected);
    check(profile->delete_all_site_records(profile, PASS(deletion_reply)), "delete-all queued");
  } else if (stage == 2) {
    check(result == ALLOY_SETTINGS_OK && count == 0, "delete-all completion catalogue empty");
    check(profile->lock_async(profile, PASS(lock_reply)), "UI lock queued");
  } else {
    check(result == ALLOY_SETTINGS_CANCELLED && count == 0, "locked catalogue cancelled");
    // A retained public snapshot is owned plaintext, never a live record grant.
    cef_string_userfree_t domain = retained->get_domain(retained);
    check(domain && domain->length != 0, "snapshot lifetime independent of lock");
    cef_string_userfree_free(domain);
    retained->base.release(&retained->base); retained = NULL;
    cef_quit_message_loop();
  }
}
/// Enumerate only after the settings-only record has been durably published.
static void CEF_CALLBACK published(alloy_site_settings_callback_t* self,
    alloy_site_settings_result_t result, cef_string_list_t embedded) {
  (void)self;
  check(result == ALLOY_SETTINGS_OK && cef_string_list_size(embedded) == 0, "setting published");
  read_records();
}
/// Check UI-thread idle configuration and callback admission after CEF initialization.
static void CEF_CALLBACK initialized(cef_browser_process_handler_t* self) {
  (void)self;
  check(!profile->list_site_records(profile, NULL), "missing callback refused unqueued");
  check(profile->set_auto_lock(profile, 0, NULL), "idle policy configured on UI");
  check(!profile->set_auto_lock(profile, -1, NULL), "negative idle refused");
  check(profile->notify_user_activity(profile), "trusted input accepted");
  read_records();
}
/// Transfer the application-owned browser-process handler reference to CEF.
static cef_browser_process_handler_t* CEF_CALLBACK get_process(cef_app_t* self) {
  (void)self; return PASS(process);
}
/// Run generated-C lifecycle, decay, idle and catalogue contracts with explicit ownership checks.
int main(int argc, char** argv) {
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  check(hash && !strcmp(hash, CEF_API_HASH_PLATFORM), "experimental API hash");
  cef_main_args_t args = {argc, argv};
  int child = cef_execute_process(&args, NULL, NULL);
  if (child >= 0) return child;
  if (argc != 3) return 2;
  cef_string_t path = text(argv[1]), password = text("RECORDS_C_PASSWORD_09");
  profile = alloy_create_profile(&path, &password); check(profile != NULL, "create profile");
  int64_t period = -1; int timebase = -1;
  check(profile->get_decay_settings(profile, &period, &timebase) && period == 0 &&
      timebase == ALLOY_DECAY_SINCE_LAST_VISIT, "new profile decay disabled");
  check(profile->set_decay_settings(profile, 86400, ALLOY_DECAY_SINCE_LAST_VISIT), "decay policy persisted");
  check(!profile->set_decay_settings(profile, -1, ALLOY_DECAY_SINCE_LAST_VISIT), "invalid decay refused");
  check(profile->get_decay_settings(profile, &period, &timebase) && period == 86400,
      "invalid decay preserves policy");
  cef_string_clear(&path); cef_string_clear(&password);
  site = text("https://sub.alpha.test:8443/path");
  INIT(app); INIT(process); INIT(settings_reply); INIT(records_reply); INIT(lock_reply);
  INIT(deletion_reply);
  app.api.get_browser_process_handler = get_process;
  process.api.on_context_initialized = initialized;
  settings_reply.api.on_complete = published;
  records_reply.api.on_complete = listed;
  lock_reply.api.on_complete = locked;
  deletion_reply.api.on_complete = deleted;
  cef_settings_t settings = {0}; settings.size = sizeof(settings);
  settings.windowless_rendering_enabled = 1;
  settings.root_cache_path = text(argv[2]);
  check(cef_initialize(&args, &settings, PASS(app), NULL), "CEF initialization");
  cef_run_message_loop(); cef_shutdown();
  profile->base.release(&profile->base);
  cef_string_clear(&site); cef_string_clear(&settings.root_cache_path);
  check(app.refs == 1 && process.refs == 1 && settings_reply.refs == 1 &&
      records_reply.refs == 1 && lock_reply.refs == 1 && deletion_reply.refs == 1,
      "callback reference ownership");
  puts("PASS profile-records-c-wrapper"); return 0;
}
