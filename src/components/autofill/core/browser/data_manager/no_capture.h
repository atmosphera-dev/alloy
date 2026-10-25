// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef COMPONENTS_AUTOFILL_CORE_BROWSER_DATA_MANAGER_NO_CAPTURE_H_
#define COMPONENTS_AUTOFILL_CORE_BROWSER_DATA_MANAGER_NO_CAPTURE_H_

namespace autofill {

// Constructs the native, ready-but-empty saved-data interface. No services,
// preferences, databases, data caches or input queues are bound in this form.
// The same facade is used by every Alloy context for item-R removed stores.
struct NoCapture {};

}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_BROWSER_DATA_MANAGER_NO_CAPTURE_H_
