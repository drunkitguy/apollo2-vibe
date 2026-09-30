/**
 * @file src/platform/windows/foreground_app.h
 * @brief Foreground window identity helpers for stream-scoped app matching.
 */
#pragma once

#include "game_activity_policy.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <winsock2.h>
#include <windows.h>

namespace platf::foreground_app {

  struct state_t {
    bool valid_window {false};
    bool shell_window {false};
    bool fullscreen_on_capture_display {false};
    bool has_active_app {false};
    bool matches_active_app {false};
    bool uses_playnite {false};
    bool tracks_active_app_window {false};
    DWORD foreground_pid {0};
    DWORD blocker_pid {0};
    RECT blocker_rect {};
    double blocker_coverage_percent {0.0};
    bool matching_window_seen {false};
    bool matching_game_fullscreen {false};
    std::uint32_t ignored_passive_window_count {0};
    std::string foreground_exe;
    std::string blocker_exe;
    std::string blocker_class;
    std::string blocker_title;
    std::string blocker_reason;
    std::string blocker_classification;
    bool blocker_opaque {false};
    bool blocker_framed {false};
    bool blocker_passive_overlay {false};
    bool blocker_desktop_ui {false};
    bool blocker_present {false};
    bool definite_desktop_blocker_present {false};
    DWORD definite_desktop_blocker_pid {0};
    std::string active_app_name;
    std::string active_app_exe;
    std::string source;
  };

  state_t snapshot(
    const std::optional<RECT> &capture_rect = std::nullopt,
    DWORD game_hint_pid = 0,
    std::string_view game_hint_exe = {}
  );

  /**
   * @brief Build a matcher for windows that belong to the running app.
   * @return A predicate over (pid, executable path), or `std::nullopt` when no app is running
   *         or the app cannot be tracked (for example the Desktop app or URL launches).
   */
  std::optional<std::function<bool(DWORD, std::string_view)>> active_app_window_matcher();

  /**
   * @brief True for the desktop, shell and Windows desktop UI windows (taskbar, Start, search).
   */
  bool is_desktop_ui_window(HWND hwnd, std::string_view executable);

  /**
   * @brief True for overlay windows that window-only capture must never select.
   *
   * Layered windows count only when they are click-through or non-activating, captionless and
   * topmost, or fully see-through. Other layered windows (skinned launchers, fade-ins) stay
   * eligible. A non-layered window counts when it is frameless and click-through or non-activating.
   * Used only for window-only target selection; the existing foreground checks are unchanged.
   */
  bool is_window_selection_overlay(HWND hwnd);

  bool path_equal_or_basename_match(std::string_view lhs, std::string_view rhs);
  bool path_is_under_directory(std::string_view path, std::string_view directory);
  bool playnite_foreground_matches_for_tests(
    std::string_view active_playnite_id,
    std::string_view status_id,
    std::string_view status_exe,
    std::string_view status_install_dir,
    std::string_view foreground_exe
  );
  using visible_window_evidence_t = game_activity_policy::visible_window_evidence_t;

  bool transient_shell_overlay_for_tests(
    std::string_view class_name,
    bool desktop_ui,
    bool covers_capture_display
  );

}  // namespace platf::foreground_app
