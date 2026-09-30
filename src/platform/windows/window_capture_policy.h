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
  };

  /**
   * @brief True when a process created at `process_created` started after the session launch.
   * Both values are FILETIME ticks; a zero launch time means no launch was recorded.
   */
  constexpr bool started_after_launch(const std::uint64_t process_created, const std::uint64_t launch_time) noexcept {
    return launch_time != 0 && process_created > launch_time;
  }

  namespace detail {
    constexpr std::uintptr_t choose_among(
      const std::span<const window_candidate_t> candidates,
      const std::uintptr_t current,
      const bool have_app_matcher,
      const bool iconic
    ) noexcept {
      const auto eligible = [&](const window_candidate_t &c) {
        if (c.id == 0 || c.iconic != iconic) {
          return false;
        }
        return !have_app_matcher || c.matches_app || c.started_after_launch;
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
      if (!have_app_matcher) {
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
   * With an app matcher only matching windows and windows of processes started after
   * the launch are eligible: foreground first, then the current target, then the
   * largest client area. Without a matcher the foreground window is followed and the
   * current target kept while it remains a candidate. Minimized windows are only
   * picked when no other window qualifies.
   */
  constexpr std::uintptr_t choose_target(
    const std::span<const window_candidate_t> candidates,
    const std::uintptr_t current,
    const bool have_app_matcher
  ) noexcept {
    if (const auto id = detail::choose_among(candidates, current, have_app_matcher, false); id != 0) {
      return id;
    }
    return detail::choose_among(candidates, current, have_app_matcher, true);
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
