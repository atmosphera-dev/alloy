// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
// Generated native C API consumer. No CefInitialize call in this process.
#define CEF_API_VERSION CEF_EXPERIMENTAL
#include "include/capi/alloy/alloy_guest_session_capi.h"
#include "include/cef_api_hash.h"
#include "include/internal/cef_string.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  alloy_guest_session_handler_t api;
  atomic_int references;
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  int done;
  int active;
  int failed;
} handler_t;
static void CEF_CALLBACK add(cef_base_ref_counted_t* base) {
  atomic_fetch_add(&((handler_t*)base)->references, 1);
}
static int CEF_CALLBACK release(cef_base_ref_counted_t* base) {
  handler_t* handler = (handler_t*)base;
  pthread_mutex_lock(&handler->mutex);
  int old = atomic_fetch_sub(&handler->references, 1);
  pthread_cond_broadcast(&handler->changed);
  pthread_mutex_unlock(&handler->mutex);
  return old == 1;
}
static int CEF_CALLBACK one(cef_base_ref_counted_t* base) {
  return atomic_load(&((handler_t*)base)->references) == 1;
}
static int CEF_CALLBACK any(cef_base_ref_counted_t* base) {
  return atomic_load(&((handler_t*)base)->references) > 0;
}
static cef_string_t string(const char* value) {
  cef_string_t result = {0};
  if (!cef_string_utf8_to_utf16(value, strlen(value), &result)) abort();
  return result;
}
static int equals(const cef_string_t* value, const char* expected) {
  cef_string_utf8_t utf8 = {0};
  if (!value || !cef_string_utf16_to_utf8(value->str, value->length, &utf8)) return 0;
  int result = utf8.length == strlen(expected) && !memcmp(utf8.str, expected, utf8.length);
  cef_string_utf8_clear(&utf8);
  return result;
}
static void CEF_CALLBACK prepared(alloy_guest_session_handler_t* self,
    alloy_guest_session_t* session, alloy_guest_protection_t* protection,
    alloy_guest_capabilities_t* capabilities) {
  handler_t* handler = (handler_t*)self;
  cef_string_userfree_t filesystem = protection->get_filesystem(protection);
  if (!equals(filesystem, "unavailable") || !session->accept(session)) {
    fputs("FAIL prepared-protection-or-accept\n", stderr);
    handler->failed = 1;
  }
  cef_string_userfree_free(filesystem);
  protection->base.release(&protection->base);
  capabilities->base.release(&capabilities->base);
  session->base.release(&session->base);
}
static void CEF_CALLBACK state(alloy_guest_session_handler_t* self,
    alloy_guest_session_t* session, const cef_string_t* state) {
  handler_t* handler = (handler_t*)self;
  cef_string_utf8_t text = {0};
  cef_string_utf16_to_utf8(state->str, state->length, &text);
  fprintf(stderr, "GUEST_C_STATE %.*s\n", (int)text.length, text.str);
  cef_string_utf8_clear(&text);
  if (equals(state, "ACTIVE")) {
    handler->active = 1;
    session->close(session);  // Reentrant on the dedicated callback sequence.
  }
  session->base.release(&session->base);
}
static void CEF_CALLBACK closed(alloy_guest_session_handler_t* self,
    alloy_guest_session_t* session, const cef_string_t* reason) {
  handler_t* handler = (handler_t*)self;
  pthread_mutex_lock(&handler->mutex);
  if (!equals(reason, "closed")) {
    cef_string_utf8_t text = {0};
    cef_string_utf16_to_utf8(reason->str, reason->length, &text);
    fprintf(stderr, "FAIL close-reason %.*s\n", (int)text.length, text.str);
    cef_string_utf8_clear(&text);
    handler->failed = 1;
  }
  handler->done = 1;
  pthread_cond_broadcast(&handler->changed);
  pthread_mutex_unlock(&handler->mutex);
  session->base.release(&session->base);
}
int main(void) {
  const char* hash = cef_api_hash(CEF_API_VERSION, 0);
  if (!hash || strcmp(hash, CEF_API_HASH_PLATFORM)) {
    fputs("FAIL api-hash\n", stderr);
    return 1;
  }
  handler_t handler = {0};
  atomic_init(&handler.references, 1);
  pthread_mutex_init(&handler.mutex, NULL);
  pthread_cond_init(&handler.changed, NULL);
  handler.api.base.size = sizeof(handler.api);
  handler.api.base.add_ref = add;
  handler.api.base.release = release;
  handler.api.base.has_one_ref = one;
  handler.api.base.has_at_least_one_ref = any;
  handler.api.on_prepared = prepared;
  handler.api.on_state_changed = state;
  handler.api.on_closed = closed;
  cef_string_t mode = string("memory"), network = string("keep");
  cef_string_t required = string("unavailable"), data = {0};
  alloy_guest_settings_t* settings = alloy_create_guest_settings(&mode, &network, &required, 1, &data);
  if (!settings) { fputs("FAIL create-settings\n", stderr); return 1; }
  settings->base.add_ref(&settings->base);
  add(&handler.api.base);
  alloy_guest_session_t* session = alloy_start_guest_session(settings, &handler.api);
  settings->base.release(&settings->base);
  cef_string_clear(&mode);
  cef_string_clear(&network);
  cef_string_clear(&required);
  if (!session) { fputs("FAIL start-session\n", stderr); return 1; }
  pthread_mutex_lock(&handler.mutex);
  while (!handler.done || atomic_load(&handler.references) != 1)
    pthread_cond_wait(&handler.changed, &handler.mutex);
  pthread_mutex_unlock(&handler.mutex);
  session->base.release(&session->base);
  int failed = handler.failed || !handler.active;
  if (!handler.active) fputs("FAIL never-active\n", stderr);
  pthread_cond_destroy(&handler.changed);
  pthread_mutex_destroy(&handler.mutex);
  if (!failed) puts("GUEST_C_SDK_PASS");
  return failed;
}
