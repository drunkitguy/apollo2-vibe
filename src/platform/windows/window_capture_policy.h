#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace platf::dxgi::window_policy {
  struct rect_i {
    std::int32_t left;
    std::int32_t top;
    std::int32_t right;
    std::int32_t bottom;
  };

  struct window_blit_t {
    bool empty;
    bool covers_output;
    std::uint32_t src_left;
    std::uint32_t src_top;
    std::uint32_t src_right;
    std::uint32_t src_bottom;
    std::uint32_t dst_x;
    std::uint32_t dst_y;
  };

  /**
   * @brief Map a WGC window frame onto the capture output at the window's true position.
   * @param output Capture display rectangle in desktop coordinates.
   * @param frame_bounds DWMWA_EXTENDED_FRAME_BOUNDS of the window, which is the origin of the WGC surface.
   * @param client_screen Client rectangle of the window in desktop coordinates.
   * @param content_w Valid width of the WGC surface (frame ContentSize clamped to the texture).
   * @param content_h Valid height of the WGC surface.
   */
  constexpr window_blit_t compute_window_blit(
    const rect_i output,
    const rect_i frame_bounds,
    const rect_i client_screen,
    const std::int32_t content_w,
    const std::int32_t content_h
  ) noexcept {
    window_blit_t blit {true, false, 0, 0, 0, 0, 0, 0};

    const std::int64_t output_w = std::int64_t {output.right} - output.left;
    const std::int64_t output_h = std::int64_t {output.bottom} - output.top;
    if (output_w <= 0 || output_h <= 0 || content_w <= 0 || content_h <= 0) {
      return blit;
    }

    // Intersect, in desktop coordinates, the client area, the valid part of the
    // WGC surface and the capture output.
    const std::int64_t left = std::max({std::int64_t {client_screen.left}, std::int64_t {frame_bounds.left}, std::int64_t {output.left}});
    const std::int64_t top = std::max({std::int64_t {client_screen.top}, std::int64_t {frame_bounds.top}, std::int64_t {output.top}});
    const std::int64_t right = std::min({std::int64_t {client_screen.right}, std::int64_t {frame_bounds.left} + content_w, std::int64_t {output.right}});
    const std::int64_t bottom = std::min({std::int64_t {client_screen.bottom}, std::int64_t {frame_bounds.top} + content_h, std::int64_t {output.bottom}});
    if (right <= left || bottom <= top) {
      return blit;
    }

    blit.empty = false;
    blit.src_left = static_cast<std::uint32_t>(left - frame_bounds.left);
    blit.src_top = static_cast<std::uint32_t>(top - frame_bounds.top);
    blit.src_right = static_cast<std::uint32_t>(right - frame_bounds.left);
    blit.src_bottom = static_cast<std::uint32_t>(bottom - frame_bounds.top);
    blit.dst_x = static_cast<std::uint32_t>(left - output.left);
    blit.dst_y = static_cast<std::uint32_t>(top - output.top);
    blit.covers_output = blit.dst_x == 0 && blit.dst_y == 0 &&
                         right - left == output_w && bottom - top == output_h;
    return blit;
  }

  struct window_candidate_t {
    std::uintptr_t id;
    bool matches_app;
    bool started_after_launch;
    bool is_foreground;
    bool iconic;
    std::int64_t client_area;
    bool new_since_launch = false;  ///< The window was not visible when the app was launched.
  };

  /**
   * @brief True when a process created at `process_created` started after the session launch.
   * Both values are FILETIME ticks; a zero launch time means no launch was recorded.
   */
  constexpr bool started_after_launch(const std::uint64_t process_created, const std::uint64_t launch_time) noexcept {
    return launch_time != 0 && process_created > launch_time;
  }

  /**
   * @brief How the running app's windows are recognized.
   */
  enum class selection_mode_e : std::uint8_t {
    app_matcher,  ///< Windows of the tracked app, plus windows of processes started after the launch.
    launched_app,  ///< The app has a command but cannot be tracked (URL or detached launch): only windows that appeared, or whose process started, after the launch.
    desktop,  ///< No app command (the Desktop app, or nothing running): follow the foreground window.
  };

  /**
   * @brief Pick the selection mode for the running app.
   * @param has_active_app An app session is active.
   * @param launches_process The app starts something: a command, detached commands, a Playnite game,
   * or any tracked process. The command-less Desktop app launches nothing.
   * @param have_app_matcher The app's own processes or install directory can be recognized.
   */
  constexpr selection_mode_e selection_mode_for(
    const bool has_active_app,
    const bool launches_process,
    const bool have_app_matcher
  ) noexcept {
    if (!has_active_app || !launches_process) {
      return selection_mode_e::desktop;
    }
    return have_app_matcher ? selection_mode_e::app_matcher : selection_mode_e::launched_app;
  }

  namespace detail {
    constexpr std::uintptr_t choose_among(
      const std::span<const window_candidate_t> candidates,
      const std::uintptr_t current,
      const selection_mode_e mode,
      const bool iconic
    ) noexcept {
      const auto eligible = [&](const window_candidate_t &c) {
        if (c.id == 0 || c.iconic != iconic) {
          return false;
        }
        switch (mode) {
          case selection_mode_e::app_matcher:
            return c.matches_app || c.started_after_launch;
          case selection_mode_e::launched_app:
            // A URL can open a new window in a launcher that was already running
            // (steam://open/bigpicture), so a new window counts even from an old process.
            return c.started_after_launch || c.new_since_launch;
          case selection_mode_e::desktop:
            return true;
        }
        return false;
      };

      for (const auto &c : candidates) {
        if (c.is_foreground && eligible(c)) {
          return c.id;
        }
      }
      if (current != 0) {
        for (const auto &c : candidates) {
          if (c.id == current && eligible(c)) {
            return c.id;
          }
        }
      }
      if (mode == selection_mode_e::desktop) {
        return 0;
      }

      // Candidates arrive in Z order, so the first of equally large windows wins.
      const window_candidate_t *best = nullptr;
      for (const auto &c : candidates) {
        if (eligible(c) && (best == nullptr || c.client_area > best->client_area)) {
          best = &c;
        }
      }
      return best != nullptr ? best->id : 0;
    }
  }  // namespace detail

  /**
   * @brief Pick the window to capture, or 0 for none (the stream shows black).
   *
   * In the app modes only eligible windows (see selection_mode_e) are considered:
   * foreground first, then the current target, then the largest client area. Windows
   * that existed before the launch and do not belong to the app are never chosen. In
   * desktop mode the foreground window is followed and the current target kept while it
   * remains a candidate. Minimized windows are only picked when nothing else qualifies.
   */
  constexpr std::uintptr_t choose_target(
    const std::span<const window_candidate_t> candidates,
    const std::uintptr_t current,
    const selection_mode_e mode
  ) noexcept {
    if (const auto id = detail::choose_among(candidates, current, mode, false); id != 0) {
      return id;
    }
    return detail::choose_among(candidates, current, mode, true);
  }

  enum class placement_action_e : std::uint8_t {
    none,  ///< Leave the window alone.
    restore,  ///< Restore a minimized or maximized window first, then plan again.
    set_rect,  ///< Move or resize the window to `window`.
  };

  struct placement_t {
    placement_action_e action;
    rect_i window;
  };

  /**
   * @brief Decide how to place the target so its client area fills the capture output.
   *
   * A window whose client area already covers the output (borderless fullscreen) is never
   * touched. A resizable window is sized so that its client area matches the output exactly;
   * its title bar and borders then lie outside the output, so no taskbar strip or frame is
   * left in the stream. A fixed-size window is only moved, client area first, when its client
   * area is not already inside the output.
   * @param output Capture display rectangle in desktop coordinates.
   * @param window_rect GetWindowRect of the window.
   * @param client_screen Client rectangle of the window in desktop coordinates.
   */
  constexpr placement_t plan_placement(
    const rect_i output,
    const rect_i window_rect,
    const rect_i client_screen,
    const bool resizable,
    const bool iconic,
    const bool zoomed
  ) noexcept {
    const placement_t leave {placement_action_e::none, window_rect};
    if (iconic) {
      return {placement_action_e::restore, window_rect};
    }
    if (client_screen.right <= client_screen.left || client_screen.bottom <= client_screen.top) {
      return leave;
    }

    const bool covers = client_screen.left <= output.left && client_screen.top <= output.top &&
                        client_screen.right >= output.right && client_screen.bottom >= output.bottom;
    if (covers) {
      return leave;
    }
    if (zoomed && resizable) {
      return {placement_action_e::restore, window_rect};
    }

    const std::int32_t inset_left = client_screen.left - window_rect.left;
    const std::int32_t inset_top = client_screen.top - window_rect.top;
    const std::int32_t inset_right = window_rect.right - client_screen.right;
    const std::int32_t inset_bottom = window_rect.bottom - client_screen.bottom;
    if (resizable) {
      return {
        placement_action_e::set_rect,
        {output.left - inset_left, output.top - inset_top, output.right + inset_right, output.bottom + inset_bottom},
      };
    }

    const bool inside = client_screen.left >= output.left && client_screen.top >= output.top &&
                        client_screen.right <= output.right && client_screen.bottom <= output.bottom;
    if (inside) {
      return leave;
    }
    const std::int32_t left = output.left - inset_left;
    const std::int32_t top = output.top - inset_top;
    return {
      placement_action_e::set_rect,
      {left, top, left + (window_rect.right - window_rect.left), top + (window_rect.bottom - window_rect.top)},
    };
  }

  struct target_debouncer_t {
    std::uintptr_t pending = 0;
    int stable_polls = 0;
    std::uintptr_t committed = 0;

    /**
     * @brief Feed one poll result.
     * @return True when the committed target changed.
     */
    constexpr bool observe(const std::uintptr_t candidate, const int polls_required) noexcept {
      if (candidate == pending) {
        if (stable_polls < polls_required) {
          ++stable_polls;
        }
      } else {
        pending = candidate;
        stable_polls = 1;
      }
      if (stable_polls >= polls_required && committed != pending) {
        committed = pending;
        return true;
      }
      return false;
    }
  };
}  // namespace platf::dxgi::window_policy
