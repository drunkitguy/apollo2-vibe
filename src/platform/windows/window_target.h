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

// platform includes
#include <winsock2.h>
#include <windows.h>

// local includes
#include "window_capture_policy.h"

namespace platf::window_target {

  /**
   * @brief Record the start of an app launch. Windows of processes created later count as the app's.
   * @param snapshot_windows Also remember which top-level windows are visible now, so a window
   * that appears later counts as new even when its process is older (a URL opening a window in a
   * launcher that is already running). Only window-only sessions need it.
   */
  void mark_launch(bool snapshot_windows);

  /**
   * @brief Time of the last app launch, or zero when none was recorded.
   */
  FILETIME launch_time();

  /**
   * @brief Enumerate candidate windows and pick the target for the running app.
   * @param current The current target, kept while it stays eligible.
   * @return Window handle, or `0` when nothing qualifies and the stream should be black.
   */
  std::uintptr_t choose(std::uintptr_t current);

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
     * @param output Capture display rectangle in desktop coordinates.
     */
    explicit tracker_t(const RECT &output);
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

  private:
    void run(std::stop_token stop_token);
    void commit(std::uintptr_t hwnd);

    RECT _output;
    std::mutex _mutex;
    std::condition_variable_any _cv;
    bool _wake = false;  ///< A new target was committed outside the tracker thread.
    dxgi::window_policy::target_debouncer_t _debouncer;
    std::atomic<std::uintptr_t> _committed {0};
    std::uintptr_t _placed_target = 0;  ///< Target the placement state below belongs to.
    int _placement_steps = 0;  ///< Placement attempts made for `_placed_target`.
    bool _placement_done = false;
    bool _focused = false;
    std::jthread _thread;
  };

}  // namespace platf::window_target
