// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef BRAVE_BROWSER_COSMETIC_FILTERS_PROFILE_COSMETIC_FILTERS_RESOURCES_H_
#define BRAVE_BROWSER_COSMETIC_FILTERS_PROFILE_COSMETIC_FILTERS_RESOURCES_H_
#include "brave/components/cosmetic_filters/common/cosmetic_filters.mojom.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
namespace content {class RenderFrameHost;}
namespace cosmetic_filters {
void BindProfileCosmeticFiltersResources(content::RenderFrameHost&,
    mojo::PendingReceiver<mojom::CosmeticFiltersResources>);
}
#endif
