/**
 * @file src/platform/windows/window_target.cpp
 * @brief Choose, place and track the window captured by a window-only stream.
 */
#include "window_target.h"

// standard includes
#include <array>
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
#include "src/process.h"
#include "utf_utils.h"

using namespace std::literals;

namespace platf::window_target {
  namespace {
    namespace window_policy = dxgi::window_policy;

    constexpr auto poll_interval = 250ms;
    constexpr int polls_required = 3;
    constexpr LONG minimum_client_size = 64;
    // Placement is retried on later polls because restores and moves are asynchronous.
    constexpr int max_placement_steps = 8;
    // The app's matcher needs a Playnite status query; it changes only when an app starts or stops.
    constexpr auto app_selection_refresh = 1s;
    constexpr std::size_t max_cached_processes = 1024;

    std::atomic<std::uint64_t> g_launch_time {0};
    // Survives display reinitialization so the choice stays sticky across capture restarts.
    std::atomic<std::uintptr_t> g_last_target {0};
    // Last foreground window dropped as shell UI, so the drop is logged once.
    std::atomic<std::uintptr_t> g_last_logged_shell_drop {0};

    std::uint64_t filetime_ticks(const FILETIME &time) {
      return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    struct process_info_t {
      std::string executable;
      std::uint64_t created = 0;
    };

    /**
     * @brief Executable paths by process id and creation time, kept across polls.
     */
    class process_cache_t {
    public:
      process_info_t query(const DWORD pid) {
        process_info_t info;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
          return info;
        }

        FILETIME created {};
        FILETIME exited {};
        FILETIME kernel {};
        FILETIME user {};
        if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
          info.created = filetime_ticks(created);
        }

        if (info.created != 0) {
          std::lock_guard lock(_mutex);
          if (const auto cached = _entries.find(pid); cached != _entries.end() && cached->second.created == info.created) {
            CloseHandle(process);
            return cached->second;
          }
        }

        std::array<wchar_t, 32768> path {};
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
          try {
            info.executable = utf_utils::to_utf8(std::wstring(path.data(), size));
          } catch (...) {
          }
        }
        CloseHandle(process);

        if (info.created != 0) {
          std::lock_guard lock(_mutex);
          if (_entries.size() >= max_cached_processes) {
            _entries.clear();
          }
          _entries[pid] = info;
        }
        return info;
      }

    private:
      std::mutex _mutex;
      std::unordered_map<DWORD, process_info_t> _entries;
    };

    process_cache_t &process_cache() {
      static process_cache_t cache;
      return cache;
    }

    using app_matcher_t = std::function<bool(DWORD, std::string_view)>;

    struct app_selection_t {
      window_policy::selection_mode_e mode = window_policy::selection_mode_e::desktop;
      std::optional<app_matcher_t> matcher;
    };

    app_selection_t load_app_selection() {
      const auto app = proc::proc.running_app_state();
      const bool launches_process = !app.command.empty() || app.has_detached_commands ||
                                    app.uses_playnite || app.trackable;

      app_selection_t selection;
      selection.matcher = foreground_app::active_app_window_matcher();
      selection.mode = window_policy::selection_mode_for(app.has_active_app, launches_process, selection.matcher.has_value());
      return selection;
    }

    app_selection_t current_app_selection() {
      static std::mutex mutex;
      static app_selection_t cached;
      static std::chrono::steady_clock::time_point loaded_at {};
      static std::uint64_t loaded_for_launch = 0;

      const auto now = std::chrono::steady_clock::now();
      const auto launch = g_launch_time.load(std::memory_order_acquire);
      std::lock_guard lock(mutex);
      if (loaded_at == std::chrono::steady_clock::time_point {} || launch != loaded_for_launch ||
          now - loaded_at >= app_selection_refresh) {
        cached = load_app_selection();
        loaded_at = now;
        loaded_for_launch = launch;
      }
      return cached;
    }

    bool window_is_cloaked(HWND hwnd) {
      DWORD cloaked = 0;
      return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    }

    bool window_class_is(HWND hwnd, const std::wstring_view expected) {
      wchar_t class_name[256] {};
      const auto length = GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
      return length > 0 && std::wstring_view(class_name, static_cast<std::size_t>(length)) == expected;
    }

    /**
     * @brief The process whose content a top-level window shows.
     * A UWP app's top-level window belongs to ApplicationFrameHost; the app itself owns the
     * hosted CoreWindow child. Returns 0 for a frame without a hosted app (suspended or closing).
     */
    DWORD content_process_id(HWND hwnd) {
      DWORD pid = 0;
      if (!GetWindowThreadProcessId(hwnd, &pid)) {
        return 0;
      }
      if (!window_class_is(hwnd, L"ApplicationFrameWindow")) {
        return pid;
      }

      for (HWND child = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr); child != nullptr;
           child = FindWindowExW(hwnd, child, L"Windows.UI.Core.CoreWindow", nullptr)) {
        DWORD hosted_pid = 0;
        if (GetWindowThreadProcessId(child, &hosted_pid) && hosted_pid != 0 && hosted_pid != pid) {
          return hosted_pid;
        }
      }
      return 0;
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

    std::string describe_window(std::uintptr_t id);

    struct enum_context_t {
      const std::optional<app_matcher_t> &matcher;
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
      if ((ex_style & WS_EX_TOOLWINDOW) != 0 || foreground_app::is_passive_overlay_window(hwnd)) {
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

      const DWORD pid = content_process_id(hwnd);
      if (pid == 0 || pid == context.own_pid) {
        return TRUE;
      }

      auto process = context.processes.find(pid);
      if (process == context.processes.end()) {
        process = context.processes.emplace(pid, process_cache().query(pid)).first;
      }
      const auto &info = process->second;

      if (foreground_app::is_desktop_ui_window(hwnd, info.executable)) {
        const auto id = reinterpret_cast<std::uintptr_t>(hwnd);
        if (is_foreground && g_last_logged_shell_drop.exchange(id, std::memory_order_relaxed) != id) {
          BOOST_LOG(debug) << "Window-only: foreground window is shell UI and is never captured: "sv << describe_window(id);
        }
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
      const DWORD pid = content_process_id(hwnd);

      std::string executable = "unknown";
      if (pid != 0) {
        const auto info = process_cache().query(pid);
        if (!info.executable.empty()) {
          try {
            executable = std::filesystem::path(info.executable).filename().string();
          } catch (...) {
          }
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

    window_policy::rect_i to_rect_i(const RECT &rect) {
      return {rect.left, rect.top, rect.right, rect.bottom};
    }

    enum class placement_step_e {
      pending,  ///< Something was requested; check again on the next poll.
      done,  ///< The window is where it should be, or cannot be placed.
    };

    /**
     * @brief Take one placement step for the target. Runs on the tracker thread only.
     *
     * Every call is asynchronous (ShowWindowAsync, SWP_ASYNCWINDOWPOS) so a hung target
     * cannot block the tracker, and none restores a window that already covers the display
     * or changes its z-order band.
     */
    placement_step_e place_step(HWND hwnd, const RECT &output) {
      if (!IsWindow(hwnd)) {
        return placement_step_e::done;
      }
      if (IsHungAppWindow(hwnd)) {
        return placement_step_e::pending;
      }

      RECT window_rect {};
      RECT client {};
      POINT client_origin {0, 0};
      if (!GetWindowRect(hwnd, &window_rect) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &client_origin)) {
        return placement_step_e::done;
      }
      const window_policy::rect_i client_screen {
        client_origin.x,
        client_origin.y,
        client_origin.x + client.right,
        client_origin.y + client.bottom,
      };

      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      const bool resizable = (style & WS_THICKFRAME) != 0;
      const auto plan = window_policy::plan_placement(
        to_rect_i(output),
        to_rect_i(window_rect),
        client_screen,
        resizable,
        IsIconic(hwnd) != FALSE,
        IsZoomed(hwnd) != FALSE
      );

      switch (plan.action) {
        case window_policy::placement_action_e::none:
          return placement_step_e::done;
        case window_policy::placement_action_e::restore:
          ShowWindowAsync(hwnd, SW_RESTORE);
          return placement_step_e::pending;
        case window_policy::placement_action_e::set_rect:
          {
            UINT flags = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
            if (!resizable) {
              flags |= SWP_NOSIZE;
            }
            SetWindowPos(
              hwnd,
              nullptr,
              plan.window.left,
              plan.window.top,
              plan.window.right - plan.window.left,
              plan.window.bottom - plan.window.top,
              flags
            );
            return placement_step_e::pending;
          }
      }
      return placement_step_e::done;
    }

    /**
     * @brief Bring the target to the foreground without restoring it or touching its topmost state.
     * Runs on the tracker thread only. Skipped while the target or the current foreground window hangs.
     */
    void focus_window(HWND hwnd) {
      if (!IsWindow(hwnd) || GetForegroundWindow() == hwnd || IsHungAppWindow(hwnd)) {
        return;
      }
      if (SetForegroundWindow(hwnd)) {
        return;
      }

      // The foreground lock refuses the request; sharing the foreground thread's input
      // state lifts it. AttachThreadInput waits on that thread, so never attach to a hung one.
      HWND foreground = GetForegroundWindow();
      const DWORD foreground_thread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
      const DWORD own_thread = GetCurrentThreadId();
      if (foreground_thread == 0 || foreground_thread == own_thread || IsHungAppWindow(foreground)) {
        return;
      }
      if (AttachThreadInput(own_thread, foreground_thread, TRUE)) {
        SetForegroundWindow(hwnd);
        AttachThreadInput(own_thread, foreground_thread, FALSE);
      }
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
    const auto selection = current_app_selection();

    enum_context_t context {
      .matcher = selection.matcher,
      .launch_time = filetime_ticks(launch_time()),
      .foreground = GetForegroundWindow(),
      .own_pid = GetCurrentProcessId(),
      .processes = {},
      .candidates = {},
    };
    EnumWindows(enum_candidate, reinterpret_cast<LPARAM>(&context));

    return window_policy::choose_target(context.candidates, current, selection.mode);
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

    {
      std::lock_guard lock(_mutex);
      _debouncer = window_policy::target_debouncer_t {
        .pending = target,
        .stable_polls = polls_required,
        .committed = target,
      };
      commit(target);
      _wake = true;
    }
    _cv.notify_all();
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
      // Wakes early when a stop is requested or select_now committed a target.
      _cv.wait_for(lock, stop_token, poll_interval, [this] {
        return _wake;
      });
      if (stop_token.stop_requested()) {
        break;
      }

      if (_wake) {
        // select_now already chose; place the target before polling again.
        _wake = false;
      } else {
        const auto current = _debouncer.committed;
        lock.unlock();
        const auto candidate = choose(current);
        lock.lock();

        if (_debouncer.observe(candidate, polls_required)) {
          commit(_debouncer.committed);
        }
      }

      const auto target = _debouncer.committed;
      if (target != _placed_target) {
        _placed_target = target;
        _placement_steps = 0;
        _placement_done = target == 0;
        _focused = target == 0;
      }
      if (_placement_done && _focused) {
        continue;
      }

      const auto hwnd = reinterpret_cast<HWND>(target);
      lock.unlock();
      bool done = _placement_done;
      if (!done) {
        done = place_step(hwnd, _output) == placement_step_e::done;
      }
      // A minimized window is focused once it has been restored.
      const bool focus_now = !_focused && !IsIconic(hwnd);
      if (focus_now) {
        focus_window(hwnd);
      }
      lock.lock();

      if (_placed_target != target) {
        continue;
      }
      _focused = _focused || focus_now;
      if (!done && ++_placement_steps >= max_placement_steps) {
        BOOST_LOG(info) << "Window-only: target did not reach the capture display; leaving it where it is"sv;
        done = true;
      }
      _placement_done = done;
    }
  }

}  // namespace platf::window_target
