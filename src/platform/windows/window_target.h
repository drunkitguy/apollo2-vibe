/**
 * @file src/platform/windows/window_target.h
 * @brief Choose, place and track the window captured by a window-only stream.
 */
#pragma once

// standard includes
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <vector>

// platform includes
#include <winsock2.h>
#include <windows.h>

// local includes
#include "window_capture_policy.h"

namespace platf::window_target {

  /**
   * @brief Record the start of an app launch. Windows of processes created later count as the app's.
   * Also remembers which top-level windows are visible now, so a window that appears later counts
   * as new even when its process is older (a URL opening a window in a launcher that is already
   * running). Taken for every launch, so a later window-only /resume still has it.
   */
  void mark_launch();

  /**
   * @brief Time of the last app launch, or zero when none was recorded.
   */
  FILETIME launch_time();

  struct choose_result_t {
    std::uintptr_t target = 0;  ///< Window handle, or `0` when nothing qualifies and the stream should be black.
    std::vector<std::uint32_t> seeds;  ///< Processes of visible windows that match the app (secondary window mode only).
    std::vector<std::uint32_t> excluded;  ///< Windows desktop UI processes seen during the scan (secondary window mode only).
  };

  /**
   * @brief Enumerate candidate windows and pick the target for the running app.
   * @param current The current target, kept while it stays eligible.
   * @param secondary_windows Treat owned windows and smaller windows of the target's process as
   * secondary windows of the app instead of target candidates, and collect the app's processes.
   * When false the selection is exactly the one without secondary window support.
   */
  choose_result_t choose(std::uintptr_t current, bool secondary_windows);

  /**
   * @brief Polls the window choice in the background, debounces changes, and places and
   * focuses each newly committed target.
   *
   * Every call that can block on another process (moving, restoring or focusing a window)
   * runs on the tracker thread, never on the capture thread.
   */
  class tracker_t {
  public:
    /**
     * @brief The committed target and the processes whose windows the stream may show.
     */
    struct window_set_t {
      std::uintptr_t target = 0;
      std::vector<std::uint32_t> pids;  ///< Sorted ascending; empty when there is no target.
      std::uint64_t generation = 0;  ///< Bumped whenever the target or the process list changes.
    };

    /**
     * @param output Capture display rectangle in desktop coordinates.
     * @param secondary_windows Show the app's menus, popups and dialogs as secondary windows:
     * changes the target rules, computes the app window set and moves the app's dialogs onto
     * the capture display. When false the tracker behaves exactly as without the feature.
     */
    explicit tracker_t(const RECT &output, bool secondary_windows = false);
    ~tracker_t();

    tracker_t(const tracker_t &) = delete;
    tracker_t &operator=(const tracker_t &) = delete;

    /**
     * @brief Choose a target synchronously and commit it without debouncing.
     * Placement and focus follow on the tracker thread.
     * @return Window handle, or `0` for none.
     */
    std::uintptr_t select_now();

    /**
     * @brief The last committed target, or `0` for none.
     */
    std::uintptr_t committed_target() const {
      return _committed.load(std::memory_order_acquire);
    }

    /**
     * @brief True when the app's secondary windows are shown (see the constructor).
     */
    bool secondary_windows() const {
      return _secondary_windows;
    }

    /**
     * @brief Copy of the current app window set. Empty unless secondary windows are shown.
     */
    window_set_t window_set() const;

    /**
     * @brief Generation of the current app window set; cheap to poll from the capture thread.
     */
    std::uint64_t window_set_generation() const {
      return _window_set_generation.load(std::memory_order_acquire);
    }

  private:
    void run(std::stop_token stop_token);
    void commit(std::uintptr_t hwnd);
    /**
     * @brief Recompute the app window set for the committed target. Call with `_mutex` held.
     */
    void update_window_set(const choose_result_t &choice);
    void place_secondary_windows(std::uintptr_t target, const std::vector<std::uint32_t> &pids);

    RECT _output;
    const bool _secondary_windows;
    mutable std::mutex _mutex;
    std::condition_variable_any _cv;
    bool _wake = false;  ///< A new target was committed outside the tracker thread.
    dxgi::window_policy::target_debouncer_t _debouncer;
    std::atomic<std::uintptr_t> _committed {0};
    std::uintptr_t _placed_target = 0;  ///< Target the placement state below belongs to.
    int _placement_steps = 0;  ///< Placement attempts made for `_placed_target`.
    bool _placement_done = false;
    bool _focused = false;
    window_set_t _window_set;  ///< Guarded by `_mutex`.
    std::atomic<std::uint64_t> _window_set_generation {0};
    std::uintptr_t _moved_for_target = 0;  ///< Target the moved-dialog list below belongs to (tracker thread only).
    std::unordered_set<std::uintptr_t> _moved_windows;  ///< Dialogs already moved onto the display (tracker thread only).
    std::jthread _thread;
  };

}  // namespace platf::window_target
