/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Regression tests for thread-safe events and shared resource lifecycle.
 */

#include <chrono>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <gtest/gtest.h>

#include "src/thread_safe.h"

using namespace std::chrono_literals;

TEST(ThreadSafeEventTest, TryPopReturnsImmediatelyAndConsumesValue) {
  safe::event_t<int> event;

  EXPECT_FALSE(event.try_pop());

  event.raise(42);
  auto value = event.try_pop();
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 42);

  EXPECT_FALSE(event.try_pop());
}

TEST(ThreadSafeEventTest, TimedPopConsumesValueWithoutLeavingStaleState) {
  safe::event_t<int> event;

  event.raise(7);
  auto value = event.pop(0ms);
  ASSERT_TRUE(value);
  EXPECT_EQ(*value, 7);

  EXPECT_FALSE(event.try_pop());
  EXPECT_FALSE(event.pop(0ms));
}

TEST(ThreadSafeSharedTest, FailedInitializationReleasesPortsAndRetryStopsOnLastReference) {
  using boost::asio::ip::udp;
  boost::asio::io_context io;
  const udp::endpoint loopback {boost::asio::ip::address_v4::loopback(), 0};
  udp::socket first_port {io, loopback};
  udp::socket blocked_port {io, loopback};
  const auto first_endpoint = first_port.local_endpoint();
  const auto second_endpoint = blocked_port.local_endpoint();
  first_port.close();

  struct alignas(64) resource_t {
    boost::asio::io_context io;
    udp::socket first {io};
    udp::socket second {io};
    int *destructions = nullptr;

    ~resource_t() {
      ++*destructions;
    }
  };

  int destructions = 0;
  int runs = 0;
  int stops = 0;
  auto shared = safe::make_shared<resource_t>([&](resource_t &resource) {
    resource.destructions = &destructions;
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(&resource) % alignof(resource_t), 0u);
    resource.first.open(udp::v4());
    boost::system::error_code ec;
    resource.first.bind(first_endpoint, ec);
    EXPECT_FALSE(ec) << ec.message();
    if (ec) {
      return -1;
    }
    resource.second.open(udp::v4());
    resource.second.bind(second_endpoint, ec);
    if (ec) {
      return -1;
    }
    ++runs;
    return 0;
  }, [&](resource_t &resource) {
    EXPECT_TRUE(resource.first.is_open());
    EXPECT_TRUE(resource.second.is_open());
    ++stops;
  });

  for (int attempt = 1; attempt <= 2; ++attempt) {
    EXPECT_FALSE(shared.ref());
    EXPECT_EQ(destructions, attempt);
    EXPECT_EQ(runs, 0);
    EXPECT_EQ(stops, 0);
  }

  blocked_port.close();
  {
    auto first = shared.ref();
    ASSERT_TRUE(first);
    auto last = shared.ref();
    ASSERT_TRUE(last);
    EXPECT_EQ(first.get(), last.get());
    EXPECT_EQ(runs, 1);
    first.release();
    EXPECT_EQ(destructions, 2);
    EXPECT_EQ(stops, 0);
  }
  EXPECT_EQ(destructions, 3);
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(stops, 1);

  // Both sockets must also be released after the successful last reference.
  udp::socket reclaimed_first {io, first_endpoint};
  udp::socket reclaimed_second {io, second_endpoint};
}
