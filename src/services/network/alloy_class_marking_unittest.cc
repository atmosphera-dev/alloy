// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <set>
#include <string_view>
#include <type_traits>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/address_list.h"
#include "net/base/io_buffer.h"
#include "net/base/network_change_notifier.h"
#include "net/base/sockaddr_storage.h"
#include "net/dns/host_resolver.h"
#include "net/dns/host_resolver_proc.h"
#include "net/dns/host_resolver_system_task.h"
#include "net/dns/mock_host_resolver.h"
#include "net/socket/client_socket_factory.h"
#include "net/socket/tcp_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "services/network/alloy_guard.h"
#include "services/network/network_context.h"
#include "services/network/network_service.h"
#include "services/network/socket_factory.h"
#include "services/network/test/fake_test_cert_verifier_params_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {
constexpr std::array<std::string_view, 10> kOwners = {
    "blink_resource_loader",
    "renderer_initiated_download",
    "service_worker_script_load",
    "websocket_stream",
    "alloy_webtransport_page",
    "alloy_webrtc_manager",
    "favicon_loader",
    "reporting",
    "missing",
    ""};

int Owner(std::string_view name) {
  return name.empty() ? -1 : AlloyGuard::Hash(name);
}
int ExpectedDscp(std::string_view name) {
#if BUILDFLAG(ALLOY_ENABLE_CLASS_MARKING)
  return net::SocketTag::AlloyDscpForAnnotation(Owner(name));
#else
  return 0;
#endif
}

// A real kernel receiver observes the IP header, independently of socket
// getters.
class Receiver {
 public:
  Receiver(bool ipv6, int type)
      : ipv6_(ipv6),
        fd_(socket(ipv6 ? AF_INET6 : AF_INET, type | SOCK_CLOEXEC, 0)) {
    CHECK(fd_.is_valid());
    net::IPEndPoint local(ipv6 ? net::IPAddress::IPv6Localhost()
                               : net::IPAddress::IPv4Localhost(),
                          0);
    net::SockaddrStorage address;
    CHECK(local.ToSockAddr(address.addr(), &address.addr_len));
    CHECK_EQ(0, bind(fd_.get(), address.addr(), address.addr_len));
    CHECK_EQ(0, getsockname(fd_.get(), address.addr(), &address.addr_len));
    CHECK(endpoint_.FromSockAddr(address.addr(), address.addr_len));
    if (type == SOCK_STREAM) {
      CHECK_EQ(0, listen(fd_.get(), 10));
    } else {
      int on = 1;
      CHECK_EQ(
          0, setsockopt(fd_.get(), ipv6 ? IPPROTO_IPV6 : IPPROTO_IP,
                        ipv6 ? IPV6_RECVTCLASS : IP_RECVTOS, &on, sizeof(on)));
      timeval limit{2, 0};
      CHECK_EQ(0, setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &limit,
                             sizeof(limit)));
    }
  }
  const net::IPEndPoint& endpoint() const { return endpoint_; }
  int ReceiveTos() {
    char byte{};
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[256]{};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    EXPECT_EQ(1, recvmsg(fd_.get(), &message, 0));
    for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
      if (!ipv6_ && c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TOS) {
        return *reinterpret_cast<unsigned char*>(CMSG_DATA(c));
      }
      if (ipv6_ && c->cmsg_level == IPPROTO_IPV6 &&
          c->cmsg_type == IPV6_TCLASS) {
        return *reinterpret_cast<int*>(CMSG_DATA(c));
      }
    }
    ADD_FAILURE() << "Receiver did not get the IP traffic class";
    return -1;
  }

 private:
  bool ipv6_;
  base::ScopedFD fd_;
  net::IPEndPoint endpoint_;
};

class AlloyClassMarkingTest : public testing::TestWithParam<bool> {
 protected:
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::MainThreadType::IO};
};

TEST(AlloyClassTableTest, ClassificationAndLocalCodepointsHaveOneSource) {
  EXPECT_EQ(AlloyGuard::Classify(Owner("navigation_url_loader")).traffic_class,
            AlloyGuard::TrafficClass::kPageLoad);
#if BUILDFLAG(ALLOY_ENABLE_CLASS_MARKING)
  std::set<int> values;
  for (auto owner : kOwners) {
    int dscp = ExpectedDscp(owner);
    EXPECT_GT(dscp, 0);
    EXPECT_LT(dscp, 64);
    EXPECT_EQ(dscp & 3, 3);  // Experimental/local pool, not standard QoS.
    EXPECT_TRUE(values.insert(dscp).second);
  }
  EXPECT_EQ(ExpectedDscp("blink_resource_loader"), 3);
  EXPECT_EQ(ExpectedDscp(""), 39);
  EXPECT_EQ(net::SocketTag::AlloyDscpForAnnotation(0), ExpectedDscp(""));
#else
  EXPECT_TRUE(std::is_empty_v<net::SocketTag>);
  for (auto owner : kOwners) {
    EXPECT_EQ(ExpectedDscp(owner), 0);
  }
#endif
}

TEST_P(AlloyClassMarkingTest, FactoryUdpClassesReachWireAndSurviveQosAndEcn) {
  EXPECT_NE(geteuid(), 0u);
  Receiver receiver(GetParam(), SOCK_DGRAM);
  for (auto owner : kOwners) {
    SCOPED_TRACE(owner);
    auto socket =
        net::ClientSocketFactory::GetDefaultFactory()
            ->CreateDatagramClientSocket(net::DatagramSocket::DEFAULT_BIND,
                                         net::handles::kInvalidNetworkHandle,
                                         nullptr, net::NetLogSource());
    // A no-op option update is valid even before the descriptor is opened.
    EXPECT_EQ(socket->SetTos(net::DSCP_NO_CHANGE, net::ECN_NO_CHANGE), net::OK);
    socket->ApplySocketTag(net::SocketTag().WithAlloyAnnotation(Owner(owner)));
    ASSERT_EQ(socket->Connect(receiver.endpoint()), net::OK);
    auto byte = base::MakeRefCounted<net::IOBufferWithSize>(1);
    byte->data()[0] = 'x';
    base::test::TestFuture<int> sent;
    int result = socket->Write(byte.get(), 1, sent.GetCallback(),
                               TRAFFIC_ANNOTATION_FOR_TESTS);
    if (result == net::ERR_IO_PENDING) {
      result = sent.Get();
    }
    ASSERT_EQ(result, 1);
    EXPECT_EQ(receiver.ReceiveTos() >> 2, ExpectedDscp(owner));

    ASSERT_EQ(socket->SetTos(net::DSCP_DEFAULT, net::ECN_ECT1), net::OK);
    base::test::TestFuture<int> sent_again;
    result = socket->Write(byte.get(), 1, sent_again.GetCallback(),
                           TRAFFIC_ANNOTATION_FOR_TESTS);
    if (result == net::ERR_IO_PENDING) {
      result = sent_again.Get();
    }
    ASSERT_EQ(result, 1);
    EXPECT_EQ(receiver.ReceiveTos(), (ExpectedDscp(owner) << 2) | 1);
  }
}

TEST_P(AlloyClassMarkingTest, FactoryTcpRetriesKeepCreatorClass) {
  Receiver receiver(GetParam(), SOCK_STREAM);
  net::IPEndPoint refused(receiver.endpoint().address(), 0);
  auto socket = net::ClientSocketFactory::GetDefaultFactory()
                    ->CreateTransportClientSocket(
                        net::AddressList(std::vector<net::IPEndPoint>{
                            refused, receiver.endpoint()}),
                        net::handles::kInvalidNetworkHandle, nullptr, nullptr,
                        nullptr, net::NetLogSource());
  socket->ApplySocketTag(
      net::SocketTag().WithAlloyAnnotation(Owner("websocket_stream")));
  base::test::TestFuture<int> connected;
  int result = socket->Connect(connected.GetCallback());
  if (result == net::ERR_IO_PENDING) {
    result = connected.Get();
  }
  ASSERT_EQ(result, net::OK);
  auto* tcp = static_cast<net::TCPClientSocket*>(socket.get());
  int tos = 0;
  socklen_t size = sizeof(tos);
  ASSERT_EQ(0, getsockopt(tcp->SocketDescriptorForTesting(),
                          GetParam() ? IPPROTO_IPV6 : IPPROTO_IP,
                          GetParam() ? IPV6_TCLASS : IP_TOS, &tos, &size));
  EXPECT_EQ(tos >> 2, ExpectedDscp("websocket_stream"));
  // A later accounting-only update must not erase the admitted creator.
  socket->ApplySocketTag(net::SocketTag());
  ASSERT_EQ(0, getsockopt(tcp->SocketDescriptorForTesting(),
                          GetParam() ? IPPROTO_IPV6 : IPPROTO_IP,
                          GetParam() ? IPV6_TCLASS : IP_TOS, &tos, &size));
  EXPECT_EQ(tos >> 2, ExpectedDscp("websocket_stream"));
}

TEST_P(AlloyClassMarkingTest, ServiceUdpFactoryUsesRetainedOwnerOnWire) {
  Receiver receiver(GetParam(), SOCK_DGRAM);
  auto context = net::CreateTestURLRequestContextBuilder()->Build();
  auto guard = base::MakeRefCounted<AlloyGuard>();
  SocketFactory factory(nullptr, context.get(), guard);
  auto annotation = net::CreateMutableNetworkTrafficAnnotationTag(
      Owner("blink_resource_loader"));
  mojo::Remote<mojom::UDPSocket> socket;
  factory.CreateUDPSocket(annotation, socket.BindNewPipeAndPassReceiver(), {});
  base::test::TestFuture<int32_t, const std::optional<net::IPEndPoint>&>
      connected;
  socket->Connect(receiver.endpoint(), nullptr, connected.GetCallback());
  ASSERT_EQ(connected.Get<0>(), net::OK);
  const uint8_t byte[] = {'x'};
  base::test::TestFuture<int32_t> sent;
  socket->Send(byte, annotation, sent.GetCallback());
  ASSERT_EQ(sent.Get(), net::OK);
  EXPECT_EQ(receiver.ReceiveTos() >> 2, ExpectedDscp("blink_resource_loader"));
}

INSTANTIATE_TEST_SUITE_P(IpFamilies, AlloyClassMarkingTest, testing::Bool());

// Replace only NetTestSuite's catch-all test proc with the actual libc call.
// The normal manager/system-task path and the Alloy admission hook still run.
class RealSystemProc final : public net::HostResolverProc {
 public:
  RealSystemProc() : HostResolverProc(nullptr, false) {}
  int Resolve(const std::string& host,
              net::AddressFamily family,
              net::HostResolverFlags flags,
              net::AddressList* addresses,
              int* os_error) override {
    return net::SystemHostResolverCall(host, family, flags, addresses, os_error,
                                       net::handles::kInvalidNetworkHandle);
  }

 private:
  ~RealSystemProc() override = default;
};

TEST_F(AlloyClassMarkingTest, ForcedSystemResolverStillReturnsOwnedResult) {
  auto proc = base::MakeRefCounted<RealSystemProc>();
  net::ScopedDefaultHostResolverProc real_system(proc.get());
  auto notifier = net::NetworkChangeNotifier::CreateMockIfNeeded();
  auto service = NetworkService::CreateForTesting();
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  auto params = CreateNetworkContextParamsForTesting();
  params->cert_verifier_params =
      FakeTestCertVerifierParamsFactory::GetCertVerifierParams();
#if BUILDFLAG(ALLOY_ENABLE_GUARD_LOG)
  params->alloy_guard_log =
      base::File(dir.GetPath().AppendASCII("alloy-guard.log"),
                 base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_APPEND);
#endif
  mojo::Remote<mojom::NetworkContext> remote;
  auto context = NetworkContext::CreateForTesting(
      service.get(), remote.BindNewPipeAndPassReceiver(), std::move(params),
      {});
  std::string name =
      base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          "alloy-system-dns-probe");
  if (name.empty()) {
    name = "localhost";
  }
  net::HostResolver::ResolveHostParameters parameters;
  parameters.source = net::HostResolverSource::SYSTEM;
  parameters.dns_query_type = net::DnsQueryType::A;
  parameters.include_canonical_name = true;
  parameters.originating_annotation =
      net::CreateMutableNetworkTrafficAnnotationTag(
          Owner("blink_resource_loader"));
  auto request = context->url_request_context()->host_resolver()->CreateRequest(
      net::HostPortPair(name, 80),
      net::NetworkAnonymizationKey::CreateTransient(),
      net::handles::kInvalidNetworkHandle, net::NetLogWithSource(), parameters);
  base::test::TestFuture<int> done;
  int result = request->Start(done.GetCallback());
  if (result == net::ERR_IO_PENDING) {
    result = done.Get();
  }
  ASSERT_EQ(result, net::OK);
  ASSERT_FALSE(request->GetAddressResults().empty());
  RecordProperty("system_lookup_name", name);
  RecordProperty("system_lookup_address",
                 request->GetAddressResults().front().ToString());
}
}  // namespace
}  // namespace network
