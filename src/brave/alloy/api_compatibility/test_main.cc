// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/test/launcher/unit_test_launcher.h"
#include "base/test/test_io_thread.h"
#include "base/unguessable_token.h"
#include "cef/libcef/common/chrome/chrome_main_delegate_cef.h"
#include "chrome/browser/chrome_content_browser_client.h"
#include "chrome/common/chrome_content_client.h"
#include "chrome/test/base/chrome_unit_test_suite.h"
#include "chrome/utility/chrome_content_utility_client.h"
#include "content/public/test/unittest_test_suite.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "services/network/alloy_buildflags.h"
#include "services/network/public/mojom/network_context.mojom.h"

namespace {
class TestBrowserClient : public ChromeContentBrowserClient {
 public:
  // The native unit suite supplies its test Network Service.
  void OnNetworkServiceCreated(network::mojom::NetworkService*) override {}

  bool ConfigureNetworkContextParams(
      content::BrowserContext* context, bool in_memory,
      const base::FilePath& partition,
      network::mojom::NetworkContextParams* params,
      cert_verifier::mojom::CertVerifierCreationParams* verifier) override {
    const bool configured =
        ChromeContentBrowserClient::ConfigureNetworkContextParams(
            context, in_memory, partition, params, verifier);
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
    const auto directory =
        base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
            "alloy-test-log-dir");
    CHECK(!directory.empty());
    CHECK(base::CreateDirectory(directory));
    params->alloy_guard_log_path = directory.AppendASCII(
        base::UnguessableToken::Create().ToString() + ".log");
#endif
    return configured;
  }
};

std::unique_ptr<content::UnitTestTestSuite::ContentClients> Clients() {
  auto clients = std::make_unique<content::UnitTestTestSuite::ContentClients>();
  clients->content_client = std::make_unique<ChromeContentClient>();
  clients->content_browser_client = std::make_unique<TestBrowserClient>();
  clients->content_utility_client =
      std::make_unique<ChromeContentUtilityClient>();
  return clients;
}
}  // namespace

int main(int argc, char** argv) {
  content::UnitTestTestSuite suite(new ChromeUnitTestSuite(argc, argv),
                                  base::BindRepeating(Clients));
  // CEF's native delegate installs the app/task-runner managers required by
  // composed Chrome pref registration. Destroy it before the suite tears down
  // the command line that ChromeMainDelegate's destructor reads.
  ChromeMainDelegateCef cef_delegate(/*runner=*/nullptr, /*settings=*/nullptr,
                                     /*application=*/nullptr);
  base::TestIOThread io(base::TestIOThread::kAutoStart);
  mojo::core::ScopedIPCSupport ipc(
      io.task_runner(), mojo::core::ScopedIPCSupport::ShutdownPolicy::FAST);
  return base::LaunchUnitTests(
      argc, argv,
      base::BindOnce(&content::UnitTestTestSuite::Run, base::Unretained(&suite)));
}
