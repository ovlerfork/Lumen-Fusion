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

  bool controller::update_role(const topology &t) {
    if (!rules.adaptive) {
      return true;
    }
    if (t.local == presence::unknown) {
      return false;
    }
    const auto expected_main = t.local == presence::present ? t.local_main : display;
    // macOS can change the main display during hotplug without changing which
    // local displays are usable. Repair that drift without recreating the VD.
    const bool main_changed = t.main_display && expected_main && t.main_display != expected_main;
    if (role != t.local || local_main != t.local_main || main_changed) {
      if (!native.layout(t.local == presence::present ? "extend" : "primary", t.local_main)) {
        if (!native.healthy(display)) {
          helper_failed = true;
        }
        return false;
      }
      role = t.local;
      local_main = t.local_main;
    }
    return true;
  }

  bool controller::may_retain(bool role_confirmed) const {
    if (!rules.adaptive || revoked || closed || helper_failed) {
      return false;
    }
    // Uncertain observations and unaccepted transitions cannot authorize teardown.
    return !role_confirmed || (role == presence::present ? rules.local_retain : rules.headless_retain);
  }

  void controller::update_retention_power() {
    native.retention_power((retained || disconnected) && !streaming_protected ? rules.power : "none");
  }

  void controller::destroy() {
    if (display) {
      native.destroy();
    }
    native.retention_power("none");
    display = 0;
    paused = false;
    role = presence::unknown;
    local_main = 0;
    disconnected = false;
    retained = false;
    helper_failed = false;
  }

  token controller::prepare(bool new_launch, mode requested, policy requested_rules, clock::time_point) {
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
    // Failed Resume attempts preserve the retained desktop and its mode.
    paused = false;
    update_retention_power();
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
      disconnected = false;
    }
  }

  void controller::streaming_power(bool protected_by_stream) {
    std::lock_guard lock(mutex);
    streaming_protected = protected_by_stream;
    update_retention_power();
  }

  void controller::settle(bool connected) {
    if (connected) {
      disconnected = true;
    }
    const auto t = inspect();
    if (!owners.empty()) {
      update_retention_power();
      return;
    }
    if (!display || !native.healthy(display) || (!disconnected && !retained)) {
      // A failed first connection has no retained desktop to restore.
      destroy();
      return;
    }
    const bool role_confirmed = update_role(t);
    if (!may_retain(role_confirmed)) {
      destroy();
      return;
    }
    disconnected = false;
    paused = retained = true;
    update_retention_power();
  }

  void controller::finish(token t, clock::time_point) {
    std::lock_guard lock(mutex);
    if (!matches(t)) {
      return;
    }
    bool connected = owners.at(t.attempt).established;
    owners.erase(t.attempt);
    settle(connected);
  }

  void controller::abort(token t, clock::time_point) {
    std::lock_guard lock(mutex);
    if (!matches(t) || owners.at(t.attempt).active) {
      return;
    }
    owners.erase(t.attempt);
    settle(false);
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

  void controller::reconcile(clock::time_point) {
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
    auto t = inspect();
    const bool role_confirmed = update_role(t);
    if (owners.empty() && (revoked || helper_failed || !may_retain(role_confirmed))) {
      destroy();
    } else {
      update_retention_power();
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
    status result {display, 0, 0, paused, closed, revoked || helper_failed, role, {}};
    for (const auto &[id, o] : owners) {
      o.active ? ++result.active : ++result.preparing;
      if (o.active && result.revoked) {
        result.revoked_owners.push_back({epoch, id});
      }
    }
    return result;
  }
}  // namespace adaptive_display
