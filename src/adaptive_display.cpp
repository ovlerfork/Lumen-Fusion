#include "adaptive_display.h"

#include <iterator>

namespace adaptive_display {
  bool controller::matches(token t) const {
    return t.epoch == epoch && owners.contains(t.attempt);
  }

  topology controller::inspect() {
    auto t = native.inspect(display);
    if (rules.override_local != presence::unknown) {
      t.local = rules.override_local;
    }
    return t;
  }

  topology controller::observe(clock::time_point now) {
    auto t = inspect();
    if (t.local == presence::present) {
      if (!candidate_since || candidate_main != t.local_main) {
        candidate_since = now;
        candidate_main = t.local_main;
      }
      if (now - *candidate_since < std::chrono::seconds(2)) {
        t.local = presence::unknown;
      }
    } else {
      candidate_since.reset();
    }
    return t;
  }

  bool controller::may_retain() const {
    if (!rules.adaptive || revoked || closed || helper_failed) {
      return false;
    }
    // Only an accepted layout can change the policy used for topology cleanup.
    return role == presence::present ? rules.local_retain : rules.headless_retain;
  }

  void controller::update_retention_power(const topology &t) {
    const bool permitted = t.power == power_source::external ||
                           (t.power == power_source::battery && rules.on_battery);
    native.retention_power((retained || last_disconnect) && !streaming_protected && permitted ? rules.power : "none");
  }

  void controller::destroy() {
    if (display) {
      native.destroy();
    }
    native.retention_power("none");
    display = 0;
    paused = false;
    deadline.reset();
    candidate_since.reset();
    role = presence::unknown;
    local_main = 0;
    last_disconnect.reset();
    retained = false;
    helper_failed = false;
  }

  token controller::prepare(bool new_launch, mode requested, policy requested_rules, clock::time_point now) {
    std::lock_guard lock(mutex);
    if (closed || requested.width <= 0 || requested.height <= 0 || requested.fps <= 0) {
      return {};
    }
    if (new_launch) {
      if (!owners.empty()) {
        return {};
      }
      destroy();
      ++epoch;
      revoked = false;
      rules = std::move(requested_rules);
    } else if (revoked) {
      return {};
    }
    if (helper_failed && !owners.empty()) {
      return {};
    }
    if (display && owners.empty() && deadline && now >= *deadline) {
      destroy();
    }
    if (display && !native.healthy(display)) {
      if (!owners.empty()) {
        return {};
      }
      destroy();
    }
    if (!display) {
      const auto t = inspect(); // Before ensure or any connection activity wake.
      if (rules.adaptive && t.local == presence::unknown) {
        return {}; // No original role to preserve; require detection or override.
      }
      role = rules.adaptive ? t.local : presence::absent;
      local_main = t.local_main;
      display = native.ensure(requested, role == presence::present ? "extend" : "primary");
      if (!display || !native.layout(role == presence::present ? "extend" : "primary", local_main) || !native.healthy(display) || !native.ready(display)) {
        destroy();
        return {};
      }
    } else if (!native.ready(display)) {
      return {};
    }
    // A retained desktop's mode and deadline remain unchanged until a successful
    // connection ends. Failed Resume attempts cannot replenish the idle budget.
    paused = false;
    update_retention_power(inspect());
    const token result {epoch, ++next_attempt};
    owners.emplace(result.attempt, owner {});
    return result;
  }

  bool controller::valid(token t) {
    std::lock_guard lock(mutex);
    return matches(t) && !revoked && !closed && !helper_failed;
  }

  bool controller::activate(token t) {
    std::lock_guard lock(mutex);
    if (!matches(t) || revoked || closed || helper_failed || owners.at(t.attempt).active || !display || !native.ready(display)) {
      return false;
    }
    owners.at(t.attempt).active = true;
    return true;
  }

  void controller::established(token t) {
    std::lock_guard lock(mutex);
    if (matches(t) && !revoked && !helper_failed && owners.at(t.attempt).active) {
      owners.at(t.attempt).established = true;
      deadline.reset();
      last_disconnect.reset();
    }
  }

  void controller::streaming_power(bool protected_by_stream) {
    std::lock_guard lock(mutex);
    streaming_protected = protected_by_stream;
    update_retention_power(inspect());
  }

  void controller::settle(bool disconnected, clock::time_point now) {
    if (disconnected) {
      last_disconnect = now;
    }
    const auto t = observe(now);
    if (!owners.empty()) {
      update_retention_power(t);
      return;
    }
    if (!display || !native.healthy(display) || !may_retain() || (!last_disconnect && !deadline && !retained)) {
      // A failed first connection has no retained desktop to restore.
      destroy();
      return;
    }
    if (last_disconnect) {
      deadline = rules.retention.count() == 0 ? std::nullopt : std::optional(*last_disconnect + rules.retention);
    }
    if (deadline && now >= *deadline) {
      destroy();
      return;
    }
    last_disconnect.reset();
    paused = retained = true;
    update_retention_power(t);
  }

  void controller::finish(token t, clock::time_point now) {
    std::lock_guard lock(mutex);
    if (!matches(t)) {
      return;
    }
    bool connected = owners.at(t.attempt).established;
    owners.erase(t.attempt);
    settle(connected, now);
  }

  void controller::abort(token t, clock::time_point now) {
    std::lock_guard lock(mutex);
    if (!matches(t) || owners.at(t.attempt).active) {
      return;
    }
    owners.erase(t.attempt);
    settle(false, now);
  }

  void controller::revoke(uint64_t target_epoch) {
    std::lock_guard lock(mutex);
    if (target_epoch && target_epoch != epoch) {
      return;
    }
    revoked = true;
    for (auto it = owners.begin(); it != owners.end();) {
      it = it->second.active ? std::next(it) : owners.erase(it);
    }
    if (owners.empty()) {
      destroy();
    }
  }

  bool controller::release_inactive() {
    std::lock_guard lock(mutex);
    if (!owners.empty()) {
      return false;
    }
    destroy();
    return true;
  }

  void controller::reconcile(clock::time_point now) {
    std::lock_guard lock(mutex);
    if (!display) {
      return;
    }
    if (!native.healthy(display)) {
      // Owners still drain their media before native teardown.
      helper_failed = true;
      if (owners.empty()) {
        destroy();
      }
      return;
    }
    auto t = observe(now);
    if (rules.adaptive && t.local != presence::unknown && (role != t.local || local_main != t.local_main)) {
      if (native.layout(t.local == presence::present ? "extend" : "primary", t.local_main)) {
        role = t.local;
        local_main = t.local_main;
      } else if (!native.healthy(display)) {
        helper_failed = true;
      }
    }
    if (owners.empty() && (revoked || helper_failed || !may_retain() || (deadline && now >= *deadline))) {
      destroy();
    } else {
      update_retention_power(t);
    }
  }

  void controller::close() {
    std::lock_guard lock(mutex);
    closed = revoked = true;
    for (auto it = owners.begin(); it != owners.end();) {
      it = it->second.active ? std::next(it) : owners.erase(it);
    }
    if (owners.empty()) {
      destroy();
    }
  }

  status controller::snapshot() {
    std::lock_guard lock(mutex);
    status result {display, 0, 0, paused, closed, revoked || helper_failed, role, deadline, {}};
    for (const auto &[id, o] : owners) {
      o.active ? ++result.active : ++result.preparing;
      if (o.active && result.revoked) {
        result.revoked_owners.push_back({epoch, id});
      }
    }
    return result;
  }
}  // namespace adaptive_display
