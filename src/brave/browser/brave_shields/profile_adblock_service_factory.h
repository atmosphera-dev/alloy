// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_BROWSER_BRAVE_SHIELDS_PROFILE_ADBLOCK_SERVICE_FACTORY_H_
#define BRAVE_BROWSER_BRAVE_SHIELDS_PROFILE_ADBLOCK_SERVICE_FACTORY_H_
#include "base/memory/weak_ptr.h"
#include "base/functional/callback.h"
class PrefService;
namespace content {class BrowserContext;}
namespace brave_shields {
class AdBlockService;
// UI-sequence lookup from original browser ownership, never a resource URL or
// an OTR parent. Null means refusal for that declared Alloy context.
AdBlockService* AdBlockServiceForContext(content::BrowserContext&);
PrefService* AdBlockPrefsForContext(content::BrowserContext&);
// Capture at admission; continuations may resolve only this original owner.
class AdBlockServiceLease {
 public:
  explicit AdBlockServiceLease(content::BrowserContext&);
  AdBlockService* Get() const;
  base::RepeatingCallback<AdBlockService*()> Getter() const;
 private:
  base::WeakPtr<content::BrowserContext> context_;
  base::WeakPtr<AdBlockService> service_;
};
}  // namespace brave_shields
#endif
