#include <gtest/gtest.h>
#include "src/adaptive_display.h"

using namespace adaptive_display;
using namespace std::chrono_literals;

namespace {
  class AdaptiveDesktop: public testing::Test {
  protected:
    topology detected {presence::absent, 0};
    uint32_t resource = 0, next_id = 10;
    int layout_calls = 0;
    bool helper_ok = true, create_ok = true, layout_ok = true, target_ready = true;
    std::string arrangement, power = "none";
    mode actual {};
    controller desktop {{
      [this](uint32_t) { return detected; },
      [this](mode m, const char *layout) {
        if (!create_ok) {
          return uint32_t(0);
        }
        actual = m;
        arrangement = layout;
        return resource = ++next_id;
      },
      [this](const char *layout, uint32_t) {
        ++layout_calls;
        if (!layout_ok) {
          return false;
        }
        arrangement = layout;
        return true;
      },
      [this](uint32_t id) { return helper_ok && resource && resource == id; },
      [this] { resource = 0; },
      [this](const std::string &kind) { power = kind; },
      [this](uint32_t id) { return target_ready && resource == id; }
    }};
    controller::clock::time_point now {};
    policy rules;
    token prepare(bool launch = true) { return desktop.prepare(launch, {1920, 1080, 60}, rules, now); }
    token connect() {
      auto t = prepare();
      EXPECT_TRUE(desktop.activate(t));
      desktop.established(t);
      return t;
    }
    uint32_t pause() {
      auto t = connect();
      desktop.finish(t, now);
      return resource;
    }
  };
}

TEST_F(AdaptiveDesktop, HeadlessPauseReusesDesktopAndPreservesMode) {
  auto id = pause();
  ASSERT_NE(id, 0);
  EXPECT_EQ(arrangement, "primary");
  EXPECT_EQ(power, "display");
  auto resumed = desktop.prepare(false, {2560, 1600, 120}, rules, now + 10s);
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(actual, (mode {1920, 1080, 60}));
  EXPECT_TRUE(desktop.activate(resumed));
  desktop.established(resumed);
  desktop.finish(resumed, now + 20s);
  EXPECT_EQ(resource, id);
  EXPECT_TRUE(desktop.snapshot().paused);
}

TEST_F(AdaptiveDesktop, LocalDisconnectRemovesAndFixedPrimaryNeverRetains) {
  detected.local = presence::present;
  auto t = connect();
  EXPECT_EQ(arrangement, "extend");
  desktop.finish(t, now);
  EXPECT_EQ(resource, 0);
  rules.adaptive = false;
  t = connect();
  EXPECT_EQ(arrangement, "primary");
  desktop.finish(t, now);
  EXPECT_EQ(resource, 0);
}

TEST_F(AdaptiveDesktop, HeadlessPrimaryDriftIsRepairedWithoutRecreation) {
  const auto id = pause();
  ASSERT_NE(id, 0);
  detected.main_display = 99; // An unusable local display became main during hotplug.
  arrangement = "changed by system";
  layout_ok = false;
  desktop.reconcile();
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");
  layout_ok = true;
  desktop.reconcile();
  EXPECT_EQ(arrangement, "primary");
  EXPECT_EQ(resource, id);
  detected.main_display = id;
  const auto calls = layout_calls;
  desktop.reconcile();
  EXPECT_EQ(layout_calls, calls); // No repeated reconfiguration when already correct.
}

TEST_F(AdaptiveDesktop, LocalPrimaryDriftIsRepairedDuringActiveStream) {
  detected = {presence::present, 42, 42};
  const auto t = connect();
  const auto id = resource;
  detected.main_display = id;
  arrangement = "changed by system";
  desktop.reconcile();
  EXPECT_EQ(arrangement, "extend");
  EXPECT_EQ(resource, id);
  EXPECT_TRUE(desktop.valid(t));
  EXPECT_EQ(desktop.snapshot().active, 1);
  detected.main_display = 42;
  const auto calls = layout_calls;
  desktop.reconcile();
  EXPECT_EQ(layout_calls, calls);
}

TEST_F(AdaptiveDesktop, LocalRetentionAndHeadlessRemovalPolicies) {
  detected.local = presence::present;
  rules.local_retain = true;
  EXPECT_NE(pause(), 0);
  desktop.revoke();
  detected.local = presence::absent;
  rules.headless_retain = false;
  EXPECT_EQ(pause(), 0);
}

TEST_F(AdaptiveDesktop, UnknownInitialTopologyRequiresOverrideButPreservesRetainedRole) {
  detected.local = presence::unknown;
  EXPECT_FALSE(prepare());
  EXPECT_EQ(resource, 0);
  rules.override_local = presence::absent;
  EXPECT_NE(pause(), 0);
  desktop.revoke();
  rules.override_local = presence::unknown;
  detected.local = presence::absent;
  auto id = pause();
  detected.local = presence::unknown;
  desktop.reconcile(now + 3s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(arrangement, "primary");
}

TEST_F(AdaptiveDesktop, HeadlessRetentionMaintainsPowerIndefinitelyUntilQuit) {
  const auto id = pause();
  ASSERT_NE(id, 0);
  EXPECT_EQ(power, "display");
  for (const auto elapsed : {24h, 240h, 2400h}) {
    desktop.reconcile(now + elapsed);
    EXPECT_EQ(resource, id);
    EXPECT_TRUE(desktop.snapshot().paused);
    EXPECT_EQ(power, "display");
  }
  desktop.revoke();
  EXPECT_EQ(resource, 0);
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, FailedInitialPreparationAndHandshakeReleaseResource) {
  create_ok = false;
  EXPECT_FALSE(prepare());
  EXPECT_EQ(resource, 0);
  create_ok = true;
  layout_ok = false;
  EXPECT_FALSE(prepare());
  EXPECT_EQ(resource, 0);
  layout_ok = true;
  auto t = prepare();
  ASSERT_TRUE(t);
  desktop.abort(t, now);
  EXPECT_EQ(resource, 0);
  t = prepare();
  ASSERT_TRUE(desktop.activate(t));
  desktop.finish(t, now); // No established control handshake.
  EXPECT_EQ(resource, 0);
}

TEST_F(AdaptiveDesktop, FailedResumeAfterLongAbsencePreservesDesktopAndPower) {
  const auto id = pause();
  now += 2400h;
  auto t = prepare(false);
  ASSERT_TRUE(t);
  desktop.abort(t, now);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");
  t = prepare(false);
  ASSERT_TRUE(t);
  desktop.reconcile(now + 2400h);
  EXPECT_EQ(resource, id);
  desktop.abort(t, now + 2400h);
  EXPECT_EQ(resource, id);
  EXPECT_TRUE(desktop.snapshot().paused);
  EXPECT_EQ(power, "display");
}

TEST_F(AdaptiveDesktop, FailedIndefiniteResumeRestoresExistingPause) {
  auto id = pause();
  now += 2400h;
  auto t = prepare(false);
  ASSERT_TRUE(desktop.activate(t));
  desktop.finish(t, now + 2400h);
  EXPECT_EQ(resource, id);
  EXPECT_TRUE(desktop.snapshot().paused);
  EXPECT_EQ(power, "display");
}

TEST_F(AdaptiveDesktop, PendingAndActiveClientsPinSharedResource) {
  auto a = connect();
  auto id = resource;
  auto b = prepare(false);
  desktop.finish(a, now);
  EXPECT_EQ(resource, id);
  EXPECT_FALSE(desktop.release_inactive());
  ASSERT_TRUE(desktop.activate(b));
  desktop.established(b);
  auto c = prepare(false);
  ASSERT_TRUE(desktop.activate(c));
  desktop.established(c);
  desktop.finish(b, now);
  EXPECT_EQ(desktop.snapshot().active, 1);
  desktop.finish(c, now);
  EXPECT_TRUE(desktop.snapshot().paused);
  EXPECT_EQ(resource, id);
}

TEST_F(AdaptiveDesktop, CancelRevokesBeforeDrainAndLateJoinCannotRetain) {
  auto a = connect();
  auto b = prepare(false);
  auto id = resource;
  desktop.revoke();
  const auto revoked = desktop.snapshot().revoked_owners;
  ASSERT_EQ(revoked.size(), 1);
  EXPECT_EQ(revoked.front().epoch, a.epoch);
  EXPECT_EQ(revoked.front().attempt, a.attempt);
  EXPECT_FALSE(desktop.activate(b));
  EXPECT_FALSE(prepare(false));
  EXPECT_EQ(resource, id); // Media owner has not drained yet.
  desktop.finish(a, now);
  EXPECT_EQ(resource, 0);
  desktop.finish(a, now);
  EXPECT_FALSE(desktop.snapshot().paused);
  auto next = prepare();
  ASSERT_TRUE(next);
  ASSERT_TRUE(desktop.activate(next));
  desktop.revoke(a.epoch); // Old process cleanup cannot revoke the next app.
  EXPECT_TRUE(desktop.valid(next));
}

TEST_F(AdaptiveDesktop, SuccessfulResumeAfterLongAbsenceReusesDesktop) {
  auto id = pause();
  now += 2400h;
  auto t = prepare(false);
  desktop.reconcile(now + 24h);
  EXPECT_EQ(resource, id);
  ASSERT_TRUE(desktop.activate(t));
  desktop.established(t);
  desktop.finish(t, now + 24h);
  desktop.reconcile(now + 2400h);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");
}

TEST_F(AdaptiveDesktop, ReturningUsableLocalDisplayRemovesIdleDesktopImmediately) {
  auto id = pause();
  detected.local = presence::unknown;
  desktop.reconcile(now);
  EXPECT_EQ(resource, id);
  detected.local = presence::present;
  detected.local_main = 1;
  desktop.reconcile(now);
  EXPECT_EQ(resource, 0);
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, LocalReturnChangesActiveRoleWithoutDestroyingDesktop) {
  auto t = connect();
  auto id = resource;
  detected.local = presence::present;
  detected.local_main = 1;
  desktop.reconcile(now);
  desktop.reconcile(now + 2s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(arrangement, "extend");
  desktop.finish(t, now + 3s);
  EXPECT_EQ(resource, 0);
}

TEST_F(AdaptiveDesktop, LocalReturnAtLastDisconnectRemovesWithoutWatcherReconcile) {
  auto t = connect();
  detected.local = presence::present;
  detected.local_main = 1;
  desktop.finish(t, now);
  EXPECT_EQ(resource, 0);
  EXPECT_EQ(power, "none");
  EXPECT_FALSE(desktop.snapshot().paused);
}

TEST_F(AdaptiveDesktop, HelperLossReleasesPausedResources) {
  pause();
  helper_ok = false;
  desktop.reconcile(now);
  EXPECT_EQ(resource, 0);
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, ExplicitReleaseAndShutdownCloseIdleAndPendingOwnership) {
  pause();
  EXPECT_TRUE(desktop.release_inactive());
  EXPECT_EQ(resource, 0);
  auto t = prepare(false);
  ASSERT_TRUE(t);
  EXPECT_FALSE(desktop.release_inactive());
  desktop.close();
  EXPECT_EQ(resource, 0);
  EXPECT_FALSE(desktop.activate(t));
  EXPECT_FALSE(prepare());
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, HelperFailureWaitsForActiveDrainBeforeRecreation) {
  auto t = connect();
  auto id = resource;
  helper_ok = false;
  desktop.reconcile(now);
  EXPECT_EQ(resource, id);
  EXPECT_FALSE(desktop.valid(t));
  EXPECT_FALSE(prepare(false));
  desktop.finish(t, now);
  EXPECT_EQ(resource, 0);
  helper_ok = true;
  auto resumed = prepare(false);
  EXPECT_TRUE(resumed);
  EXPECT_NE(resource, id);
}

TEST_F(AdaptiveDesktop, ShutdownDoesNotDestroyAnUndrainedActiveDesktop) {
  auto t = connect();
  auto id = resource;
  desktop.close();
  EXPECT_EQ(resource, id);
  desktop.established(t);
  desktop.finish(t, now);
  EXPECT_EQ(resource, 0);
  EXPECT_FALSE(desktop.snapshot().paused);
  EXPECT_FALSE(prepare());
}

TEST_F(AdaptiveDesktop, PresentOverrideSelectsExtensionAndNonePowerRetainsWithoutAssertion) {
  detected.local = presence::unknown;
  rules.override_local = presence::present;
  rules.local_retain = true;
  rules.power = "none";
  EXPECT_NE(pause(), 0);
  EXPECT_EQ(arrangement, "extend");
  EXPECT_EQ(power, "none");
  EXPECT_TRUE(desktop.snapshot().paused);
}

TEST_F(AdaptiveDesktop, FailedPendingOwnerPreservesEstablishedDisconnect) {
  auto a = connect();
  auto b = prepare(false);
  const auto id = resource;
  desktop.finish(a, now + 10s);
  EXPECT_EQ(power, "display");
  desktop.abort(b, now + 2400h);
  EXPECT_EQ(resource, id);
  desktop.reconcile(now + 4800h);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");
}

TEST_F(AdaptiveDesktop, FailedActiveHandshakePreservesEstablishedDisconnect) {
  auto a = connect();
  const auto id = resource;
  auto b = prepare(false);
  ASSERT_TRUE(desktop.activate(b));
  desktop.finish(a, now + 10s);
  desktop.reconcile(now + 2400h);
  EXPECT_NE(resource, 0); // The unfinished owner pins the desktop.
  desktop.finish(b, now + 2400h);
  EXPECT_EQ(resource, id);
  EXPECT_TRUE(desktop.snapshot().paused);
  EXPECT_EQ(power, "display");
}

TEST_F(AdaptiveDesktop, ResumePowerSurvivesPreparationAndFailedStreamingAssertion) {
  const auto id = pause();
  auto t = prepare(false);
  ASSERT_TRUE(t);
  EXPECT_EQ(power, "display");
  ASSERT_TRUE(desktop.activate(t));
  desktop.streaming_power(false); // Native streaming assertion acquisition failed.
  EXPECT_EQ(power, "display");
  desktop.finish(t, now + 10s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");

  t = prepare(false);
  ASSERT_TRUE(desktop.activate(t));
  desktop.streaming_power(true);
  EXPECT_EQ(power, "none");
  desktop.established(t);
  auto pending = prepare(false);
  desktop.finish(t, now + 20s);
  desktop.streaming_power(false); // Called before the last streaming assertion is released.
  EXPECT_EQ(power, "display");
  desktop.abort(pending, now + 30s);
  EXPECT_EQ(power, "display");
  EXPECT_TRUE(desktop.snapshot().paused);
}

TEST_F(AdaptiveDesktop, PendingResumeMaintainsSystemPowerUntilQuit) {
  rules.power = "system";
  const auto id = pause();
  auto t = prepare(false);
  desktop.reconcile(now + 2400h);
  EXPECT_EQ(power, "system");
  desktop.abort(t, now + 2400h);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "system");

  desktop.revoke();
  EXPECT_EQ(resource, 0);
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, TemporaryTargetLossPreservesPausedAndActiveOwnership) {
  const auto id = pause();
  target_ready = false;
  desktop.reconcile(now + 1s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");
  EXPECT_FALSE(prepare(false));
  EXPECT_TRUE(desktop.snapshot().paused);
  target_ready = true;
  auto t = prepare(false);
  ASSERT_TRUE(t);
  target_ready = false;
  EXPECT_FALSE(desktop.activate(t));
  EXPECT_TRUE(desktop.valid(t));
  target_ready = true;
  ASSERT_TRUE(desktop.activate(t));
  desktop.established(t);
  target_ready = false;
  desktop.reconcile(now + 2s);
  EXPECT_TRUE(desktop.valid(t));
  EXPECT_TRUE(desktop.snapshot().revoked_owners.empty());
  desktop.finish(t, now + 3s);
  EXPECT_EQ(resource, id);
  target_ready = true;
  EXPECT_TRUE(prepare(false));
  EXPECT_EQ(resource, id);
}

TEST_F(AdaptiveDesktop, FailedRoleTransitionCannotAuthorizeLocalRemoval) {
  const auto id = pause();
  detected.local = presence::present;
  detected.local_main = 1;
  layout_ok = false;
  desktop.reconcile(now);
  desktop.reconcile(now + 2s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(desktop.snapshot().role, presence::absent);
  auto t = prepare(false);
  desktop.abort(t, now + 3s);
  EXPECT_EQ(resource, id);
  layout_ok = true;
  desktop.reconcile(now + 4s);
  EXPECT_EQ(resource, 0);
}

TEST_F(AdaptiveDesktop, FailedLayoutDoesNotBlockCancelOrConfirmedDeath) {
  for (int reason = 0; reason < 2; ++reason) {
    detected.local = presence::absent;
    layout_ok = helper_ok = true;
    ASSERT_NE(pause(), 0);
    detected.local = presence::present;
    detected.local_main = 1;
    layout_ok = false;
    desktop.reconcile(now);
    desktop.reconcile(now + 2s);
    ASSERT_NE(resource, 0);
    if (reason == 0) {
      desktop.revoke();
    } else {
      helper_ok = false;
      desktop.reconcile(now + 3s);
    }
    EXPECT_EQ(resource, 0);
    EXPECT_EQ(power, "none");
  }
}

TEST_F(AdaptiveDesktop, LocalLossAtLastDisconnectRetainsWithoutWatcherReconcile) {
  detected.local = presence::present;
  detected.local_main = 1;
  auto t = connect();
  const auto id = resource;
  ASSERT_EQ(arrangement, "extend");
  detected.local = presence::absent;
  detected.local_main = 0;
  desktop.finish(t, now + 10s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(arrangement, "primary");
  EXPECT_EQ(desktop.snapshot().role, presence::absent);
  EXPECT_TRUE(desktop.snapshot().paused);
  EXPECT_EQ(power, "display");
}

TEST_F(AdaptiveDesktop, FailedPrimaryTransitionDefersRemovalUntilTopologyRecovers) {
  detected.local = presence::present;
  detected.local_main = 1;
  auto t = connect();
  const auto id = resource;
  detected.local = presence::absent;
  detected.local_main = 0;
  layout_ok = false;
  desktop.finish(t, now + 10s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(desktop.snapshot().role, presence::present);
  EXPECT_TRUE(desktop.snapshot().paused);
  desktop.reconcile(now + 11s);
  EXPECT_EQ(resource, id);
  detected.local = presence::unknown;
  desktop.reconcile(now + 12s);
  EXPECT_EQ(resource, id);
  detected.local = presence::absent;
  layout_ok = true;
  desktop.reconcile(now + 13s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(arrangement, "primary");
  EXPECT_EQ(desktop.snapshot().role, presence::absent);
  desktop.reconcile(now + 2400h);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(power, "display");
}
