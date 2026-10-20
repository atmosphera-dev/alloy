// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <memory>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/address_list.h"
#include "net/base/alloy_traffic_class.h"
#include "net/base/network_change_notifier.h"
#include "net/dns/address_sorter_posix.h"
#include "net/dns/host_resolver_manager.h"
#include "net/dns/host_resolver_proc.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/socket_test_util.h"
#include "services/network/proxy_auto_config_library.h"
#include "services/proxy_resolver/public/mojom/proxy_resolver.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net {
namespace {
struct ProbeReceipts {
  int creates = 0;
  int connects = 0;
  int writes = 0;
  std::vector<int> owners;
  std::vector<std::string> destinations;
};

// The only data-send entry of DatagramClientSocket fails the test before any
// kernel send/sendto/sendmsg could occur. Native mode still runs real connect()
// and getsockname() underneath, for the independently captured namespace leg.
class NoSendSocket : public MockUDPClientSocket {
 public:
  NoSendSocket(ProbeReceipts& receipts,
               bool native,
               bool async,
               bool private_routes)
      : receipts_(receipts), async_(async), private_routes_(private_routes) {
    if (native) {
      native_ =
          ClientSocketFactory::GetDefaultFactory()->CreateDatagramClientSocket(
              DatagramSocket::DEFAULT_BIND, handles::kInvalidNetworkHandle,
              nullptr, NetLogSource());
    }
  }
  int ConnectAsync(const IPEndPoint& address,
                   CompletionOnceCallback callback) override {
    ++receipts_.connects;
    receipts_.destinations.push_back(address.ToString());
    receipts_.owners.push_back(tag_.alloy_annotation());
    if (native_) {
      return native_->ConnectAsync(address, std::move(callback));
    }
    local_ = IPEndPoint(address.address().IsIPv6()
                            ? *IPAddress::FromIPLiteral("2001:db8::10")
                            : *IPAddress::FromIPLiteral("192.0.2.100"),
                        12345);
    int result = OK;
    if (private_routes_ &&
        (!address.address().IsIPv6() ||
         address.address().ToString() == "2001:4860:4860::8888")) {
      result = ERR_ADDRESS_UNREACHABLE;
    }
    if (async_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(callback), result));
      return ERR_IO_PENDING;
    }
    return result;
  }
  int GetLocalAddress(IPEndPoint* address) const override {
    if (native_) {
      return native_->GetLocalAddress(address);
    }
    *address = local_;
    return OK;
  }
  void ApplySocketTag(const SocketTag& tag) override {
    tag_ = tag;
    if (native_) {
      native_->ApplySocketTag(tag);
    }
  }
  int Write(IOBuffer*,
            int,
            CompletionOnceCallback,
            const NetworkTrafficAnnotationTag&) override {
    ++receipts_.writes;
    ADD_FAILURE() << "Route-selection socket attempted a data send";
    return ERR_UNEXPECTED;
  }

 private:
  ProbeReceipts& receipts_;
  bool async_;
  bool private_routes_;
  IPEndPoint local_;
  SocketTag tag_;
  std::unique_ptr<DatagramClientSocket> native_;
};

class NoSendFactory : public MockClientSocketFactory {
 public:
  explicit NoSendFactory(bool native = false,
                         bool async = false,
                         bool private_routes = false)
      : native_(native), async_(async), private_routes_(private_routes) {}
  std::unique_ptr<DatagramClientSocket> CreateDatagramClientSocket(
      DatagramSocket::BindType,
      handles::NetworkHandle,
      NetLog*,
      const NetLogSource&) override {
    ++receipts.creates;
    return std::make_unique<NoSendSocket>(receipts, native_, async_,
                                          private_routes_);
  }
  ProbeReceipts receipts;

 private:
  bool native_;
  bool async_;
  bool private_routes_;
};

class NoDnsProc final : public HostResolverProc {
 public:
  NoDnsProc() : HostResolverProc(nullptr, false) {}
  int Resolve(const std::string&,
              AddressFamily,
              HostResolverFlags,
              AddressList*,
              int*) override {
    return ERR_NAME_NOT_RESOLVED;
  }

 private:
  ~NoDnsProc() override = default;
};

class ProbeClient : public proxy_resolver::mojom::HostResolverRequestClient {
 public:
  ProbeClient(mojo::PendingReceiver<
                  proxy_resolver::mojom::HostResolverRequestClient> receiver,
              base::OnceCallback<void(int, const IPAddressList&)> done)
      : receiver_(this, std::move(receiver)), done_(std::move(done)) {}
  void ReportResult(int error, const IPAddressList& addresses) override {
    std::move(done_).Run(error, addresses);
  }

 private:
  mojo::Receiver<proxy_resolver::mojom::HostResolverRequestClient> receiver_;
  base::OnceCallback<void(int, const IPAddressList&)> done_;
};
}  // namespace

// Matches the existing HostResolverManager friend-access test pattern. No
// production method or new runtime observer is introduced for testing.
class AlloyRouteProbeTest : public testing::Test {
 protected:
  int GlobalProbe(HostResolverManager& manager,
                  ClientSocketFactory& factory,
                  const IPAddress& destination) {
    base::test::TestFuture<int> done;
    int result = manager.StartGloballyReachableCheck(
        destination, handles::kInvalidNetworkHandle, NetLogWithSource(),
        &factory, done.GetCallback());
    return result == ERR_IO_PENDING ? done.Get() : result;
  }
  void PacProbe(NoSendFactory& factory, network::MyIpAddressImpl::Mode mode) {
    network::MyIpAddressImpl impl(mode);
    impl.SetSocketFactoryForTest(&factory);
    impl.SetHostResolverProcForTest(base::MakeRefCounted<NoDnsProc>());
    base::test::TestFuture<int, const IPAddressList&> done;
    mojo::PendingRemote<proxy_resolver::mojom::HostResolverRequestClient>
        remote;
    ProbeClient client(remote.InitWithNewPipeAndPassReceiver(),
                       done.GetCallback());
    impl.AddRequest(std::move(remote));
    ASSERT_TRUE(done.Wait());
    EXPECT_EQ(done.Get<0>(), OK);
    EXPECT_FALSE(done.Get<1>().empty());
  }
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};
};

TEST_F(AlloyRouteProbeTest, SharedAndPacRouteHelpersNeverSend) {
  auto notifier = NetworkChangeNotifier::CreateMockIfNeeded();
  bool native = base::CommandLine::ForCurrentProcess()->HasSwitch(
      "alloy-route-probes-native");
  for (bool async : {false, true}) {
    NoSendFactory factory(native, async);
    HostResolverManager manager(HostResolver::ManagerOptions(), nullptr,
                                nullptr);
    EXPECT_EQ(
        GlobalProbe(manager, factory, *IPAddress::FromIPLiteral("8.8.8.8")),
        OK);
    if (!native) {
      EXPECT_EQ(GlobalProbe(manager, factory,
                            *IPAddress::FromIPLiteral("2001:4860:4860::8888")),
                OK);
    }
    PacProbe(factory, network::MyIpAddressImpl::Mode::kMyIpAddress);
    PacProbe(factory, network::MyIpAddressImpl::Mode::kMyIpAddressEx);
    EXPECT_GT(factory.receipts.connects, 0);
    EXPECT_EQ(factory.receipts.writes, 0);
    for (int owner : factory.receipts.owners) {
      EXPECT_EQ(owner, -1);
    }
    RecordProperty(async ? "async_probe_connects" : "sync_probe_connects",
                   factory.receipts.connects);
  }
  RecordProperty("native_route_connections", native ? "true" : "false");
}

TEST_F(AlloyRouteProbeTest, PacPrivateRouteFallbackNeverSends) {
  NoSendFactory factory(false, true, true);
  PacProbe(factory, network::MyIpAddressImpl::Mode::kMyIpAddressEx);
  EXPECT_GE(factory.receipts.connects, 6);
  EXPECT_EQ(factory.receipts.writes, 0);
  for (int owner : factory.receipts.owners) {
    EXPECT_EQ(owner, -1);
  }
}

TEST_F(AlloyRouteProbeTest, DnsAddressSorterRetainsActualOwnerWithoutSending) {
  auto notifier = NetworkChangeNotifier::CreateMockIfNeeded();
  NoSendFactory factory(false, true);
  AddressSorterPosix sorter(&factory);
  base::test::TestFuture<bool, std::vector<IPEndPoint>> done;
  constexpr int owner = AlloyAnnotationHash("blink_resource_loader");
  sorter.Sort({IPEndPoint(IPAddress::IPv6Localhost(), 80),
               IPEndPoint(IPAddress::IPv4Localhost(), 80)},
              NetworkAnonymizationKey::CreateTransient(),
              handles::kInvalidNetworkHandle, done.GetCallback(),
              SocketTag().WithAlloyAnnotation(owner));
  EXPECT_TRUE(done.Get<0>());
  EXPECT_EQ(done.Get<1>().size(), 2u);
  ASSERT_GT(factory.receipts.connects, 0);
  EXPECT_EQ(factory.receipts.writes, 0);
  for (int actual : factory.receipts.owners) {
#if BUILDFLAG(ALLOY_ENABLE_CLASS_MARKING)
    EXPECT_EQ(actual, owner);
#else
    EXPECT_EQ(actual, -1);
#endif
  }
}
}  // namespace net
