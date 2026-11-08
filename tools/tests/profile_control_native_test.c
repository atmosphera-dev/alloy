// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../profile_control.h"

typedef struct fixture {
  profile_control_transport transport;
  unsigned dispatches, replies, replayed, pending;
  int peer;
  int reenter;
  const char* title;
} fixture;

static void dispatch(void* opaque, const profile_control_request* request);
static void reply(void* opaque, const profile_control_request* request, int replayed) {
  fixture* test = opaque;
  ++test->replies;
  test->replayed += replayed != 0;
  test->pending += !request->complete;
  assert(request->browser == 7 && request->executions == 1);
  assert(!strcmp(request->action, "memory-release-peer"));
}
static void dispatch(void* opaque, const profile_control_request* request) {
  fixture* test = opaque;
  assert(!request->complete && request->executions == 1);
  assert(test->peer);  // Same stateful release invariant as the native consumer.
  test->peer = 0;
  ++test->dispatches;
  if (test->reenter)
    assert(profile_control_receive(&test->transport, 7, test->title,
        dispatch, reply, test) == PROFILE_CONTROL_REPLAYED);
}
int main(void) {
  const char* one = "ALLOY_PROFILE_PROBE:00000000000000000000000000000001:memory-release-peer";
  const char* two = "ALLOY_PROFILE_PROBE:00000000000000000000000000000002:memory-release-peer";
  fixture test = {0};
  test.peer = 1;
  assert(profile_control_receive(&test.transport, 7, one, dispatch, reply, &test) == PROFILE_CONTROL_DISPATCHED);
  assert(profile_control_receive(&test.transport, 7, one, dispatch, reply, &test) == PROFILE_CONTROL_REPLAYED);
  assert(test.dispatches == 1 && test.replies == 2 && test.replayed == 1 && test.pending == 0);
  test.peer = 1;
  assert(profile_control_receive(&test.transport, 7, two, dispatch, reply, &test) == PROFILE_CONTROL_DISPATCHED);
  assert(profile_control_receive(&test.transport, 7, one, dispatch, reply, &test) == PROFILE_CONTROL_REPLAYED);
  assert(test.dispatches == 2 && test.replies == 4 && test.replayed == 2);
  assert(profile_control_receive(&test.transport, 8, one, dispatch, reply, &test) == PROFILE_CONTROL_CONFLICT);
  assert(profile_control_receive(&test.transport, 7,
      "ALLOY_PROFILE_PROBE:00000000000000000000000000000001:different-action",
      dispatch, reply, &test) == PROFILE_CONTROL_CONFLICT);
  assert(profile_control_receive(&test.transport, 7, "ordinary page title", dispatch, reply, &test) == PROFILE_CONTROL_UNRELATED);
  assert(profile_control_receive(&test.transport, 7, "ALLOY_PROFILE_PROBE:memory-release-peer", dispatch, reply, &test) == PROFILE_CONTROL_INVALID);
  assert(test.dispatches == 2 && test.replies == 4);
  profile_control_destroy(&test.transport);
  assert(!test.transport.requests);
  fixture nested = {0};
  nested.peer = 1; nested.reenter = 1; nested.title = one;
  assert(profile_control_receive(&nested.transport, 7, one, dispatch, reply, &nested) == PROFILE_CONTROL_DISPATCHED);
  assert(nested.dispatches == 1 && nested.replies == 2 && nested.replayed == 1 && nested.pending == 1);
  profile_control_destroy(&nested.transport);
  puts("PASS native-control-execute-once-replay-fresh-request-conflict-reentrancy-replies");
  return 0;
}
