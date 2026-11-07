// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef COMPONENTS_KEYED_SERVICE_CORE_SERVICE_CREATION_GATE_H_
#define COMPONENTS_KEYED_SERVICE_CORE_SERVICE_CREATION_GATE_H_

#include "base/functional/callback.h"
#include "base/supports_user_data.h"
#include "components/keyed_service/core/keyed_service_export.h"

// Immutable lifecycle input supplied by the owner of a native context/key.
// Factories do not know the consumer, its keys, or how admission is decided.
// It gates new construction; already-created services remain available for
// in-flight native construction/teardown, whose external operations are gated
// by their original owners separately.
class KEYED_SERVICE_EXPORT ServiceCreationGate final : public base::SupportsUserData::Data {
 public:
  using Callback = base::RepeatingCallback<bool()>;
  static void Attach(base::SupportsUserData& context, Callback callback);
  static bool Allows(const base::SupportsUserData& context);
  static bool Allows(const Callback& callback);
 private:
  explicit ServiceCreationGate(Callback callback);
  const Callback callback_;
};
#endif
