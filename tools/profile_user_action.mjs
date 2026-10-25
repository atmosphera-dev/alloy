// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later

// One owner for fixture gestures and their actions. Callbacks supply the native
// transport; this component knows neither the probe nor the native test driver.
// Priming a renderer gesture is not acknowledgement by the browser's other IPC pipe.
export function createNativeActionSequencer({prime, acknowledge, dispatch}) {
  let tail = Promise.resolve();
  return (target, options) => {
    const operation = tail.then(async () => {
      const ticket = await prime(target);
      await acknowledge(target, ticket);
      return dispatch(target, options, ticket);
    });
    // A refused action remains refused for its caller; it does not strand the
    // queue or turn the next independent action into a replay of the old one.
    tail = operation.then(() => undefined, () => undefined);
    return operation;
  };
}
