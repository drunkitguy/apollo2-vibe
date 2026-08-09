/**
 * @file src/fec_adaptive.h
 * @brief Loss-adaptive FEC percentage for the video stream.
 *
 * Apollo pays a fixed FEC overhead — 20% by default — on every frame regardless of whether the
 * link is losing anything. On a path whose capacity is the binding constraint that overhead
 * comes straight out of the latency budget: a measured 4K60 session on this project's hardware
 * delivered at 68-71 Mbit/s with the sender at 110% duty cycle and **0.00% loss**, so a fifth of
 * the bottleneck was being spent on recovery that never recovered anything.
 *
 * ## Why this needs no protocol change
 *
 * The FEC percentage is already per-frame on the wire. `fecPercentage` is a per-frame local in
 * the packetiser, Apollo already varies it (it drops to 0 for abnormally large frames), and the
 * value actually used is written into every packet's `fecInfo` field so the receiver reconstructs
 * from what the frame carries rather than from anything negotiated. This module only changes
 * where that per-frame number comes from.
 *
 * ## Control law, and why it is asymmetric this way round
 *
 * The dangerous direction is *down*: less FEC means a single lost packet costs a whole frame plus
 * an IDR recovery. The safe direction is *up*. So the asymmetry is the opposite of a congestion
 * controller's — **restore to the ceiling immediately on any loss, and step down only after a
 * sustained clean period.**
 *
 * One clean wired capture is not proof of a lossless network, and the client here is a handheld
 * that is usually on Wi-Fi. The floor is therefore deliberately non-zero and configurable, and
 * the default floor still carries real protection.
 *
 * ## Thread ownership
 *
 * | State | Written by | Read by | Discipline |
 * |---|---|---|---|
 * | `state_t::current_pct` | control stream thread, on each loss report | video broadcast thread, once per frame | `std::atomic`, relaxed |
 * | `state_t::clean_reports` | control stream thread only | control stream thread only | plain, single-threaded |
 *
 * The two threads never write the same field. A frame that races an update simply uses the older
 * percentage, which is harmless because the value it used is what goes into that frame's
 * `fecInfo`.
 */
#pragma once

#include <atomic>
#include <chrono>

namespace fec_adaptive {

  /**
   * @brief How many consecutive clean loss reports are required before stepping down.
   * @details Moonlight reports loss roughly twice a second, so this is very roughly five
   *          seconds of clean delivery per step. Deliberately unhurried: the cost of stepping
   *          down too eagerly is a visible artefact, and the cost of stepping down too slowly is
   *          a few percent of bandwidth.
   */
  constexpr int CLEAN_REPORTS_PER_STEP = 10;

  /// Percentage points removed per step down.
  constexpr int STEP_DOWN_PCT = 5;

  /**
   * @brief Controller state for one session.
   * @details Not copyable in spirit; one instance lives in the session.
   */
  struct state_t {
    /// Percentage the packetiser should use right now. Read on the broadcast thread.
    std::atomic<int> current_pct {0};

    /// Consecutive clean reports seen. Control stream thread only.
    int clean_reports {0};

    /// Highest percentage this controller may use, from `config::stream.fec_percentage`.
    int ceiling_pct {0};

    /// Lowest percentage this controller may use, from `config::stream.fec_percentage_min`.
    int floor_pct {0};

    /// Whether the controller ever stepped below the ceiling, for the trace metadata.
    std::atomic<bool> engaged {false};
  };

  /**
   * @brief Decide the next FEC percentage from one loss report.
   * @details Pure function so the control law can be unit tested without a session, a socket or
   *          a client. Does not clamp `current` into range on its own — a caller that changes the
   *          envelope mid-session is responsible for that, and no caller does.
   * @param current Percentage currently in use.
   * @param loss_count Packets the client reported lost since its last report.
   * @param clean_reports In/out. Consecutive clean reports so far; updated by this call.
   * @param ceiling_pct Upper bound, the configured `fec_percentage`.
   * @param floor_pct Lower bound, never crossed however clean the link looks.
   * @return Percentage to use for subsequent frames.
   */
  int next_percentage(
    int current,
    int loss_count,
    int &clean_reports,
    int ceiling_pct,
    int floor_pct
  );

  /**
   * @brief Initialise a session's controller from the current configuration.
   * @details Starts at the ceiling. Adaptation only ever reduces from there, so a session that
   *          never sees a clean stretch behaves exactly like the fixed-percentage host it
   *          replaces.
   * @param state Controller to initialise.
   */
  void begin_session(state_t &state);

  /**
   * @brief Feed one client loss report into the controller.
   * @details Control stream thread only. Logs every change, because an adaptive system that
   *          quietly compensates for a broken link is worse than a fixed one that fails loudly.
   * @param state Controller for the session.
   * @param loss_count Packets lost since the client's last report.
   * @param interval Time the client says that report covers, for the log line only.
   */
  void on_loss_report(state_t &state, int loss_count, std::chrono::milliseconds interval);

}  // namespace fec_adaptive
