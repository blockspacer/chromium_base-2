// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains some tests for TCPClientSocket.
// transport_client_socket_unittest.cc contans some other tests that
// are common for TCP and other types of sockets.

#include "net/socket/tcp_client_socket.h"

#include <stddef.h>

#include "base/power_monitor/power_monitor.h"
#include "base/power_monitor/power_monitor_source.h"
#include "base/test/bind_test_util.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/log/net_log_source.h"
#include "net/socket/socket_performance_watcher.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/tcp_server_socket.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/gtest_util.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

// This matches logic in tcp_client_socket.cc. Only used once, but defining it
// in this file instead of just inlining the OS checks where its used makes it
// more grep-able.
#if !defined(OS_ANDROID) && !defined(OS_NACL)
#define TCP_CLIENT_SOCKET_OBSERVES_SUSPEND
#endif

using net::test::IsError;
using net::test::IsOk;
using testing::Not;

namespace base {
class TimeDelta;
}

namespace net {

namespace {

// Test power monitor source that can simulate entering suspend mode. Can't use
// the one in base/ because it insists on bringing its own MessageLoop.
class TestPowerMonitorSource : public base::PowerMonitorSource {
 public:
  TestPowerMonitorSource() = default;
  ~TestPowerMonitorSource() override = default;

  void Suspend() { ProcessPowerEvent(SUSPEND_EVENT); }

  void Resume() { ProcessPowerEvent(RESUME_EVENT); }

  bool IsOnBatteryPowerImpl() override { return false; }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestPowerMonitorSource);
};

class TCPClientSocketTest : public testing::Test {
 public:
  TCPClientSocketTest()
      : task_environment_(base::test::TaskEnvironment::MainThreadType::IO) {
    std::unique_ptr<TestPowerMonitorSource> power_monitor_source =
        std::make_unique<TestPowerMonitorSource>();
    power_monitor_source_ = power_monitor_source.get();
    base::PowerMonitor::Initialize(std::move(power_monitor_source));
  }

  ~TCPClientSocketTest() override { base::PowerMonitor::ShutdownForTesting(); }

  void Suspend() { power_monitor_source_->Suspend(); }
  void Resume() { power_monitor_source_->Resume(); }

  void CreateConnectedSockets(
      std::unique_ptr<StreamSocket>* accepted_socket,
      std::unique_ptr<TCPClientSocket>* client_socket,
      std::unique_ptr<ServerSocket>* server_socket_opt = nullptr) {
    IPAddress local_address = IPAddress::IPv4Localhost();

    std::unique_ptr<TCPServerSocket> server_socket =
        std::make_unique<TCPServerSocket>(nullptr, NetLogSource());
    ASSERT_THAT(server_socket->Listen(IPEndPoint(local_address, 0), 1), IsOk());
    IPEndPoint server_address;
    ASSERT_THAT(server_socket->GetLocalAddress(&server_address), IsOk());

    *client_socket = std::make_unique<TCPClientSocket>(
        AddressList(server_address), nullptr, nullptr, NetLogSource());

    EXPECT_THAT((*client_socket)->Bind(IPEndPoint(local_address, 0)), IsOk());

    IPEndPoint local_address_result;
    EXPECT_THAT((*client_socket)->GetLocalAddress(&local_address_result),
                IsOk());
    EXPECT_EQ(local_address, local_address_result.address());

    TestCompletionCallback connect_callback;
    int connect_result = (*client_socket)->Connect(connect_callback.callback());

    TestCompletionCallback accept_callback;
    int result =
        server_socket->Accept(accepted_socket, accept_callback.callback());
    result = accept_callback.GetResult(result);
    ASSERT_THAT(result, IsOk());

    ASSERT_THAT(connect_callback.GetResult(connect_result), IsOk());

    EXPECT_TRUE((*client_socket)->IsConnected());
    EXPECT_TRUE((*accepted_socket)->IsConnected());
    if (server_socket_opt)
      *server_socket_opt = std::move(server_socket);
  }

 private:
  base::test::TaskEnvironment task_environment_;

  TestPowerMonitorSource* power_monitor_source_;
};

// Try binding a socket to loopback interface and verify that we can
// still connect to a server on the same interface.
TEST_F(TCPClientSocketTest, BindLoopbackToLoopback) {
  IPAddress lo_address = IPAddress::IPv4Localhost();

  TCPServerSocket server(nullptr, NetLogSource());
  ASSERT_THAT(server.Listen(IPEndPoint(lo_address, 0), 1), IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());

  TCPClientSocket socket(AddressList(server_address), nullptr, nullptr,
                         NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(lo_address, 0)), IsOk());

  IPEndPoint local_address_result;
  EXPECT_THAT(socket.GetLocalAddress(&local_address_result), IsOk());
  EXPECT_EQ(lo_address, local_address_result.address());

  TestCompletionCallback connect_callback;
  int connect_result = socket.Connect(connect_callback.callback());

  TestCompletionCallback accept_callback;
  std::unique_ptr<StreamSocket> accepted_socket;
  int result = server.Accept(&accepted_socket, accept_callback.callback());
  result = accept_callback.GetResult(result);
  ASSERT_THAT(result, IsOk());

  EXPECT_THAT(connect_callback.GetResult(connect_result), IsOk());

  EXPECT_TRUE(socket.IsConnected());
  socket.Disconnect();
  EXPECT_FALSE(socket.IsConnected());
  EXPECT_EQ(ERR_SOCKET_NOT_CONNECTED,
            socket.GetLocalAddress(&local_address_result));
}

// Try to bind socket to the loopback interface and connect to an
// external address, verify that connection fails.
TEST_F(TCPClientSocketTest, BindLoopbackToExternal) {
  IPAddress external_ip(72, 14, 213, 105);
  TCPClientSocket socket(AddressList::CreateFromIPAddress(external_ip, 80),
                         nullptr, nullptr, NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(IPAddress::IPv4Localhost(), 0)), IsOk());

  TestCompletionCallback connect_callback;
  int result = socket.Connect(connect_callback.callback());

  // We may get different errors here on different system, but
  // connect() is not expected to succeed.
  EXPECT_THAT(connect_callback.GetResult(result), Not(IsOk()));
}

// Bind a socket to the IPv4 loopback interface and try to connect to
// the IPv6 loopback interface, verify that connection fails.
TEST_F(TCPClientSocketTest, BindLoopbackToIPv6) {
  TCPServerSocket server(nullptr, NetLogSource());
  int listen_result =
      server.Listen(IPEndPoint(IPAddress::IPv6Localhost(), 0), 1);
  if (listen_result != OK) {
    LOG(ERROR) << "Failed to listen on ::1 - probably because IPv6 is disabled."
                  " Skipping the test";
    return;
  }

  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());
  TCPClientSocket socket(AddressList(server_address), nullptr, nullptr,
                         NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(IPAddress::IPv4Localhost(), 0)), IsOk());

  TestCompletionCallback connect_callback;
  int result = socket.Connect(connect_callback.callback());

  EXPECT_THAT(connect_callback.GetResult(result), Not(IsOk()));
}

TEST_F(TCPClientSocketTest, WasEverUsed) {
  IPAddress lo_address = IPAddress::IPv4Localhost();
  TCPServerSocket server(nullptr, NetLogSource());
  ASSERT_THAT(server.Listen(IPEndPoint(lo_address, 0), 1), IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());

  TCPClientSocket socket(AddressList(server_address), nullptr, nullptr,
                         NetLogSource());

  EXPECT_FALSE(socket.WasEverUsed());

  EXPECT_THAT(socket.Bind(IPEndPoint(lo_address, 0)), IsOk());

  // Just connecting the socket should not set WasEverUsed.
  TestCompletionCallback connect_callback;
  int connect_result = socket.Connect(connect_callback.callback());
  EXPECT_FALSE(socket.WasEverUsed());

  TestCompletionCallback accept_callback;
  std::unique_ptr<StreamSocket> accepted_socket;
  int result = server.Accept(&accepted_socket, accept_callback.callback());
  ASSERT_THAT(accept_callback.GetResult(result), IsOk());
  EXPECT_THAT(connect_callback.GetResult(connect_result), IsOk());

  EXPECT_FALSE(socket.WasEverUsed());
  EXPECT_TRUE(socket.IsConnected());

  // Writing some data to the socket _should_ set WasEverUsed.
  const char kRequest[] = "GET / HTTP/1.0";
  auto write_buffer = base::MakeRefCounted<StringIOBuffer>(kRequest);
  TestCompletionCallback write_callback;
  socket.Write(write_buffer.get(), write_buffer->size(),
               write_callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS);
  EXPECT_TRUE(socket.WasEverUsed());
  socket.Disconnect();
  EXPECT_FALSE(socket.IsConnected());

  EXPECT_TRUE(socket.WasEverUsed());

  // Re-use the socket, which should set WasEverUsed to false.
  EXPECT_THAT(socket.Bind(IPEndPoint(lo_address, 0)), IsOk());
  TestCompletionCallback connect_callback2;
  connect_result = socket.Connect(connect_callback2.callback());
  EXPECT_FALSE(socket.WasEverUsed());
}

class TestSocketPerformanceWatcher : public SocketPerformanceWatcher {
 public:
  TestSocketPerformanceWatcher() : connection_changed_count_(0u) {}
  ~TestSocketPerformanceWatcher() override = default;

  bool ShouldNotifyUpdatedRTT() const override { return true; }

  void OnUpdatedRTTAvailable(const base::TimeDelta& rtt) override {}

  void OnConnectionChanged() override { connection_changed_count_++; }

  size_t connection_changed_count() const { return connection_changed_count_; }

 private:
  size_t connection_changed_count_;

  DISALLOW_COPY_AND_ASSIGN(TestSocketPerformanceWatcher);
};

// TestSocketPerformanceWatcher requires kernel support for tcp_info struct, and
// so it is enabled only on certain platforms.
#if defined(TCP_INFO) || defined(OS_LINUX)
#define MAYBE_TestSocketPerformanceWatcher TestSocketPerformanceWatcher
#else
#define MAYBE_TestSocketPerformanceWatcher TestSocketPerformanceWatcher
#endif
// Tests if the socket performance watcher is notified if the same socket is
// used for a different connection.
TEST_F(TCPClientSocketTest, MAYBE_TestSocketPerformanceWatcher) {
  const size_t kNumIPs = 2;
  IPAddressList ip_list;
  for (size_t i = 0; i < kNumIPs; ++i)
    ip_list.push_back(IPAddress(72, 14, 213, i));

  std::unique_ptr<TestSocketPerformanceWatcher> watcher(
      new TestSocketPerformanceWatcher());
  TestSocketPerformanceWatcher* watcher_ptr = watcher.get();

  TCPClientSocket socket(
      AddressList::CreateFromIPAddressList(ip_list, "example.com"),
      std::move(watcher), nullptr, NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(IPAddress::IPv4Localhost(), 0)), IsOk());

  TestCompletionCallback connect_callback;

  ASSERT_NE(OK, connect_callback.GetResult(
                    socket.Connect(connect_callback.callback())));

  EXPECT_EQ(kNumIPs - 1, watcher_ptr->connection_changed_count());
}

// On Android, where socket tagging is supported, verify that
// TCPClientSocket::Tag works as expected.
#if defined(OS_ANDROID)
TEST_F(TCPClientSocketTest, Tag) {
  if (!CanGetTaggedBytes()) {
    DVLOG(0) << "Skipping test - GetTaggedBytes unsupported.";
    return;
  }

  // Start test server.
  EmbeddedTestServer test_server;
  test_server.AddDefaultHandlers(base::FilePath());
  ASSERT_TRUE(test_server.Start());

  AddressList addr_list;
  ASSERT_TRUE(test_server.GetAddressList(&addr_list));
  TCPClientSocket s(addr_list, NULL, NULL, NetLogSource());

  // Verify TCP connect packets are tagged and counted properly.
  int32_t tag_val1 = 0x12345678;
  uint64_t old_traffic = GetTaggedBytes(tag_val1);
  SocketTag tag1(SocketTag::UNSET_UID, tag_val1);
  s.ApplySocketTag(tag1);
  TestCompletionCallback connect_callback;
  int connect_result = s.Connect(connect_callback.callback());
  EXPECT_THAT(connect_callback.GetResult(connect_result), IsOk());
  EXPECT_GT(GetTaggedBytes(tag_val1), old_traffic);

  // Verify socket can be retagged with a new value and the current process's
  // UID.
  int32_t tag_val2 = 0x87654321;
  old_traffic = GetTaggedBytes(tag_val2);
  SocketTag tag2(getuid(), tag_val2);
  s.ApplySocketTag(tag2);
  const char kRequest1[] = "GET / HTTP/1.0";
  scoped_refptr<IOBuffer> write_buffer1 =
      base::MakeRefCounted<StringIOBuffer>(kRequest1);
  TestCompletionCallback write_callback1;
  EXPECT_EQ(s.Write(write_buffer1.get(), strlen(kRequest1),
                    write_callback1.callback(), TRAFFIC_ANNOTATION_FOR_TESTS),
            static_cast<int>(strlen(kRequest1)));
  EXPECT_GT(GetTaggedBytes(tag_val2), old_traffic);

  // Verify socket can be retagged with a new value and the current process's
  // UID.
  old_traffic = GetTaggedBytes(tag_val1);
  s.ApplySocketTag(tag1);
  const char kRequest2[] = "\n\n";
  scoped_refptr<IOBufferWithSize> write_buffer2 =
      base::MakeRefCounted<IOBufferWithSize>(strlen(kRequest2));
  memmove(write_buffer2->data(), kRequest2, strlen(kRequest2));
  TestCompletionCallback write_callback2;
  EXPECT_EQ(s.Write(write_buffer2.get(), strlen(kRequest2),
                    write_callback2.callback(), TRAFFIC_ANNOTATION_FOR_TESTS),
            static_cast<int>(strlen(kRequest2)));
  EXPECT_GT(GetTaggedBytes(tag_val1), old_traffic);

  s.Disconnect();
}

TEST_F(TCPClientSocketTest, TagAfterConnect) {
  if (!CanGetTaggedBytes()) {
    DVLOG(0) << "Skipping test - GetTaggedBytes unsupported.";
    return;
  }

  // Start test server.
  EmbeddedTestServer test_server;
  test_server.AddDefaultHandlers(base::FilePath());
  ASSERT_TRUE(test_server.Start());

  AddressList addr_list;
  ASSERT_TRUE(test_server.GetAddressList(&addr_list));
  TCPClientSocket s(addr_list, NULL, NULL, NetLogSource());

  // Connect socket.
  TestCompletionCallback connect_callback;
  int connect_result = s.Connect(connect_callback.callback());
  EXPECT_THAT(connect_callback.GetResult(connect_result), IsOk());

  // Verify socket can be tagged with a new value and the current process's
  // UID.
  int32_t tag_val2 = 0x87654321;
  uint64_t old_traffic = GetTaggedBytes(tag_val2);
  SocketTag tag2(getuid(), tag_val2);
  s.ApplySocketTag(tag2);
  const char kRequest1[] = "GET / HTTP/1.0";
  scoped_refptr<IOBuffer> write_buffer1 =
      base::MakeRefCounted<StringIOBuffer>(kRequest1);
  TestCompletionCallback write_callback1;
  EXPECT_EQ(s.Write(write_buffer1.get(), strlen(kRequest1),
                    write_callback1.callback(), TRAFFIC_ANNOTATION_FOR_TESTS),
            static_cast<int>(strlen(kRequest1)));
  EXPECT_GT(GetTaggedBytes(tag_val2), old_traffic);

  // Verify socket can be retagged with a new value and the current process's
  // UID.
  int32_t tag_val1 = 0x12345678;
  old_traffic = GetTaggedBytes(tag_val1);
  SocketTag tag1(SocketTag::UNSET_UID, tag_val1);
  s.ApplySocketTag(tag1);
  const char kRequest2[] = "\n\n";
  scoped_refptr<IOBuffer> write_buffer2 =
      base::MakeRefCounted<StringIOBuffer>(kRequest2);
  TestCompletionCallback write_callback2;
  EXPECT_EQ(s.Write(write_buffer2.get(), strlen(kRequest2),
                    write_callback2.callback(), TRAFFIC_ANNOTATION_FOR_TESTS),
            static_cast<int>(strlen(kRequest2)));
  EXPECT_GT(GetTaggedBytes(tag_val1), old_traffic);

  s.Disconnect();
}
#endif  // defined(OS_ANDROID)

// Tests for closing sockets on suspend mode.
#if defined(TCP_CLIENT_SOCKET_OBSERVES_SUSPEND)

// Entering suspend mode shouldn't affect sockets that haven't connected yet, or
// listening server sockets.
TEST_F(TCPClientSocketTest, SuspendBeforeConnect) {
  IPAddress lo_address = IPAddress::IPv4Localhost();

  TCPServerSocket server(nullptr, NetLogSource());
  ASSERT_THAT(server.Listen(IPEndPoint(lo_address, 0), 1), IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());

  TCPClientSocket socket(AddressList(server_address), nullptr, nullptr,
                         NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(lo_address, 0)), IsOk());

  IPEndPoint local_address_result;
  EXPECT_THAT(socket.GetLocalAddress(&local_address_result), IsOk());
  EXPECT_EQ(lo_address, local_address_result.address());

  TestCompletionCallback accept_callback;
  std::unique_ptr<StreamSocket> accepted_socket;
  ASSERT_THAT(server.Accept(&accepted_socket, accept_callback.callback()),
              IsError(ERR_IO_PENDING));

  Suspend();
  // Power notifications happen asynchronously, so have to wait for the socket
  // to be notified of the suspend event.
  base::RunLoop().RunUntilIdle();

  TestCompletionCallback connect_callback;
  int connect_result = socket.Connect(connect_callback.callback());

  ASSERT_THAT(accept_callback.WaitForResult(), IsOk());

  ASSERT_THAT(connect_callback.GetResult(connect_result), IsOk());

  EXPECT_TRUE(socket.IsConnected());
  EXPECT_TRUE(accepted_socket->IsConnected());
}

// TCP socket that hangs when establishing a connection. This is needed to make
// sure establishing a connection doesn't succeed synchronously.
class NeverConnectingTCPClientSocket : public TCPClientSocket {
 public:
  NeverConnectingTCPClientSocket(
      const AddressList& addresses,
      std::unique_ptr<SocketPerformanceWatcher> socket_performance_watcher,
      net::NetLog* net_log,
      const net::NetLogSource& source)
      : TCPClientSocket(addresses,
                        std::move(socket_performance_watcher),
                        net_log,
                        source) {}

 private:
  int ConnectInternal(const IPEndPoint& endpoint) override {
    return ERR_IO_PENDING;
  }
};

TEST_F(TCPClientSocketTest, SuspendDuringConnect) {
  IPAddress lo_address = IPAddress::IPv4Localhost();

  TCPServerSocket server(nullptr, NetLogSource());
  ASSERT_THAT(server.Listen(IPEndPoint(lo_address, 0), 1), IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());

  NeverConnectingTCPClientSocket socket(AddressList(server_address), nullptr,
                                        nullptr, NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(lo_address, 0)), IsOk());

  IPEndPoint local_address_result;
  EXPECT_THAT(socket.GetLocalAddress(&local_address_result), IsOk());
  EXPECT_EQ(lo_address, local_address_result.address());

  TestCompletionCallback connect_callback;
  int rv = socket.Connect(connect_callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  Suspend();
  EXPECT_THAT(connect_callback.WaitForResult(),
              IsError(ERR_NETWORK_IO_SUSPENDED));
}

TEST_F(TCPClientSocketTest, SuspendDuringConnectMultipleAddresses) {
  IPAddress lo_address = IPAddress::IPv4Localhost();

  TCPServerSocket server(nullptr, NetLogSource());
  ASSERT_THAT(server.Listen(IPEndPoint(IPAddress(0, 0, 0, 0), 0), 1), IsOk());
  IPEndPoint server_address;
  ASSERT_THAT(server.GetLocalAddress(&server_address), IsOk());

  AddressList address_list;
  address_list.push_back(
      IPEndPoint(IPAddress(127, 0, 0, 1), server_address.port()));
  address_list.push_back(
      IPEndPoint(IPAddress(127, 0, 0, 2), server_address.port()));
  NeverConnectingTCPClientSocket socket(address_list, nullptr, nullptr,
                                        NetLogSource());

  EXPECT_THAT(socket.Bind(IPEndPoint(lo_address, 0)), IsOk());

  IPEndPoint local_address_result;
  EXPECT_THAT(socket.GetLocalAddress(&local_address_result), IsOk());
  EXPECT_EQ(lo_address, local_address_result.address());

  TestCompletionCallback connect_callback;
  int rv = socket.Connect(connect_callback.callback());
  ASSERT_THAT(rv, IsError(ERR_IO_PENDING));
  Suspend();
  EXPECT_THAT(connect_callback.WaitForResult(),
              IsError(ERR_NETWORK_IO_SUSPENDED));
}

TEST_F(TCPClientSocketTest, SuspendWhileIdle) {
  std::unique_ptr<StreamSocket> accepted_socket;
  std::unique_ptr<TCPClientSocket> client_socket;
  std::unique_ptr<ServerSocket> server_socket;
  CreateConnectedSockets(&accepted_socket, &client_socket, &server_socket);

  Suspend();
  // Power notifications happen asynchronously.
  base::RunLoop().RunUntilIdle();

  scoped_refptr<IOBuffer> buffer = base::MakeRefCounted<IOBuffer>(1);
  buffer->data()[0] = '1';
  TestCompletionCallback callback;
  // Check that the client socket is disconnected, and actions fail with
  // ERR_NETWORK_IO_SUSPENDED.
  EXPECT_FALSE(client_socket->IsConnected());
  EXPECT_THAT(client_socket->Read(buffer.get(), 1, callback.callback()),
              IsError(ERR_NETWORK_IO_SUSPENDED));
  EXPECT_THAT(client_socket->Write(buffer.get(), 1, callback.callback(),
                                   TRAFFIC_ANNOTATION_FOR_TESTS),
              IsError(ERR_NETWORK_IO_SUSPENDED));

  // Check that the accepted socket is disconnected, and actions fail with
  // ERR_NETWORK_IO_SUSPENDED.
  EXPECT_FALSE(accepted_socket->IsConnected());
  EXPECT_THAT(accepted_socket->Read(buffer.get(), 1, callback.callback()),
              IsError(ERR_NETWORK_IO_SUSPENDED));
  EXPECT_THAT(accepted_socket->Write(buffer.get(), 1, callback.callback(),
                                     TRAFFIC_ANNOTATION_FOR_TESTS),
              IsError(ERR_NETWORK_IO_SUSPENDED));

  // Reconnecting the socket should work.
  TestCompletionCallback connect_callback;
  int connect_result = client_socket->Connect(connect_callback.callback());
  accepted_socket.reset();
  TestCompletionCallback accept_callback;
  int accept_result =
      server_socket->Accept(&accepted_socket, accept_callback.callback());
  ASSERT_THAT(accept_callback.GetResult(accept_result), IsOk());
  EXPECT_THAT(connect_callback.GetResult(connect_result), IsOk());
}

TEST_F(TCPClientSocketTest, SuspendDuringRead) {
  std::unique_ptr<StreamSocket> accepted_socket;
  std::unique_ptr<TCPClientSocket> client_socket;
  CreateConnectedSockets(&accepted_socket, &client_socket);

  // Start a read. This shouldn't complete, since the other end of the pipe
  // writes no data.
  scoped_refptr<IOBuffer> read_buffer = base::MakeRefCounted<IOBuffer>(1);
  read_buffer->data()[0] = '1';
  TestCompletionCallback callback;
  ASSERT_THAT(client_socket->Read(read_buffer.get(), 1, callback.callback()),
              IsError(ERR_IO_PENDING));

  // Simulate a suspend event. Can't use a real power event, as it would affect
  // |accepted_socket| as well.
  client_socket->OnSuspend();
  EXPECT_THAT(callback.WaitForResult(), IsError(ERR_NETWORK_IO_SUSPENDED));

  // Check that the client socket really is disconnected.
  EXPECT_FALSE(client_socket->IsConnected());
  EXPECT_THAT(client_socket->Read(read_buffer.get(), 1, callback.callback()),
              IsError(ERR_NETWORK_IO_SUSPENDED));
  EXPECT_THAT(client_socket->Write(read_buffer.get(), 1, callback.callback(),
                                   TRAFFIC_ANNOTATION_FOR_TESTS),
              IsError(ERR_NETWORK_IO_SUSPENDED));
}

TEST_F(TCPClientSocketTest, SuspendDuringWrite) {
  std::unique_ptr<StreamSocket> accepted_socket;
  std::unique_ptr<TCPClientSocket> client_socket;
  CreateConnectedSockets(&accepted_socket, &client_socket);

  // Write to the socket until a write doesn't complete synchronously.
  const int kBufferSize = 4096;
  scoped_refptr<IOBuffer> write_buffer =
      base::MakeRefCounted<IOBuffer>(kBufferSize);
  memset(write_buffer->data(), '1', kBufferSize);
  TestCompletionCallback callback;
  while (true) {
    int rv =
        client_socket->Write(write_buffer.get(), kBufferSize,
                             callback.callback(), TRAFFIC_ANNOTATION_FOR_TESTS);
    if (rv == ERR_IO_PENDING)
      break;
    ASSERT_GT(rv, 0);
  }

  // Simulate a suspend event. Can't use a real power event, as it would affect
  // |accepted_socket| as well.
  client_socket->OnSuspend();
  EXPECT_THAT(callback.WaitForResult(), IsError(ERR_NETWORK_IO_SUSPENDED));

  // Check that the client socket really is disconnected.
  EXPECT_FALSE(client_socket->IsConnected());
  EXPECT_THAT(client_socket->Read(write_buffer.get(), 1, callback.callback()),
              IsError(ERR_NETWORK_IO_SUSPENDED));
  EXPECT_THAT(client_socket->Write(write_buffer.get(), 1, callback.callback(),
                                   TRAFFIC_ANNOTATION_FOR_TESTS),
              IsError(ERR_NETWORK_IO_SUSPENDED));
}

TEST_F(TCPClientSocketTest, SuspendDuringReadAndWrite) {
  enum class ReadCallbackAction {
    kNone,
    kDestroySocket,
    kDisconnectSocket,
    kReconnectSocket,
  };

  for (ReadCallbackAction read_callback_action : {
           ReadCallbackAction::kNone,
           ReadCallbackAction::kDestroySocket,
           ReadCallbackAction::kDisconnectSocket,
           ReadCallbackAction::kReconnectSocket,
       }) {
    std::unique_ptr<StreamSocket> accepted_socket;
    std::unique_ptr<TCPClientSocket> client_socket;
    std::unique_ptr<ServerSocket> server_socket;
    CreateConnectedSockets(&accepted_socket, &client_socket, &server_socket);

    // Start a read. This shouldn't complete, since the other end of the pipe
    // writes no data.
    scoped_refptr<IOBuffer> read_buffer = base::MakeRefCounted<IOBuffer>(1);
    read_buffer->data()[0] = '1';
    TestCompletionCallback read_callback;

    // Used int the ReadCallbackAction::kReconnectSocket case, since can't run a
    // nested message loop in the read callback.
    TestCompletionCallback nested_connect_callback;
    int nested_connect_result;

    CompletionOnceCallback read_completion_once_callback =
        base::BindLambdaForTesting([&](int result) {
          EXPECT_FALSE(client_socket->IsConnected());
          switch (read_callback_action) {
            case ReadCallbackAction::kNone:
              break;
            case ReadCallbackAction::kDestroySocket:
              client_socket.reset();
              break;
            case ReadCallbackAction::kDisconnectSocket:
              client_socket->Disconnect();
              break;
            case ReadCallbackAction::kReconnectSocket: {
              TestCompletionCallback connect_callback;
              nested_connect_result =
                  client_socket->Connect(nested_connect_callback.callback());
              break;
            }
          }
          read_callback.callback().Run(result);
        });
    ASSERT_THAT(client_socket->Read(read_buffer.get(), 1,
                                    std::move(read_completion_once_callback)),
                IsError(ERR_IO_PENDING));

    // Write to the socket until a write doesn't complete synchronously.
    const int kBufferSize = 4096;
    scoped_refptr<IOBuffer> write_buffer =
        base::MakeRefCounted<IOBuffer>(kBufferSize);
    memset(write_buffer->data(), '1', kBufferSize);
    TestCompletionCallback write_callback;
    while (true) {
      int rv = client_socket->Write(write_buffer.get(), kBufferSize,
                                    write_callback.callback(),
                                    TRAFFIC_ANNOTATION_FOR_TESTS);
      if (rv == ERR_IO_PENDING)
        break;
      ASSERT_GT(rv, 0);
    }

    // Simulate a suspend event. Can't use a real power event, as it would
    // affect |accepted_socket| as well.
    client_socket->OnSuspend();
    EXPECT_THAT(read_callback.WaitForResult(),
                IsError(ERR_NETWORK_IO_SUSPENDED));
    if (read_callback_action == ReadCallbackAction::kNone) {
      EXPECT_THAT(write_callback.WaitForResult(),
                  IsError(ERR_NETWORK_IO_SUSPENDED));

      // Check that the client socket really is disconnected.
      EXPECT_FALSE(client_socket->IsConnected());
      EXPECT_THAT(
          client_socket->Read(read_buffer.get(), 1, read_callback.callback()),
          IsError(ERR_NETWORK_IO_SUSPENDED));
      EXPECT_THAT(
          client_socket->Write(write_buffer.get(), 1, write_callback.callback(),
                               TRAFFIC_ANNOTATION_FOR_TESTS),
          IsError(ERR_NETWORK_IO_SUSPENDED));
    } else {
      // Each of the actions taken in the read callback will cancel the pending
      // write callback.
      EXPECT_FALSE(write_callback.have_result());
    }

    if (read_callback_action == ReadCallbackAction::kReconnectSocket) {
      // Finish establishing a connection, just to make sure the reconnect case
      // completely works.
      accepted_socket.reset();
      TestCompletionCallback accept_callback;
      int accept_result =
          server_socket->Accept(&accepted_socket, accept_callback.callback());
      ASSERT_THAT(accept_callback.GetResult(accept_result), IsOk());
      EXPECT_THAT(nested_connect_callback.GetResult(nested_connect_result),
                  IsOk());
    }
  }
}

#endif  // defined(TCP_CLIENT_SOCKET_OBSERVES_SUSPEND)

}  // namespace

}  // namespace net
