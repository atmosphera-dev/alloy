// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "components/keyed_service/core/service_creation_gate.h"

#include <memory>
#include <utility>

#include "base/check.h"

namespace {
const char kServiceCreationGate = 0;
}
ServiceCreationGate::ServiceCreationGate(Callback callback) : callback_(std::move(callback)) {}
void ServiceCreationGate::Attach(base::SupportsUserData& context, Callback callback) {
  CHECK(callback); CHECK(!context.GetUserData(&kServiceCreationGate));
  context.SetUserData(&kServiceCreationGate,
      std::unique_ptr<ServiceCreationGate>(new ServiceCreationGate(std::move(callback))));
}
bool ServiceCreationGate::Allows(const base::SupportsUserData& context) {
  const auto* gate = static_cast<const ServiceCreationGate*>(context.GetUserData(&kServiceCreationGate));
  return !gate || Allows(gate->callback_);
}
bool ServiceCreationGate::Allows(const Callback& callback) {
  return !callback || callback.Run();
}
