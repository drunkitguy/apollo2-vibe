/**
 * @file src/fec_adaptive.cpp
 * @brief Definitions for the loss-adaptive FEC controller. See fec_adaptive.h for the design.
 */
// this include
#include "fec_adaptive.h"

// standard includes
#include <algorithm>

// local includes
#include "config.h"
#include "logging.h"

using namespace std::literals;

namespace fec_adaptive {

  int next_percentage(
    int current,
    int loss_count,
    int &clean_reports,
    int ceiling_pct,
    int floor_pct
  ) {
    if (loss_count > 0) {
      // Any loss at all restores full protection immediately. No averaging, no threshold: by the
      // time a rate of loss is established the frames it damaged are already on screen.
      clean_reports = 0;
      return ceiling_pct;
    }

    if (++clean_reports < CLEAN_REPORTS_PER_STEP) {
      return current;
    }

    clean_reports = 0;
    return std::max(floor_pct, current - STEP_DOWN_PCT);
  }

  void begin_session(state_t &state) {
    state.ceiling_pct = config::stream.fec_percentage;
    state.floor_pct = std::min(config::stream.fec_percentage_min, state.ceiling_pct);
    state.clean_reports = 0;
    state.current_pct.store(state.ceiling_pct, std::memory_order_relaxed);
    state.engaged.store(false, std::memory_order_relaxed);

    if (config::stream.fec_adaptive) {
      BOOST_LOG(info) << "Adaptive FEC: enabled, "sv << state.floor_pct << "% to "sv
                      << state.ceiling_pct << "%, starting at the ceiling"sv;
    }
  }

  void on_loss_report(state_t &state, int loss_count, std::chrono::milliseconds interval) {
    if (!config::stream.fec_adaptive) {
      return;
    }

    auto before = state.current_pct.load(std::memory_order_relaxed);
    auto after = next_percentage(before, loss_count, state.clean_reports, state.ceiling_pct, state.floor_pct);

    if (after == before) {
      return;
    }

    state.current_pct.store(after, std::memory_order_relaxed);
    if (after < state.ceiling_pct) {
      state.engaged.store(true, std::memory_order_relaxed);
    }

    // Every adjustment is logged at info, on purpose. Adaptation masks configuration faults: a
    // controller that silently settles on a lower value is indistinguishable from a healthy link
    // unless it says what it did and why.
    if (after > before) {
      BOOST_LOG(info) << "Adaptive FEC: "sv << before << "% -> "sv << after
                      << "% after "sv << loss_count << " lost packets in "sv
                      << interval.count() << " ms"sv;
    } else {
      BOOST_LOG(info) << "Adaptive FEC: "sv << before << "% -> "sv << after
                      << "% after "sv << CLEAN_REPORTS_PER_STEP << " clean reports"sv;
    }
  }

}  // namespace fec_adaptive
