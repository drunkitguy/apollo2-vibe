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
   */
  void mark_launch();

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
   * @brief Move a newly chosen window onto the capture display, maximize it when it is
   * resizable and does not already cover the display, and focus it.
   * @param hwnd The target window.
   * @param output Capture display rectangle in desktop coordinates.
   */
  void prepare_target(HWND hwnd, const RECT &output);

  /**
   * @brief Polls the window choice in the background and debounces changes.
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
    dxgi::window_policy::target_debouncer_t _debouncer;
    std::atomic<std::uintptr_t> _committed {0};
    std::jthread _thread;
  };

}  // namespace platf::window_target
