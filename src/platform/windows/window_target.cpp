/**
 * @file src/platform/windows/window_target.cpp
 * @brief Choose, place and track the window captured by a window-only stream.
 */
#include "window_target.h"

// standard includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// platform includes
#include <dwmapi.h>

// local includes
#include "foreground_app.h"
#include "misc.h"
#include "src/logging.h"
#include "tools/playnite_launcher/focus_utils.h"
#include "utf_utils.h"

using namespace std::literals;

namespace platf::window_target {
  namespace {
    namespace window_policy = dxgi::window_policy;

    constexpr auto poll_interval = 250ms;
    constexpr int polls_required = 3;
    constexpr LONG minimum_client_size = 64;

    std::atomic<std::uint64_t> g_launch_time {0};
    // Survives display reinitialization so the choice stays sticky across capture restarts.
    std::atomic<std::uintptr_t> g_last_target {0};

    std::uint64_t filetime_ticks(const FILETIME &time) {
      return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    struct process_info_t {
      std::string executable;
      std::uint64_t created = 0;
    };

    process_info_t query_process(DWORD pid) {
      process_info_t info;

      std::wstring path;
      if (playnite_launcher::focus::get_process_image_path(pid, path)) {
        try {
          info.executable = utf_utils::to_utf8(path);
        } catch (...) {
        }
      }

      if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        FILETIME created {};
        FILETIME exited {};
        FILETIME kernel {};
        FILETIME user {};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
          info.created = filetime_ticks(created);
        }
        CloseHandle(process);
      }
      return info;
    }

    bool window_is_cloaked(HWND hwnd) {
      DWORD cloaked = 0;
      return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    }

    std::int64_t window_client_area(HWND hwnd, const bool iconic) {
      LONG width = 0;
      LONG height = 0;
      if (iconic) {
        // A minimized window has no client area; use its restored size instead.
        WINDOWPLACEMENT placement {};
        placement.length = sizeof(placement);
        if (!GetWindowPlacement(hwnd, &placement)) {
          return 0;
        }
        width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
        height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
      } else {
        RECT client {};
        if (!GetClientRect(hwnd, &client)) {
          return 0;
        }
        width = client.right - client.left;
        height = client.bottom - client.top;
      }

      if (width < minimum_client_size || height < minimum_client_size) {
        return 0;
      }
      return static_cast<std::int64_t>(width) * height;
    }

    struct enum_context_t {
      const std::optional<std::function<bool(DWORD, std::string_view)>> &matcher;
      std::uint64_t launch_time;
      HWND foreground;
      DWORD own_pid;
      std::unordered_map<DWORD, process_info_t> processes;
      std::vector<window_policy::window_candidate_t> candidates;
    };

    BOOL CALLBACK enum_candidate(HWND hwnd, LPARAM param) {
      auto &context = *reinterpret_cast<enum_context_t *>(param);

      if (!IsWindowVisible(hwnd) || window_is_cloaked(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return TRUE;
      }

      const auto ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      if ((ex_style & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
      }

      const bool is_foreground = hwnd == context.foreground;
      if (GetWindow(hwnd, GW_OWNER) != nullptr && !is_foreground) {
        return TRUE;
      }

      const bool iconic = IsIconic(hwnd) != FALSE;
      const auto client_area = window_client_area(hwnd, iconic);
      if (client_area == 0) {
        return TRUE;
      }

      DWORD pid = 0;
      if (!GetWindowThreadProcessId(hwnd, &pid) || pid == 0 || pid == context.own_pid) {
        return TRUE;
      }

      auto process = context.processes.find(pid);
      if (process == context.processes.end()) {
        process = context.processes.emplace(pid, query_process(pid)).first;
      }
      const auto &info = process->second;

      if (foreground_app::is_desktop_ui_window(hwnd, info.executable)) {
        return TRUE;
      }

      context.candidates.push_back(window_policy::window_candidate_t {
        .id = reinterpret_cast<std::uintptr_t>(hwnd),
        .matches_app = context.matcher && (*context.matcher)(pid, info.executable),
        .started_after_launch = window_policy::started_after_launch(info.created, context.launch_time),
        .is_foreground = is_foreground,
        .iconic = iconic,
        .client_area = client_area,
      });
      return TRUE;
    }

    std::string describe_window(std::uintptr_t id) {
      if (id == 0) {
        return "none";
      }

      const auto hwnd = reinterpret_cast<HWND>(id);
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);

      std::string executable = "unknown";
      std::wstring path;
      if (pid != 0 && playnite_launcher::focus::get_process_image_path(pid, path)) {
        try {
          executable = utf_utils::to_utf8(std::filesystem::path(path).filename().wstring());
        } catch (...) {
        }
      }

      std::string class_name;
      wchar_t class_buffer[256] {};
      if (GetClassNameW(hwnd, class_buffer, static_cast<int>(std::size(class_buffer))) > 0) {
        try {
          class_name = utf_utils::to_utf8(class_buffer);
        } catch (...) {
        }
      }

      // Titles are deliberately not logged; they can contain document names.
      return "pid=" + std::to_string(pid) + " exe=" + executable + " class=" + class_name;
    }

    bool rect_inside(const RECT &inner, const RECT &outer) {
      return inner.left >= outer.left && inner.top >= outer.top &&
             inner.right <= outer.right && inner.bottom <= outer.bottom;
    }

    bool rect_covers(const RECT &cover, const RECT &area) {
      return cover.left <= area.left && cover.top <= area.top &&
             cover.right >= area.right && cover.bottom >= area.bottom;
    }

    bool visible_frame_bounds(HWND hwnd, RECT &bounds) {
      return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))) ||
             GetWindowRect(hwnd, &bounds);
    }
  }  // namespace

  void mark_launch() {
    FILETIME now {};
    GetSystemTimeAsFileTime(&now);
    g_launch_time.store(filetime_ticks(now), std::memory_order_release);
    g_last_target.store(0, std::memory_order_release);
  }

  FILETIME launch_time() {
    const auto ticks = g_launch_time.load(std::memory_order_acquire);
    return FILETIME {
      .dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFull),
      .dwHighDateTime = static_cast<DWORD>(ticks >> 32),
    };
  }

  std::uintptr_t choose(std::uintptr_t current) {
    const auto matcher = foreground_app::active_app_window_matcher();

    enum_context_t context {
      .matcher = matcher,
      .launch_time = filetime_ticks(launch_time()),
      .foreground = GetForegroundWindow(),
      .own_pid = GetCurrentProcessId(),
      .processes = {},
      .candidates = {},
    };
    EnumWindows(enum_candidate, reinterpret_cast<LPARAM>(&context));

    return window_policy::choose_target(context.candidates, current, matcher.has_value());
  }

  void prepare_target(HWND hwnd, const RECT &output) {
    if (!hwnd || !IsWindow(hwnd)) {
      return;
    }

    if (IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_RESTORE);
    }

    RECT bounds {};
    if (visible_frame_bounds(hwnd, bounds) && !rect_covers(bounds, output)) {
      if (!rect_inside(bounds, output)) {
        if (IsZoomed(hwnd)) {
          ShowWindow(hwnd, SW_RESTORE);
          visible_frame_bounds(hwnd, bounds);
        }

        // SetWindowPos positions the full window rectangle, which includes the invisible
        // resize border; offset it so the visible frame starts at the display origin.
        RECT window_rect {};
        if (GetWindowRect(hwnd, &window_rect)) {
          const auto x = window_rect.left + (output.left - bounds.left);
          const auto y = window_rect.top + (output.top - bounds.top);
          SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }

      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      if ((style & WS_THICKFRAME) != 0 && (style & WS_MAXIMIZEBOX) != 0 && !IsZoomed(hwnd)) {
        ShowWindow(hwnd, SW_MAXIMIZE);
      }
    }

    playnite_launcher::focus::try_focus_hwnd(hwnd);
  }

  tracker_t::tracker_t(const RECT &output):
      _output(output) {
    _thread = std::jthread([this](std::stop_token stop_token) {
      run(stop_token);
    });
  }

  tracker_t::~tracker_t() {
    if (_thread.joinable()) {
      _thread.request_stop();
      _cv.notify_all();
      _thread.join();
    }
  }

  std::uintptr_t tracker_t::select_now() {
    const auto target = choose(g_last_target.load(std::memory_order_acquire));

    std::lock_guard lock(_mutex);
    _debouncer = window_policy::target_debouncer_t {
      .pending = target,
      .stable_polls = polls_required,
      .committed = target,
    };
    commit(target);
    return target;
  }

  void tracker_t::commit(std::uintptr_t hwnd) {
    _committed.store(hwnd, std::memory_order_release);
    g_last_target.store(hwnd, std::memory_order_release);
    BOOST_LOG(info) << "Window-only target: "sv << describe_window(hwnd);
  }

  void tracker_t::run(std::stop_token stop_token) {
    syncThreadDesktop();

    std::unique_lock lock(_mutex);
    while (!stop_token.stop_requested()) {
      // Wakes early only when a stop is requested.
      _cv.wait_for(lock, stop_token, poll_interval, [] {
        return false;
      });
      if (stop_token.stop_requested()) {
        break;
      }

      const auto current = _debouncer.committed;
      lock.unlock();
      const auto candidate = choose(current);
      lock.lock();

      if (_debouncer.observe(candidate, polls_required)) {
        commit(_debouncer.committed);
      }
    }
  }

}  // namespace platf::window_target
