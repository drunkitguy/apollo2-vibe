/**
 * @file src/focus_hints.cpp
 * @brief Definitions for host text-input focus hints.
 */
// this include
#include "focus_hints.h"

// standard includes
#include <atomic>
#include <mutex>

// local includes
#include "config.h"
#include "logging.h"

using namespace std::literals;

namespace focus_hints {

  namespace {
    /// Guards `active` and `callback` against a session starting while another stops.
    std::mutex mtx;
    bool active = false;
    callback_t callback;
  }  // namespace

  kind_t kind_from_view(const std::string &value) {
    if (value == "text"sv) {
      return kind_t::text;
    }
    if (value == "numeric"sv) {
      return kind_t::numeric;
    }
    if (value == "password"sv) {
      return kind_t::password;
    }
    return kind_t::none;
  }

  const char *to_string(kind_t kind) {
    switch (kind) {
      case kind_t::none:
        return "none";
      case kind_t::text:
        return "text";
      case kind_t::numeric:
        return "numeric";
      case kind_t::password:
        return "password";
      default:
        return "unknown";
    }
  }

  bool start(callback_t cb) {
    if (!config::video.focus_hints) {
      return false;
    }

    std::lock_guard lg {mtx};

    if (active) {
      // One detector serves the host, not the session. A second concurrent session would
      // otherwise stand up a second UI Automation client for the same desktop.
      BOOST_LOG(debug) << "Focus hints: a monitor is already running"sv;
      return false;
    }

    callback = std::move(cb);

    if (!platf::start_monitor([](kind_t kind) {
          // Runs on the detector thread. The mutex is only ever held for the duration of
          // this hand-off, never across the platform call that produced it.
          callback_t local;
          {
            std::lock_guard lg {mtx};
            if (!active) {
              return;
            }
            local = callback;
          }
          if (local) {
            local(kind);
          }
        })) {
      BOOST_LOG(warning) << "Focus hints: no detector available on this platform"sv;
      callback = nullptr;
      return false;
    }

    active = true;
    BOOST_LOG(info) << "Focus hints: watching for text input focus"sv;
    return true;
  }

  void stop() {
    {
      std::lock_guard lg {mtx};
      if (!active) {
        return;
      }
      active = false;
    }

    // Outside the lock: stop_monitor() joins the detector thread, and that thread takes the
    // same mutex on its way through the callback shim.
    platf::stop_monitor();

    std::lock_guard lg {mtx};
    callback = nullptr;
  }

#ifndef _WIN32
  namespace platf {
    bool start_monitor(const std::function<void(kind_t)> &) {
      // Focus classification needs an accessibility API. Only Windows is implemented.
      return false;
    }

    void stop_monitor() {
    }
  }  // namespace platf
#endif

}  // namespace focus_hints
