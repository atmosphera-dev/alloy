// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef ALLOY_TOOLS_PROFILE_CONTROL_H_
#define ALLOY_TOOLS_PROFILE_CONTROL_H_

#include <stdlib.h>
#include <string.h>

typedef struct profile_control_request {
  char id[33];
  char action[257];
  int browser;
  int complete;
  unsigned executions;
  struct profile_control_request* next;
} profile_control_request;

typedef struct profile_control_transport {
  profile_control_request* requests;
} profile_control_transport;

typedef void (*profile_control_dispatch_fn)(void*, const profile_control_request*);
typedef void (*profile_control_reply_fn)(void*, const profile_control_request*, int);

enum profile_control_result {
  PROFILE_CONTROL_UNRELATED = 0,
  PROFILE_CONTROL_DISPATCHED = 1,
  PROFILE_CONTROL_REPLAYED = 2,
  PROFILE_CONTROL_INVALID = -1,
  PROFILE_CONTROL_CONFLICT = -2,
  PROFILE_CONTROL_NO_MEMORY = -3
};

// A title is a level notification, not a one-shot command. This receiver owns
// consumption before calling user dispatch, including reentrant notification.
static inline int profile_control_receive(
    profile_control_transport* transport, int browser, const char* title,
    profile_control_dispatch_fn dispatch, profile_control_reply_fn reply,
    void* opaque) {
  static const char prefix[] = "ALLOY_PROFILE_PROBE:";
  if (!title || strncmp(title, prefix, sizeof(prefix) - 1))
    return PROFILE_CONTROL_UNRELATED;
  const char* payload = title + sizeof(prefix) - 1;
  if (strlen(payload) < 34 || payload[32] != ':' || browser <= 0)
    return PROFILE_CONTROL_INVALID;
  for (size_t i = 0; i < 32; ++i)
    if (!((payload[i] >= '0' && payload[i] <= '9') ||
          (payload[i] >= 'a' && payload[i] <= 'f')))
      return PROFILE_CONTROL_INVALID;
  const char* action = payload + 33;
  size_t length = strlen(action);
  if (!length || length > 256 || strspn(action,
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") != length)
    return PROFILE_CONTROL_INVALID;
  for (profile_control_request* found = transport->requests; found;
       found = found->next) {
    if (strncmp(found->id, payload, 32)) continue;
    if (found->browser != browser || strcmp(found->action, action))
      return PROFILE_CONTROL_CONFLICT;
    reply(opaque, found, 1);
    return PROFILE_CONTROL_REPLAYED;
  }
  profile_control_request* request =
      (profile_control_request*)calloc(1, sizeof(profile_control_request));
  if (!request) return PROFILE_CONTROL_NO_MEMORY;
  memcpy(request->id, payload, 32);
  memcpy(request->action, action, length);
  request->browser = browser;
  request->next = transport->requests;
  transport->requests = request;
  request->executions = 1;
  dispatch(opaque, request);
  request->complete = 1;
  reply(opaque, request, 0);
  return PROFILE_CONTROL_DISPATCHED;
}

static inline void profile_control_destroy(profile_control_transport* transport) {
  while (transport->requests) {
    profile_control_request* request = transport->requests;
    transport->requests = request->next;
    free(request);
  }
}

#endif
