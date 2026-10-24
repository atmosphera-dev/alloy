// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "base/unguessable_token.h"
#include "content/public/test/test_content_browser_client.h"
#include "content/public/test/test_content_client.h"
#include "content/public/test/unittest_test_suite.h"
#include "content/test/content_test_suite.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace {
class NavigationMockBrowserClient : public content::TestContentBrowserClient {
 public:
  bool ConfigureNetworkContextParams(
      content::BrowserContext* context, bool in_memory,
      const base::FilePath& partition,
      network::mojom::NetworkContextParams* params,
      cert_verifier::mojom::CertVerifierCreationParams* verifier) override {
    const bool result = TestContentBrowserClient::ConfigureNetworkContextParams(
        context, in_memory, partition, params, verifier);
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    const auto directory = base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
        "alloy-mock-log-dir");
    CHECK(!directory.empty());
    CHECK(base::CreateDirectory(directory));
    // Native browser sandbox preparation opens this file before forwarding its
    // descriptor to the Network Service, exactly as for an actual embedder.
    params->alloy_guard_log_path = directory.AppendASCII(
        base::UnguessableToken::Create().ToString()+".log");
#endif
    return result;
  }
};

std::unique_ptr<content::UnitTestTestSuite::ContentClients> CreateClients() {
  auto clients = std::make_unique<content::UnitTestTestSuite::ContentClients>();
  clients->content_client = std::make_unique<content::TestContentClient>();
  clients->content_browser_client = std::make_unique<NavigationMockBrowserClient>();
  return clients;
}
}  // namespace

int main(int argc, char** argv) {
  content::UnitTestTestSuite suite(
      new content::ContentTestSuite(argc, argv),
      base::BindRepeating(CreateClients));
  return base::LaunchUnitTests(
      argc, argv,
      base::BindOnce(&content::UnitTestTestSuite::Run, base::Unretained(&suite)));
}
