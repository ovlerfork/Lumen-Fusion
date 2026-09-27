#include <gtest/gtest.h>
#include "src/adaptive_display.h"

using namespace adaptive_display;
using namespace std::chrono_literals;

namespace {
  class AdaptiveDesktop: public testing::Test {
  protected:
    topology detected {presence::absent, 0, power_source::external};
    uint32_t resource = 0, next_id = 10;
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
  EXPECT_EQ(desktop.snapshot().deadline, now + 620s);
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

TEST_F(AdaptiveDesktop, PowerChangesOnlyReconcileIdleAssertions) {
  const auto id = pause();
  const auto deadline = desktop.snapshot().deadline;
  ASSERT_NE(id, 0);
  EXPECT_EQ(power, "display");
  for (const auto source : {power_source::battery, power_source::unknown, power_source::external}) {
    detected.power = source;
    desktop.reconcile(now + 1s);
    EXPECT_EQ(resource, id);
    EXPECT_EQ(desktop.snapshot().deadline, deadline);
    EXPECT_TRUE(desktop.snapshot().paused);
    EXPECT_EQ(power, source == power_source::external ? "display" : "none");
  }
  detected.power = power_source::battery;
  desktop.reconcile(now + 600s);
  EXPECT_EQ(resource, 0);
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, BatteryOptInControlsPowerAndUnknownPowerRetainsPassively) {
  detected.power = power_source::battery;
  ASSERT_NE(pause(), 0);
  EXPECT_EQ(power, "none");
  rules.on_battery = true;
  rules.power = "system";
  ASSERT_NE(pause(), 0);
  EXPECT_EQ(power, "system");
  rules.retention = 0s;
  detected.power = power_source::unknown;
  const auto id = pause();
  ASSERT_NE(id, 0);
  EXPECT_EQ(power, "none");
  EXPECT_FALSE(desktop.snapshot().deadline);
  desktop.reconcile(now + 10000s);
  EXPECT_EQ(resource, id);
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

TEST_F(AdaptiveDesktop, FailedResumeRestoresOriginalDeadlineIncludingExpiredAttempt) {
  const auto id = pause();
  now += 590s;
  auto t = prepare(false);
  desktop.abort(t, now);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(desktop.snapshot().deadline, controller::clock::time_point {} + 600s);
  t = prepare(false);
  desktop.reconcile(now + 20s); // Preparation pins it past the old deadline.
  EXPECT_EQ(resource, id);
  desktop.abort(t, now + 20s);
  EXPECT_EQ(resource, 0);
}

TEST_F(AdaptiveDesktop, FailedIndefiniteResumeRestoresExistingPause) {
  rules.retention = 0s;
  auto id = pause();
  auto t = prepare(false);
  ASSERT_TRUE(desktop.activate(t));
  desktop.finish(t, now + 1000s);
  EXPECT_EQ(resource, id);
  EXPECT_TRUE(desktop.snapshot().paused);
  EXPECT_FALSE(desktop.snapshot().deadline);
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

TEST_F(AdaptiveDesktop, ExpiryCannotDestroyAReservedResumeOrItsReplacement) {
  auto id = pause();
  now += 599s;
  auto t = prepare(false);
  desktop.reconcile(now + 2s);
  EXPECT_EQ(resource, id);
  ASSERT_TRUE(desktop.activate(t));
  desktop.established(t);
  desktop.finish(t, now + 2s);
  desktop.reconcile(now + 3s);
  EXPECT_EQ(resource, id);
  now += 602s;
  desktop.reconcile(now);
  EXPECT_EQ(resource, 0);
  t = prepare(false);
  ASSERT_TRUE(t);
  EXPECT_NE(resource, id);
  desktop.reconcile(now + 1s);
  EXPECT_NE(resource, 0);
}

TEST_F(AdaptiveDesktop, ReturningLocalDisplayMustRemainAvailableBeforeRemoval) {
  auto id = pause();
  detected.local = presence::present;
  detected.local_main = 1;
  desktop.reconcile(now);
  EXPECT_EQ(resource, id);
  detected.local = presence::unknown;
  desktop.reconcile(now + 1s);
  EXPECT_EQ(resource, id);
  detected.local = presence::present;
  desktop.reconcile(now + 2s);
  EXPECT_EQ(resource, id);
  desktop.reconcile(now + 4s);
  EXPECT_EQ(resource, 0);
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
  detected.power = power_source::battery;
  EXPECT_NE(pause(), 0);
  EXPECT_EQ(arrangement, "extend");
  EXPECT_EQ(power, "none");
  EXPECT_TRUE(desktop.snapshot().paused);
}

TEST_F(AdaptiveDesktop, FailedPendingOwnerDoesNotMoveEstablishedDisconnectDeadline) {
  auto a = connect();
  auto b = prepare(false);
  const auto id = resource;
  desktop.finish(a, now + 10s);
  EXPECT_EQ(power, "display");
  desktop.abort(b, now + 500s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(desktop.snapshot().deadline, now + 610s);
  desktop.reconcile(now + 610s);
  EXPECT_EQ(resource, 0);
}

TEST_F(AdaptiveDesktop, FailedActiveHandshakeCannotReplenishElapsedPendingBudget) {
  auto a = connect();
  auto b = prepare(false);
  ASSERT_TRUE(desktop.activate(b));
  desktop.finish(a, now + 10s);
  desktop.reconcile(now + 700s);
  EXPECT_NE(resource, 0); // The unfinished owner pins the desktop.
  desktop.finish(b, now + 700s);
  EXPECT_EQ(resource, 0);
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
  EXPECT_EQ(desktop.snapshot().deadline, now + 600s);

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
  EXPECT_EQ(desktop.snapshot().deadline, now + 620s);
}

TEST_F(AdaptiveDesktop, PendingResumePowerTracksBatteryPolicy) {
  rules.on_battery = true;
  rules.power = "system";
  pause();
  auto t = prepare(false);
  detected.power = power_source::battery;
  desktop.reconcile(now + 1s);
  EXPECT_EQ(power, "system");
  detected.power = power_source::unknown;
  desktop.reconcile(now + 2s);
  EXPECT_EQ(power, "none");
  detected.power = power_source::external;
  desktop.abort(t, now + 3s);
  EXPECT_EQ(power, "system");

  desktop.revoke();
  rules.on_battery = false;
  pause();
  t = prepare(false);
  detected.power = power_source::battery;
  desktop.reconcile(now + 4s);
  EXPECT_EQ(power, "none");
  desktop.abort(t, now + 5s);
  EXPECT_EQ(power, "none");
}

TEST_F(AdaptiveDesktop, TemporaryTargetLossPreservesPausedAndActiveOwnership) {
  const auto id = pause();
  const auto deadline = desktop.snapshot().deadline;
  target_ready = false;
  desktop.reconcile(now + 1s);
  EXPECT_EQ(resource, id);
  EXPECT_EQ(desktop.snapshot().deadline, deadline);
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

TEST_F(AdaptiveDesktop, FailedLayoutDoesNotBlockExpiryCancelOrConfirmedDeath) {
  for (int reason = 0; reason < 3; ++reason) {
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
      desktop.reconcile(now + 600s);
    } else if (reason == 1) {
      desktop.revoke();
    } else {
      helper_ok = false;
      desktop.reconcile(now + 3s);
    }
    EXPECT_EQ(resource, 0);
    EXPECT_EQ(power, "none");
  }
}
