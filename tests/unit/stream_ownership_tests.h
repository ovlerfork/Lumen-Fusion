// Included by stream.cpp to exercise cancellation without starting media workers.
#include <gtest/gtest.h>

namespace stream {
  TEST(StreamOwnership, DesktopCancellationTargetsOnlyItsOwner) {
    session_t old {}, resumed {}, current {}, unmanaged {};
    for (auto *owner : {&old, &resumed, &current, &unmanaged}) {
      owner->mail = std::make_shared<safe::mail_raw_t>();
      owner->shutdown_event = owner->mail->event<bool>(mail::shutdown);
      owner->state = session::state_e::RUNNING;
    }
    old.desktop = {7, 1};
    resumed.desktop = {7, 2};
    current.desktop = {8, 3};

    // An old watcher observation can arrive after the next generation activates.
    for (auto *owner : {&old, &resumed, &current, &unmanaged}) {
      session::stop_by_desktop_owner(*owner, old.desktop);
    }
    EXPECT_EQ(session::state(old), session::state_e::STOPPING);
    EXPECT_TRUE(old.shutdown_event->peek());
    EXPECT_EQ(session::state(resumed), session::state_e::RUNNING);
    EXPECT_FALSE(resumed.shutdown_event->peek());
    EXPECT_EQ(session::state(current), session::state_e::RUNNING);
    EXPECT_FALSE(current.shutdown_event->peek());
    EXPECT_EQ(session::state(unmanaged), session::state_e::RUNNING);
    EXPECT_FALSE(unmanaged.shutdown_event->peek());

    session::stop_by_desktop_owner(current, current.desktop);
    EXPECT_EQ(session::state(current), session::state_e::STOPPING);
    EXPECT_TRUE(current.shutdown_event->peek());
  }
}  // namespace stream
