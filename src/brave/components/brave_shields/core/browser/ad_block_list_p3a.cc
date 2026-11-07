// Copyright (c) 2024 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
#include "brave/components/brave_shields/core/browser/ad_block_list_p3a.h"
namespace brave_shields {
AdBlockListP3A::AdBlockListP3A(PrefService*) {}
AdBlockListP3A::~AdBlockListP3A() = default;
void AdBlockListP3A::ReportFilterListUsage() {}
void AdBlockListP3A::OnFilterListCatalogLoaded(
    const std::vector<FilterListCatalogEntry>&, const std::string&) {}
}  // namespace brave_shields
