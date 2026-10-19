#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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
    std::uint32_t pid = 0;  ///< Process of the window; 0 disables the same-process rule for it.
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

      const window_candidate_t *current_candidate = nullptr;
      if (current != 0) {
        for (const auto &c : candidates) {
          if (c.id == current && eligible(c)) {
            current_candidate = &c;
            break;
          }
        }
      }

      for (const auto &c : candidates) {
        if (!c.is_foreground || !eligible(c)) {
          continue;
        }
        // A smaller foreground window of the target's own process (an unowned dialog or
        // message box) is drawn as a secondary window instead of replacing the target.
        if (current_candidate != nullptr && c.id != current_candidate->id && c.pid != 0 &&
            c.pid == current_candidate->pid && c.client_area < current_candidate->client_area) {
          continue;
        }
        return c.id;
      }
      if (current_candidate != nullptr) {
        return current_candidate->id;
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
   * A foreground window does not replace the current target when both have the same
   * non-zero `pid` and the foreground window's client area is smaller.
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

  /**
   * @brief Map the whole WGC surface of a secondary window (menu, popup, dialog) onto the
   * capture output. Unlike the target, a secondary window keeps its frame and title bar.
   * @param output Capture display rectangle in desktop coordinates.
   * @param frame_bounds DWMWA_EXTENDED_FRAME_BOUNDS of the window, the origin of its WGC surface.
   * @param content_w Valid width of the cached surface.
   * @param content_h Valid height of the cached surface.
   */
  constexpr window_blit_t compute_layer_blit(
    const rect_i output,
    const rect_i frame_bounds,
    const std::int32_t content_w,
    const std::int32_t content_h
  ) noexcept {
    return compute_window_blit(output, frame_bounds, frame_bounds, content_w, content_h);
  }

  /**
   * @brief State of one top-level window, as read by the capture helper.
   */
  struct app_window_t {
    std::uintptr_t id = 0;
    std::uint32_t pid = 0;  ///< Process whose content the window shows.
    bool visible = false;
    bool cloaked = false;
    bool iconic = false;
    bool root = false;  ///< GetAncestor(GA_ROOT) is the window itself.
    bool shadow = false;  ///< SysShadow, the system drop-shadow helper window.
    bool transparent = false;  ///< Layered with zero alpha.
    rect_i bounds {0, 0, 0, 0};  ///< DWMWA_EXTENDED_FRAME_BOUNDS.
  };

  struct layer_t {
    std::uintptr_t id = 0;
    bool is_target = false;
    bool below_target = false;  ///< A secondary window that lies below the target in z-order.
  };

  /**
   * @brief Maximum number of secondary windows drawn over the target.
   */
  constexpr std::size_t max_secondary_layers = 16;

  /**
   * @brief Maximum number of process ids in an app window set.
   */
  constexpr std::size_t max_app_pids = 62;

  constexpr bool rects_intersect(const rect_i a, const rect_i b) noexcept {
    return std::max(a.left, b.left) < std::min(a.right, b.right) &&
           std::max(a.top, b.top) < std::min(a.bottom, b.bottom);
  }

  /**
   * @brief True for a visible top-level window of the app, other than the target, that
   * lies at least partly on the capture output and is drawn as its own layer.
   * @param app_pids App process ids, sorted ascending.
   */
  constexpr bool is_secondary_window(
    const app_window_t &w,
    const std::uintptr_t target,
    const std::span<const std::uint32_t> app_pids,
    const rect_i output
  ) noexcept {
    if (w.id == 0 || w.id == target || w.pid == 0) {
      return false;
    }
    if (!w.visible || w.cloaked || w.iconic || !w.root || w.shadow || w.transparent) {
      return false;
    }
    if (!std::binary_search(app_pids.begin(), app_pids.end(), w.pid)) {
      return false;
    }
    if (w.bounds.right <= w.bounds.left || w.bounds.bottom <= w.bounds.top) {
      return false;
    }
    return rects_intersect(w.bounds, output);
  }

  /**
   * @brief Order the target and the app's secondary windows for compositing.
   *
   * Keeps the target (when it is in the list, whatever its state) and the topmost
   * secondary windows, at most `out.size() - 1` of them, and writes them bottom-up.
   * Secondary windows below the target are marked `below_target`.
   * @param windows_top_down Top-level windows in EnumWindows (top-down) order.
   * @param app_pids App process ids, sorted ascending.
   * @param out Receives the layers bottom-up; its size is the layer cap plus one.
   * @return Number of layers written.
   */
  constexpr std::size_t plan_layers(
    const std::span<const app_window_t> windows_top_down,
    const std::uintptr_t target,
    const std::span<const std::uint32_t> app_pids,
    const rect_i output,
    const std::span<layer_t> out
  ) noexcept {
    if (out.empty()) {
      return 0;
    }

    const std::size_t max_secondary = out.size() - 1;
    std::size_t count = 0;
    std::size_t secondary = 0;
    bool target_seen = false;
    // Collect top-down first, then reverse in place.
    for (const auto &w : windows_top_down) {
      if (target != 0 && w.id == target) {
        if (!target_seen) {
          target_seen = true;
          out[count++] = layer_t {w.id, true, false};
        }
        continue;
      }
      if (secondary >= max_secondary || !is_secondary_window(w, target, app_pids, output)) {
        continue;
      }
      out[count++] = layer_t {w.id, false, target_seen};
      ++secondary;
    }
    std::reverse(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
  }

  /**
   * @brief Build the sorted app process set sent to the capture helper.
   *
   * The target's process is kept first when the cap applies, then the seeds in order.
   * PID 0, duplicates and excluded PIDs are dropped. The result is sorted ascending.
   */
  inline std::vector<std::uint32_t> build_app_pids(
    const std::uint32_t target_pid,
    const std::span<const std::uint32_t> seeds,
    const std::span<const std::uint32_t> excluded,
    const std::size_t max_pids
  ) {
    std::vector<std::uint32_t> pids;
    const auto add = [&](const std::uint32_t pid) {
      if (pid == 0 || pids.size() >= max_pids ||
          std::find(excluded.begin(), excluded.end(), pid) != excluded.end() ||
          std::find(pids.begin(), pids.end(), pid) != pids.end()) {
        return;
      }
      pids.push_back(pid);
    };

    add(target_pid);
    for (const auto pid : seeds) {
      add(pid);
    }
    std::sort(pids.begin(), pids.end());
    return pids;
  }

  /**
   * @brief Where to move a dialog of the app so the stream shows it.
   * @param output Capture display rectangle in desktop coordinates.
   * @param window_rect GetWindowRect of the dialog.
   * @return The rectangle centred on the output (aligned to its top or left edge on an axis
   * where the window is larger) when less than half of the window lies on the output,
   * otherwise nothing.
   */
  constexpr std::optional<rect_i> plan_secondary_move(const rect_i output, const rect_i window_rect) noexcept {
    const std::int64_t window_w = std::int64_t {window_rect.right} - window_rect.left;
    const std::int64_t window_h = std::int64_t {window_rect.bottom} - window_rect.top;
    const std::int64_t output_w = std::int64_t {output.right} - output.left;
    const std::int64_t output_h = std::int64_t {output.bottom} - output.top;
    if (window_w <= 0 || window_h <= 0 || output_w <= 0 || output_h <= 0) {
      return std::nullopt;
    }

    const std::int64_t inside_w = std::max<std::int64_t>(
      0,
      std::min<std::int64_t>(window_rect.right, output.right) - std::max<std::int64_t>(window_rect.left, output.left)
    );
    const std::int64_t inside_h = std::max<std::int64_t>(
      0,
      std::min<std::int64_t>(window_rect.bottom, output.bottom) - std::max<std::int64_t>(window_rect.top, output.top)
    );
    if (2 * inside_w * inside_h >= window_w * window_h) {
      return std::nullopt;
    }

    const std::int64_t left = window_w <= output_w ? output.left + (output_w - window_w) / 2 : output.left;
    const std::int64_t top = window_h <= output_h ? output.top + (output_h - window_h) / 2 : output.top;
    return rect_i {
      static_cast<std::int32_t>(left),
      static_cast<std::int32_t>(top),
      static_cast<std::int32_t>(left + window_w),
      static_cast<std::int32_t>(top + window_h),
    };
  }

  /**
   * @brief True when a secondary layer may be drawn.
   *
   * A layer is drawn once it delivered a frame after it was last shown. A reused window
   * whose content did not change may never deliver one, so its cached frame is used once
   * `reuse_delay` has passed since the show. Times share one clock; zero means never.
   */
  constexpr bool layer_content_ready(
    const std::int64_t frame_at,
    const std::int64_t shown_at,
    const std::int64_t now,
    const std::int64_t reuse_delay
  ) noexcept {
    if (frame_at == 0) {
      return false;
    }
    return frame_at >= shown_at || now - shown_at >= reuse_delay;
  }
}  // namespace platf::dxgi::window_policy
