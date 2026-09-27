#include "adaptive_display.h"
#include "config.h"
#include "globals.h"
#include "logging.h"
#include "process.h"

#include <condition_variable>
#include <thread>

namespace adaptive_display {
#ifdef __APPLE__
  backend macos_backend();
  namespace {
    struct runtime {
      controller desktop {macos_backend()};
      std::mutex mutex;
      std::condition_variable changed;
      bool stopping = false;
      std::thread watcher;

      void watch() {
        std::lock_guard lock(mutex);
        if (!watcher.joinable() && !stopping) {
          watcher = std::thread([this] {
            std::unique_lock lock(mutex);
            while (!stopping) {
              changed.wait(lock, [this] { return stopping || desktop.snapshot().display; });
              if (stopping) {
                break;
              }
              changed.wait_for(lock, std::chrono::seconds(1));
              if (stopping) {
                break;
              }
              lock.unlock();
              if (desktop.snapshot().display) {
                // Process polling owns its own lifecycle lock. Never call it
                // from a native callback or while holding the controller lock.
                proc::proc.running();
                desktop.reconcile();
                const auto current = desktop.snapshot();
                if (current.revoked && current.active) {
                  mail::man->event<bool>(mail::broadcast_shutdown)->raise(true);
                }
              }
              lock.lock();
            }
          });
        }
        changed.notify_all();
      }
      void stop() {
        desktop.close();
        {
          std::lock_guard lock(mutex);
          stopping = true;
        }
        changed.notify_all();
        if (watcher.joinable()) {
          watcher.join();
        }
        desktop.release_inactive();
      }
    };
    runtime &state() {
      // Shutdown explicitly joins the watcher before platform teardown. Keep
      // the closed state available to late RTSP socket destructors.
      static auto *value = new runtime;
      return *value;
    }
    bool shutting_down() {
      return mail::man->event<bool>(mail::shutdown)->peek();
    }
  }
#endif

  bool enabled() {
#ifdef __APPLE__
    return config::video.virtual_display == "enabled" &&
           (config::video.virtual_display_layout == "adaptive" || config::video.virtual_display_layout == "primary");
#else
    return false;
#endif
  }

  token prepare(bool new_launch, int width, int height, int fps) {
#ifdef __APPLE__
    if (!enabled() || shutting_down()) {
      return {};
    }
    const auto &v = config::video;
    policy p;
    p.adaptive = v.virtual_display_layout == "adaptive";
    p.local_retain = v.virtual_display_local_disconnect == "retain";
    p.headless_retain = v.virtual_display_headless_disconnect == "retain";
    p.on_battery = v.virtual_display_retention_on_battery;
    p.retention = std::chrono::seconds(v.virtual_display_retention_seconds);
    p.power = v.virtual_display_retention_power;
    p.override_local = v.virtual_display_local_override == "present" ? presence::present :
                       v.virtual_display_local_override == "absent" ? presence::absent : presence::unknown;
    auto &r = state();
    if (r.desktop.snapshot().display) {
      BOOST_LOG(info) << "Preserving virtual desktop mode until Quit; negotiated client output uses media scaling";
    }
    auto t = r.desktop.prepare(new_launch, {width, height, fps}, p);
    if (t) {
      r.watch();
    } else {
      BOOST_LOG(warning) << "Virtual desktop preparation refused: check topology/override and helper health";
    }
    return t;
#else
    return {};
#endif
  }
  bool valid(token t) {
#ifdef __APPLE__
    return !shutting_down() && (!t || state().desktop.valid(t));
#else
    return true;
#endif
  }
  bool activate(token t) {
#ifdef __APPLE__
    return !shutting_down() && (!t || state().desktop.activate(t));
#else
    return true;
#endif
  }
  void established(token t) {
#ifdef __APPLE__
    if (t && !shutting_down()) {
      state().desktop.established(t);
    }
#endif
  }
  void finish(token t) {
#ifdef __APPLE__
    if (t) {
      if (shutting_down()) {
        state().desktop.close();
      }
      state().desktop.finish(t);
    }
#endif
  }
  void abort(token t) {
#ifdef __APPLE__
    if (t) {
      if (shutting_down()) {
        state().desktop.close();
      }
      state().desktop.abort(t);
    }
#endif
  }
  void revoke(uint64_t epoch) {
#ifdef __APPLE__
    state().desktop.revoke(epoch);
#endif
  }
  bool release_inactive() {
#ifdef __APPLE__
    return state().desktop.release_inactive();
#else
    return true;
#endif
  }
  status snapshot() {
#ifdef __APPLE__
    return state().desktop.snapshot();
#else
    return {};
#endif
  }
  void close() {
#ifdef __APPLE__
    state().desktop.close();
#endif
  }
  void shutdown() {
#ifdef __APPLE__
    state().stop();
#endif
  }
}  // namespace adaptive_display
