/**
 * @file tests/unit/platform/test_macos_signal_dispatcher.cpp
 * @brief Tests macOS process signal dispatch.
 */
#if defined(__APPLE__) || defined(__MACH__)

  #include <atomic>
  #include <cerrno>
  #include <chrono>
  #include <dirent.h>
  #include <csignal>
  #include <future>
  #include <thread>

  #include <gtest/gtest.h>

  #include <src/platform/macos/signal_dispatcher.h>

using namespace std::chrono_literals;

TEST(MacosSignalDispatcherTests, DeliversThreadDirectedSigintOnWorkerThread) {
  std::promise<std::thread::id> delivered;
  auto delivered_future = delivered.get_future();
  const auto raising_thread = std::this_thread::get_id();
  macos_signal::dispatcher dispatcher;

  ASSERT_TRUE(dispatcher.register_handler(SIGINT, [&delivered]() {
    delivered.set_value(std::this_thread::get_id());
  }));

  std::raise(SIGINT);

  ASSERT_EQ(delivered_future.wait_for(1s), std::future_status::ready);
  EXPECT_NE(delivered_future.get(), raising_thread);
}

TEST(MacosSignalDispatcherTests, DeliversProcessDirectedSigtermOnWorkerThread) {
  std::promise<std::thread::id> delivered;
  auto delivered_future = delivered.get_future();
  const auto raising_thread = std::this_thread::get_id();
  macos_signal::dispatcher dispatcher;

  ASSERT_TRUE(dispatcher.register_handler(SIGTERM, [&delivered]() {
    delivered.set_value(std::this_thread::get_id());
  }));

  ASSERT_EQ(kill(getpid(), SIGTERM), 0);

  ASSERT_EQ(delivered_future.wait_for(1s), std::future_status::ready);
  EXPECT_NE(delivered_future.get(), raising_thread);
}

TEST(MacosSignalDispatcherTests, StopWaitsForRunningCallback) {
  std::promise<void> callback_started;
  auto callback_started_future = callback_started.get_future();
  std::promise<void> allow_callback_finish;
  auto allow_callback_finish_future = allow_callback_finish.get_future().share();
  std::promise<void> stop_returned;
  auto stop_returned_future = stop_returned.get_future();
  macos_signal::dispatcher dispatcher;

  ASSERT_TRUE(dispatcher.register_handler(SIGTERM, [&]() {
    callback_started.set_value();
    allow_callback_finish_future.wait();
  }));
  ASSERT_EQ(kill(getpid(), SIGTERM), 0);

  const bool started = callback_started_future.wait_for(1s) == std::future_status::ready;
  if (!started) {
    allow_callback_finish.set_value();
    FAIL() << "SIGTERM callback did not start";
  }

  std::thread stopper([&]() {
    dispatcher.stop();
    stop_returned.set_value();
  });
  EXPECT_EQ(stop_returned_future.wait_for(50ms), std::future_status::timeout);

  allow_callback_finish.set_value();
  EXPECT_EQ(stop_returned_future.wait_for(1s), std::future_status::ready);
  stopper.join();
}

namespace {
  // Preserve the test runner's dispositions while testing process defaults.
  struct signal_dispositions {
    struct sigaction interrupt {};
    struct sigaction terminate {};

    signal_dispositions() {
      sigaction(SIGINT, nullptr, &interrupt);
      sigaction(SIGTERM, nullptr, &terminate);
      struct sigaction action {};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      sigaction(SIGINT, &action, nullptr);
      sigaction(SIGTERM, &action, nullptr);
    }

    ~signal_dispositions() {
      sigaction(SIGINT, &interrupt, nullptr);
      sigaction(SIGTERM, &terminate, nullptr);
    }
  };

  int open_descriptor_count() {
    DIR *directory = opendir("/dev/fd");
    if (!directory) {
      return -1;
    }
    int count = 0;
    while (const auto *entry = readdir(directory)) {
      if (entry->d_name[0] != '.') {
        ++count;
      }
    }
    closedir(directory);
    return count;
  }
}  // namespace

TEST(MacosSignalDispatcherTests, RepeatedRegistrationRestoresOriginalDefaults) {
  signal_dispositions dispositions;
  std::promise<void> delivered[2];
  macos_signal::dispatcher dispatcher;
  int index = 0;
  struct sigaction action {};
  for (const int signal : {SIGINT, SIGTERM}) {
    ASSERT_EQ(sigaction(signal, nullptr, &action), 0);
    EXPECT_EQ(action.sa_handler, SIG_DFL);
    ASSERT_TRUE(dispatcher.register_handler(signal, []() {}));
    auto *completion = &delivered[index++];
    auto future = completion->get_future();
    ASSERT_TRUE(dispatcher.register_handler(signal, [completion]() { completion->set_value(); }));
    ASSERT_EQ(std::raise(signal), 0);
    ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
  }
  dispatcher.stop();
  dispatcher.stop();
  EXPECT_FALSE(dispatcher.register_handler(SIGINT, []() {}));
  for (const int signal : {SIGINT, SIGTERM}) {
    ASSERT_EQ(sigaction(signal, nullptr, &action), 0);
    EXPECT_EQ(action.sa_handler, SIG_DFL);
  }
}

TEST(MacosSignalDispatcherTests, RejectsSecondOwnerUntilFirstStops) {
  std::promise<void> delivered;
  auto future = delivered.get_future();
  macos_signal::dispatcher first;
  macos_signal::dispatcher second;
  ASSERT_TRUE(first.register_handler(SIGINT, [&]() { delivered.set_value(); }));
  EXPECT_FALSE(second.register_handler(SIGTERM, []() {}));
  ASSERT_EQ(std::raise(SIGINT), 0);
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
  first.stop();
  ASSERT_TRUE(second.register_handler(SIGTERM, []() {}));
}

TEST(MacosSignalDispatcherTests, PreservesErrnoWithFullPipeAndStops) {
  std::promise<void> started;
  auto started_future = started.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  std::atomic_bool first {true};
  macos_signal::dispatcher dispatcher;
  ASSERT_TRUE(dispatcher.register_handler(SIGINT, [&]() {
    if (first.exchange(false)) {
      started.set_value();
      release_future.wait();
    }
  }));
  errno = EDOM;
  const int raised = std::raise(SIGINT);
  const int saved_errno = errno;
  EXPECT_EQ(raised, 0);
  EXPECT_EQ(saved_errno, EDOM);
  const bool callback_started = started_future.wait_for(1s) == std::future_status::ready;
  if (!callback_started) {
    release.set_value();
    FAIL() << "SIGINT callback did not start";
  }
  // The worker is blocked, so these bytes exceed the macOS pipe capacity.
  for (int i = 0; i < 100000; ++i) {
    errno = EDOM;
    const int result = std::raise(SIGINT);
    const int after_signal = errno;
    EXPECT_EQ(result, 0);
    EXPECT_EQ(after_signal, EDOM);
  }
  release.set_value();
  dispatcher.stop();
}

TEST(MacosSignalDispatcherTests, EarlyReturnJoinsCallbackAndClosesDescriptors) {
  const int before = open_descriptor_count();
  ASSERT_GE(before, 0);
  for (int i = 0; i < 20; ++i) {
    std::promise<void> started;
    auto started_future = started.get_future();
    bool completed = false;
    auto run = [&]() {
      macos_signal::dispatcher dispatcher;
      if (!dispatcher.register_handler(SIGTERM, [&]() {
            started.set_value();
            std::this_thread::sleep_for(10ms);
            completed = true;
          })) {
        return false;
      }
      if (std::raise(SIGTERM) != 0) {
        return false;
      }
      return started_future.wait_for(1s) == std::future_status::ready;
    };
    ASSERT_TRUE(run());
    EXPECT_TRUE(completed);
  }
  EXPECT_EQ(open_descriptor_count(), before);
}

#endif
