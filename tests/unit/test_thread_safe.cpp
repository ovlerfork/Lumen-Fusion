/**
 * @file tests/unit/test_thread_safe.cpp
 * @brief Regression tests for thread-safe event behavior.
 */

#include <chrono>

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
