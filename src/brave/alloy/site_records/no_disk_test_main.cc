// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/alloy/storage_policy.h"
#include "base/functional/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "mojo/core/embedder/embedder.h"
#include "net/test/net_test_suite.h"

int main(int argc, char** argv) {
  base::alloy::InitializeStoragePolicy(true);
  NetTestSuite suite(argc, argv);
  mojo::core::Init();
  return base::LaunchUnitTests(
      argc, argv, base::BindOnce(&NetTestSuite::Run, base::Unretained(&suite)));
}
