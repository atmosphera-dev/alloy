// SPDX-FileCopyrightText: 2026 Alex Indigo
// SPDX-License-Identifier: LGPL-3.0-or-later
#ifndef SERVICES_NETWORK_ALLOY_TEST_DNS_SERVER_H_
#define SERVICES_NETWORK_ALLOY_TEST_DNS_SERVER_H_

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <stop_token>
#include <thread>
#include <vector>
#include "base/check.h"
#include "base/check_op.h"
#include "net/base/ip_endpoint.h"

namespace network::test {
// A real loopback-only Do53 receiver. Its counters observe packets, not mock
// resolver calls. Records are deliberately limited to A=127.0.0.1 / NODATA.
class AlloyTestDnsServer {
 public:
  AlloyTestDnsServer() {
    fd_ = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK_GE(fd_, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    socklen_t size = sizeof(address);
    CHECK_EQ(getsockname(fd_, reinterpret_cast<sockaddr*>(&address), &size), 0);
    port_ = ntohs(address.sin_port);
    thread_ = std::jthread([this](std::stop_token stop) { Serve(stop); });
  }
  ~AlloyTestDnsServer() {
    thread_.request_stop();
    thread_.join();
    close(fd_);
  }
  net::IPEndPoint endpoint() const {
    return net::IPEndPoint(net::IPAddress::IPv4Localhost(), port_);
  }
  int Queries(std::string name) {
    std::lock_guard lock(mutex_);
    return queries_[name];
  }
 private:
  void Serve(std::stop_token stop) {
    while (!stop.stop_requested()) {
      pollfd poller{fd_, POLLIN, 0};
      if (poll(&poller, 1, 20) <= 0 || !(poller.revents & POLLIN)) continue;
      std::array<uint8_t, 4096> wire{};
      sockaddr_storage peer{};
      socklen_t peer_size = sizeof(peer);
      ssize_t n = recvfrom(fd_, wire.data(), wire.size(), 0,
          reinterpret_cast<sockaddr*>(&peer), &peer_size);
      if (n < 17) continue;
      size_t cursor = 12;
      std::string name;
      bool valid = true;
      while (cursor < static_cast<size_t>(n)) {
        size_t length = wire[cursor++];
        if (!length) break;
        if (length > 63 || cursor + length > static_cast<size_t>(n)) {
          valid = false; break;
        }
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char*>(wire.data() + cursor), length);
        cursor += length;
      }
      if (!valid || cursor + 4 > static_cast<size_t>(n)) continue;
      uint16_t qtype = (wire[cursor] << 8) | wire[cursor + 1];
      size_t question_end = cursor + 4;
      {
        std::lock_guard lock(mutex_);
        ++queries_[name];
      }
      std::vector<uint8_t> reply = {wire[0], wire[1], 0x81, 0x80,
          0, 1, 0, static_cast<uint8_t>(qtype == 1), 0, 0, 0, 0};
      reply.insert(reply.end(), wire.begin() + 12, wire.begin() + question_end);
      if (qtype == 1) {
        const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1,
            0, 0, 0, 60, 0, 4, 127, 0, 0, 1};
        reply.insert(reply.end(), std::begin(answer), std::end(answer));
      }
      CHECK_EQ(sendto(fd_, reply.data(), reply.size(), 0,
          reinterpret_cast<sockaddr*>(&peer), peer_size), static_cast<ssize_t>(reply.size()));
    }
  }
  int fd_ = -1;
  uint16_t port_ = 0;
  std::mutex mutex_;
  std::map<std::string, int> queries_;
  std::jthread thread_;
};
}  // namespace network::test
#endif
