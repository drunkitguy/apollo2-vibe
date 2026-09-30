#include "src/platform/windows/display_helper_v2/diagnostics.h"

#include <mutex>
#include <utility>

namespace display_helper::v2::diagnostics {
  namespace {
    struct sink_state_t {
      std::mutex mutex;
      Sink sink;
    };

    // set_sink() runs from a static initializer in another translation unit
    // (diagnostics_boost.cpp), before or after this one's. With the MinGW
    // toolchain a namespace-scope std::mutex is initialized dynamically, so
    // locking one before its initializer ran fails with EINVAL and terminates
    // the process at startup. Construct the state on first use instead, and
    // never destroy it so late log calls during exit stay safe.
    sink_state_t &sink_state() {
      static auto *state = new sink_state_t {};
      return *state;
    }
  }  // namespace

  void set_sink(Sink next_sink) {
    auto &state = sink_state();
    std::lock_guard lock {state.mutex};
    state.sink = std::move(next_sink);
  }

  void emit(Level level, std::string message) {
    auto &state = sink_state();
    Sink active_sink;
    {
      std::lock_guard lock {state.mutex};
      active_sink = state.sink;
    }
    if (active_sink) {
      active_sink(level, message);
    }
  }
}  // namespace display_helper::v2::diagnostics
