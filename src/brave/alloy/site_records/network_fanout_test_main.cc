// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/functional/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "content/public/test/unittest_test_suite.h"
#include "content/test/content_test_suite.h"
namespace alloy::site_records {
std::unique_ptr<content::UnitTestTestSuite::ContentClients> CreateFanoutClients();
}
int main(int argc,char** argv) {
  content::UnitTestTestSuite suite(new content::ContentTestSuite(argc,argv),
      base::BindRepeating(&alloy::site_records::CreateFanoutClients));
  return base::LaunchUnitTests(argc,argv,base::BindOnce(
      &content::UnitTestTestSuite::Run,base::Unretained(&suite)));
}
