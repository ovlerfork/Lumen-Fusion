#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace adaptive_display {
  enum class presence {
    unknown,
    absent,
    present
  };
  enum class power_source {
    unknown,
    battery,
    external
  };
  struct topology {
    presence local = presence::unknown;
    uint32_t local_main = 0;
    power_source power = power_source::unknown;
  };
  struct policy {
    bool adaptive = true;
    bool local_retain = false;
    bool headless_retain = true;
    bool on_battery = false;
    std::chrono::seconds retention {600};
    std::string power = "display";
    presence override_local = presence::unknown;
  };
  struct mode {
    int width, height, fps;
    bool operator==(const mode &) const = default;
  };
  struct token {
    uint64_t epoch = 0, attempt = 0;
    explicit operator bool() const {
      return attempt != 0;
    }
  };
  struct status {
    uint32_t display = 0;
    std::size_t preparing = 0, active = 0;
    bool paused = false, closed = false, revoked = false;
    presence role = presence::unknown;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::vector<token> revoked_owners;
  };

  // Native calls are serialized by the controller. They must not call back into
  // the controller or acquire stream, process, or RTSP locks.
  struct backend {
    std::function<topology(uint32_t)> inspect;
    std::function<uint32_t(mode, const char *)> ensure;
    std::function<bool(const char *, uint32_t)> layout;
    // Ownership survives temporary target invisibility; readiness only gates admission.
    std::function<bool(uint32_t)> healthy;
    std::function<void()> destroy;
    std::function<void(const std::string &)> retention_power;
    std::function<bool(uint32_t)> ready;
  };

  class controller {
  public:
    using clock = std::chrono::steady_clock;
    explicit controller(backend native):
        native(std::move(native)) {
    }
    token prepare(bool new_launch, mode requested, policy rules, clock::time_point now = clock::now());
    bool valid(token owner);
    bool activate(token owner);
    void established(token owner);
    // Acquire streaming protection before true; notify false before releasing it.
    void streaming_power(bool protected_by_stream);
    void finish(token owner, clock::time_point now = clock::now());
    void abort(token owner, clock::time_point now = clock::now());
    void revoke(uint64_t epoch = 0);
    bool release_inactive();
    void reconcile(clock::time_point now = clock::now());
    void close();
    status snapshot();

  private:
    struct owner {
      bool active = false, established = false;
    };
    bool matches(token t) const;
    topology inspect();
    topology observe(clock::time_point now);
    bool update_role(const topology &t);
    bool may_retain(bool role_confirmed) const;
    void update_retention_power(const topology &t);
    void settle(bool disconnected, clock::time_point now);
    void destroy();
    backend native;
    std::mutex mutex;
    std::map<uint64_t, owner> owners;
    uint64_t epoch = 0, next_attempt = 0;
    bool revoked = true, closed = false, paused = false;
    bool retained = false, helper_failed = false, streaming_protected = false;
    uint32_t display = 0;
    policy rules;
    presence role = presence::unknown;
    uint32_t local_main = 0, candidate_main = 0;
    std::optional<clock::time_point> candidate_since, deadline, last_disconnect;
  };

  // Runtime binding; unmanaged layouts receive an empty token.
  bool enabled();
  token prepare(bool new_launch, int width, int height, int fps);
  bool valid(token owner);
  bool activate(token owner);
  void established(token owner);
  void streaming_power(bool protected_by_stream);
  void finish(token owner);
  void abort(token owner);
  void revoke(uint64_t epoch = 0);
  bool release_inactive();
  status snapshot();
  void close();
  void shutdown();
  void show_status_panel();
}  // namespace adaptive_display
