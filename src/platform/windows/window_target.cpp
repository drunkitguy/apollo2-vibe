/**
 * @file src/platform/windows/window_target.cpp
 * @brief Choose, place and track the window captured by a window-only stream.
 */
#include "window_target.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    constexpr auto process_revalidate_interval = 1s;
    // Owner chains are short; the bound only guards against a cycle.
    constexpr int max_owner_walk = 32;
    constexpr std::size_t max_remembered_windows = 1024;
    // The System process; never part of an app.
    constexpr std::uint32_t system_pid = 4;

    std::atomic<std::uint64_t> g_launch_time {0};
    // Survives display reinitialization so the choice stays sticky across capture restarts.
    std::atomic<std::uintptr_t> g_last_target {0};
    // Last foreground window dropped as shell UI, so the drop is logged once.
    std::atomic<std::uintptr_t> g_last_logged_shell_drop {0};

    // Top-level windows that were visible when the app was launched, sorted. Null when no
    // snapshot could be taken, in which case no window counts as new.
    //
    // Every piece of shared state in this file is constructed on first use and never
    // destroyed, so neither static initialization order nor exit order can matter.
    struct launch_windows_state_t {
      std::mutex mutex;
      std::shared_ptr<const std::vector<std::uintptr_t>> windows;
    };

    launch_windows_state_t &launch_windows_state() {
      static auto *state = new launch_windows_state_t {};
      return *state;
    }

    std::shared_ptr<const std::vector<std::uintptr_t>> launch_windows() {
      auto &state = launch_windows_state();
      std::lock_guard lock(state.mutex);
      return state.windows;
    }

    /**
     * @brief Collect the visible top-level windows of the interactive "Default" desktop.
     * @return Null when the desktop cannot be opened or enumerated (for example while the
     * host is locked), so that no window counts as new.
     */
    std::shared_ptr<const std::vector<std::uintptr_t>> visible_top_level_windows() {
      HDESK desktop = OpenDesktopW(L"Default", 0, FALSE, DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
      if (!desktop) {
        BOOST_LOG(debug) << "Window-only: cannot open the Default desktop to record launch windows: "sv << GetLastError();
        return nullptr;
      }

      std::vector<std::uintptr_t> windows;
      const BOOL enumerated = EnumDesktopWindows(
        desktop,
        [](HWND hwnd, LPARAM param) -> BOOL {
          if (IsWindowVisible(hwnd)) {
            reinterpret_cast<std::vector<std::uintptr_t> *>(param)->push_back(reinterpret_cast<std::uintptr_t>(hwnd));
          }
          return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows)
      );
      const DWORD error = enumerated ? ERROR_SUCCESS : GetLastError();
      CloseDesktop(desktop);
      if (!enumerated) {
        BOOST_LOG(debug) << "Window-only: cannot enumerate the Default desktop to record launch windows: "sv << error;
        return nullptr;
      }

      std::sort(windows.begin(), windows.end());
      return std::make_shared<const std::vector<std::uintptr_t>>(std::move(windows));
    }

    std::uint64_t filetime_ticks(const FILETIME &time) {
      return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    }

    struct process_info_t {
      std::string executable;
      std::uint64_t created = 0;
    };

    /**
     * @brief Executable paths by process id and creation time, kept across polls.
     *
     * A cached entry is trusted for `process_revalidate_interval`; after that the process
     * creation time is read again so a reused process id is noticed.
     */
    class process_cache_t {
    public:
      process_info_t query(const DWORD pid) {
        const auto now = std::chrono::steady_clock::now();
        {
          std::lock_guard lock(_mutex);
          if (const auto cached = _entries.find(pid);
              cached != _entries.end() && now - cached->second.validated_at < process_revalidate_interval) {
            return cached->second.info;
          }
        }

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
          if (const auto cached = _entries.find(pid); cached != _entries.end() && cached->second.info.created == info.created) {
            CloseHandle(process);
            cached->second.validated_at = now;
            return cached->second.info;
          }
        }

        // Only cache misses get here; keep the long-path buffer off the stack.
        std::vector<wchar_t> path(32768);
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
          _entries[pid] = entry_t {info, now};
        }
        return info;
      }

    private:
      struct entry_t {
        process_info_t info;
        std::chrono::steady_clock::time_point validated_at;
      };

      std::mutex _mutex;
      std::unordered_map<DWORD, entry_t> _entries;
    };

    process_cache_t &process_cache() {
      static auto *cache = new process_cache_t {};
      return *cache;
    }

    // Secondary windows already logged, so each is logged once.
    struct logged_windows_t {
      std::mutex mutex;
      std::unordered_set<std::uintptr_t> ids;
    };

    logged_windows_t &logged_secondary_windows() {
      static auto *logged = new logged_windows_t {};
      return *logged;
    }

    bool first_time_secondary(const std::uintptr_t id) {
      auto &logged = logged_secondary_windows();
      std::lock_guard lock(logged.mutex);
      if (logged.ids.size() >= max_remembered_windows) {
        logged.ids.clear();
      }
      return logged.ids.insert(id).second;
    }

    using app_matcher_t = std::function<bool(DWORD, std::string_view)>;

    struct app_selection_t {
      window_policy::selection_mode_e mode = window_policy::selection_mode_e::desktop;
      std::optional<app_matcher_t> matcher;
      bool job_matcher = false;  ///< The matcher tests job membership (one job query per call).
    };

    app_selection_t load_app_selection() {
      const auto app = proc::proc.running_app_state();
      const bool launches_process = !app.command.empty() || app.has_detached_commands ||
                                    app.uses_playnite || app.trackable;

      app_selection_t selection;
      selection.matcher = foreground_app::active_app_window_matcher();
      selection.mode = window_policy::selection_mode_for(app.has_active_app, launches_process, selection.matcher.has_value());
      // Mirrors foreground_app::active_app_window_matcher(): Playnite apps match by path,
      // other trackable apps by job membership.
      selection.job_matcher = selection.matcher.has_value() && app.trackable && !app.uses_playnite;
      return selection;
    }

    struct app_selection_cache_t {
      std::mutex mutex;
      app_selection_t cached;
      std::chrono::steady_clock::time_point loaded_at {};
      std::uint64_t loaded_for_launch = 0;
    };

    app_selection_t current_app_selection() {
      static auto *cache = new app_selection_cache_t {};

      const auto now = std::chrono::steady_clock::now();
      const auto launch = g_launch_time.load(std::memory_order_acquire);
      std::lock_guard lock(cache->mutex);
      if (cache->loaded_at == std::chrono::steady_clock::time_point {} || launch != cache->loaded_for_launch ||
          now - cache->loaded_at >= app_selection_refresh) {
        cache->cached = load_app_selection();
        cache->loaded_at = now;
        cache->loaded_for_launch = launch;
      }
      return cache->cached;
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

    /**
     * @brief The topmost visible window in the owner chain of `hwnd`, or `hwnd` itself when
     * none of its owners is visible. A window with a visible owner is a secondary window
     * (dialog, owned popup) of that owner; owners that are all hidden (a hidden application
     * window owning the main form) do not make a window secondary.
     */
    HWND visible_owner_root(HWND hwnd) {
      HWND root = hwnd;
      HWND owner = GetWindow(hwnd, GW_OWNER);
      for (int step = 0; owner != nullptr && step < max_owner_walk; ++step) {
        if (IsWindowVisible(owner)) {
          root = owner;
        }
        owner = GetWindow(owner, GW_OWNER);
      }
      return root;
    }

    struct enum_context_t {
      const std::optional<app_matcher_t> &matcher;
      const std::vector<std::uintptr_t> *launch_windows;
      std::uint64_t launch_time;
      HWND foreground;
      DWORD own_pid;
      bool secondary_windows;  ///< Owned windows are secondary; candidates carry their process id.
      bool collect_seeds;  ///< Collect the processes of visible windows that match the app.
      std::unordered_map<DWORD, process_info_t> processes;
      std::vector<window_policy::window_candidate_t> candidates;
      std::uintptr_t foreground_root;  ///< Visible owner root of a foreground secondary window.
      std::vector<std::uint32_t> seeds;
      std::vector<std::uint32_t> excluded;
      std::vector<DWORD> checked_pids;  ///< Processes already checked for seeds.
      const std::vector<std::uint32_t> *job_pids;  ///< Sorted job members, queried once per poll; null to use the matcher.
    };

    /**
     * @brief Captioned without a maximize box (a dialog frame), or a standard dialog.
     */
    bool is_dialog_like(HWND hwnd) {
      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      return ((style & WS_CAPTION) == WS_CAPTION && (style & WS_MAXIMIZEBOX) == 0) || window_class_is(hwnd, L"#32770");
    }

    const process_info_t &process_of(enum_context_t &context, const DWORD pid) {
      auto process = context.processes.find(pid);
      if (process == context.processes.end()) {
        process = context.processes.emplace(pid, process_cache().query(pid)).first;
      }
      return process->second;
    }

    /**
     * @brief Record the process of a visible top-level window as an app process when it
     * matches the app, or as excluded when it is Windows desktop UI.
     */
    void collect_app_process(enum_context_t &context, HWND hwnd) {
      const DWORD pid = content_process_id(hwnd);
      if (pid == 0 || pid == context.own_pid ||
          std::find(context.checked_pids.begin(), context.checked_pids.end(), pid) != context.checked_pids.end()) {
        return;
      }
      context.checked_pids.push_back(pid);

      const auto &info = process_of(context, pid);
      const bool matches = context.job_pids != nullptr ?
                             std::binary_search(context.job_pids->begin(), context.job_pids->end(), static_cast<std::uint32_t>(pid)) :
                             (*context.matcher)(pid, info.executable);
      if (foreground_app::is_windows_desktop_ui_process(info.executable)) {
        context.excluded.push_back(pid);
      } else if (matches) {
        context.seeds.push_back(pid);
      }
    }

    BOOL CALLBACK enum_candidate(HWND hwnd, LPARAM param) {
      auto &context = *reinterpret_cast<enum_context_t *>(param);

      if (!IsWindowVisible(hwnd) || window_is_cloaked(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return TRUE;
      }

      // Seeds are collected before the tool-window and overlay filters: menus and popups
      // of a matching process make it part of the app too.
      if (context.collect_seeds) {
        collect_app_process(context, hwnd);
      }

      const auto ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
      if ((ex_style & WS_EX_TOOLWINDOW) != 0 || foreground_app::is_window_selection_overlay(hwnd)) {
        return TRUE;
      }

      const bool is_foreground = hwnd == context.foreground;
      if (context.secondary_windows) {
        // A dialog or owned popup is drawn above its owner as a secondary window and never
        // becomes the target. While it is active its owner counts as the foreground window.
        if (const HWND root = visible_owner_root(hwnd); root != hwnd) {
          const auto id = reinterpret_cast<std::uintptr_t>(hwnd);
          if (is_foreground) {
            context.foreground_root = reinterpret_cast<std::uintptr_t>(root);
          }
          if (first_time_secondary(id)) {
            BOOST_LOG(debug) << "Window-only: secondary window "sv << describe_window(id);
          }
          return TRUE;
        }
      } else if (GetWindow(hwnd, GW_OWNER) != nullptr && !is_foreground) {
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

      const auto &info = process_of(context, pid);

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
        .new_since_launch = context.launch_windows &&
                            !std::binary_search(context.launch_windows->begin(), context.launch_windows->end(), reinterpret_cast<std::uintptr_t>(hwnd)),
        .pid = context.secondary_windows ? static_cast<std::uint32_t>(pid) : 0u,
        .dialog_like = context.secondary_windows && is_dialog_like(hwnd),
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

      // Only a main window that can be maximized is sized to the display; dialogs and
      // other owned or fixed windows are only moved.
      const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      const bool resizable = (style & WS_THICKFRAME) != 0 && (style & WS_MAXIMIZEBOX) != 0 &&
                             GetWindow(hwnd, GW_OWNER) == nullptr;
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
    /**
     * @brief Focus for secondary window mode: leaves an active app alone and focuses the
     * target's modal dialog instead of the disabled target (window_policy::plan_focus).
     */
    void focus_app_window(HWND target, const std::vector<std::uint32_t> &app_pids) {
      if (!IsWindow(target)) {
        return;
      }

      window_policy::focus_state_t state {
        .target = reinterpret_cast<std::uintptr_t>(target),
        .target_enabled = IsWindowEnabled(target) != FALSE,
      };
      if (HWND foreground = GetForegroundWindow()) {
        state.foreground_root = reinterpret_cast<std::uintptr_t>(visible_owner_root(foreground));
      }
      if (HWND popup = GetLastActivePopup(target); popup != nullptr && popup != target) {
        const DWORD pid = content_process_id(popup);
        state.last_active_popup = reinterpret_cast<std::uintptr_t>(popup);
        state.popup_visible = IsWindowVisible(popup) != FALSE;
        state.popup_enabled = IsWindowEnabled(popup) != FALSE;
        state.popup_in_app = pid != 0 && std::binary_search(app_pids.begin(), app_pids.end(), static_cast<std::uint32_t>(pid));
      }

      if (const auto chosen = window_policy::plan_focus(state); chosen != 0) {
        focus_window(reinterpret_cast<HWND>(chosen));
      }
    }
  }  // namespace

  void mark_launch() {
    auto windows = visible_top_level_windows();
    {
      auto &state = launch_windows_state();
      std::lock_guard lock(state.mutex);
      state.windows = std::move(windows);
    }

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

  choose_result_t choose(std::uintptr_t current, const bool secondary_windows) {
    const auto selection = current_app_selection();
    const auto windows_at_launch = launch_windows();
    const bool collect_seeds = secondary_windows && selection.mode == window_policy::selection_mode_e::app_matcher &&
                               selection.matcher.has_value();
    std::vector<std::uint32_t> job_pids;
    if (collect_seeds && selection.job_matcher) {
      job_pids = proc::proc.running_app_pids();
    }

    enum_context_t context {
      .matcher = selection.matcher,
      .launch_windows = windows_at_launch.get(),
      .launch_time = filetime_ticks(launch_time()),
      .foreground = GetForegroundWindow(),
      .own_pid = GetCurrentProcessId(),
      .secondary_windows = secondary_windows,
      // Only the app matcher mode adds processes beyond the target's own.
      .collect_seeds = collect_seeds,
      .processes = {},
      .candidates = {},
      .foreground_root = 0,
      .seeds = {},
      .excluded = {},
      .checked_pids = {},
      .job_pids = collect_seeds && selection.job_matcher ? &job_pids : nullptr,
    };
    EnumWindows(enum_candidate, reinterpret_cast<LPARAM>(&context));

    if (context.foreground_root != 0) {
      for (auto &candidate : context.candidates) {
        if (candidate.id == context.foreground_root) {
          candidate.is_foreground = true;
        }
      }
    }

    return choose_result_t {
      .target = window_policy::choose_target(context.candidates, current, selection.mode),
      .seeds = std::move(context.seeds),
      .excluded = std::move(context.excluded),
    };
  }

  tracker_t::tracker_t(const RECT &output, const bool secondary_windows):
      _output(output),
      _secondary_windows(secondary_windows) {
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
    const auto choice = choose(g_last_target.load(std::memory_order_acquire), _secondary_windows);
    const auto target = choice.target;

    {
      std::lock_guard lock(_mutex);
      _debouncer = window_policy::target_debouncer_t {
        .pending = target,
        .stable_polls = polls_required,
        .committed = target,
      };
      commit(target);
      if (_secondary_windows) {
        update_window_set(choice);
      }
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

  tracker_t::window_set_t tracker_t::window_set() const {
    std::lock_guard lock(_mutex);
    return _window_set;
  }

  void tracker_t::update_window_set(const choose_result_t &choice) {
    const auto target = _debouncer.committed;
    std::vector<std::uint32_t> pids;
    if (target != 0) {
      auto excluded = choice.excluded;
      excluded.push_back(GetCurrentProcessId());
      excluded.push_back(system_pid);
      const DWORD target_pid = content_process_id(reinterpret_cast<HWND>(target));
      pids = window_policy::build_app_pids(target_pid, choice.seeds, excluded, window_policy::max_app_pids);
    }
    if (target == _window_set.target && pids == _window_set.pids && _window_set.generation != 0) {
      return;
    }

    _window_set.target = target;
    _window_set.pids = std::move(pids);
    _window_set.generation = _window_set_generation.load(std::memory_order_relaxed) + 1;
    _window_set_generation.store(_window_set.generation, std::memory_order_release);
    BOOST_LOG(debug) << "Window-only: app window set has "sv << _window_set.pids.size() << " process(es)"sv;
  }

  void tracker_t::place_secondary_windows(const std::uintptr_t target, const std::vector<std::uint32_t> &pids) {
    if (target != _moved_for_target || _moved_windows.size() >= max_remembered_windows) {
      _moved_for_target = target;
      _moved_windows.clear();
    }
    if (target == 0 || pids.empty()) {
      return;
    }

    struct dialogs_context_t {
      HWND target;
      HWND foreground;
      const std::vector<std::uint32_t> &pids;
      std::vector<HWND> dialogs;
    } context {reinterpret_cast<HWND>(target), GetForegroundWindow(), pids, {}};

    // Dialogs of the app: captioned, not tool windows, and owned or active. Popups and menus
    // are never moved, nor are the app's other main windows (an inactive unowned window).
    EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto &context = *reinterpret_cast<dialogs_context_t *>(param);
        if (hwnd == context.target || !IsWindowVisible(hwnd) || IsIconic(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd ||
            (GetWindow(hwnd, GW_OWNER) == nullptr && hwnd != context.foreground)) {
          return TRUE;
        }
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const auto ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((style & WS_CAPTION) != WS_CAPTION || (ex_style & WS_EX_TOOLWINDOW) != 0 || window_is_cloaked(hwnd)) {
          return TRUE;
        }
        const DWORD pid = content_process_id(hwnd);
        if (pid != 0 && std::binary_search(context.pids.begin(), context.pids.end(), static_cast<std::uint32_t>(pid))) {
          context.dialogs.push_back(hwnd);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&context)
    );

    for (HWND hwnd : context.dialogs) {
      const auto id = reinterpret_cast<std::uintptr_t>(hwnd);
      if (_moved_windows.contains(id) || IsHungAppWindow(hwnd)) {
        continue;
      }
      RECT window_rect {};
      if (!GetWindowRect(hwnd, &window_rect)) {
        continue;
      }
      const auto moved = window_policy::plan_secondary_move(to_rect_i(_output), to_rect_i(window_rect));
      if (!moved) {
        continue;
      }

      _moved_windows.insert(id);
      SetWindowPos(
        hwnd,
        nullptr,
        moved->left,
        moved->top,
        0,
        0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS
      );
      BOOST_LOG(debug) << "Window-only: moved a secondary window onto the capture display: "sv << describe_window(id);
    }
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
        const auto choice = choose(current, _secondary_windows);
        lock.lock();

        if (_debouncer.observe(choice.target, polls_required)) {
          commit(_debouncer.committed);
        }
        if (_secondary_windows) {
          update_window_set(choice);
        }
      }

      if (_secondary_windows) {
        const auto app_target = _window_set.target;
        const auto app_pids = _window_set.pids;
        lock.unlock();
        place_secondary_windows(app_target, app_pids);
        lock.lock();
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
      const auto focus_pids = _secondary_windows ? _window_set.pids : std::vector<std::uint32_t> {};
      lock.unlock();
      bool done = _placement_done;
      if (!done) {
        done = place_step(hwnd, _output) == placement_step_e::done;
      }
      // A minimized window is focused once it has been restored.
      const bool focus_now = !_focused && !IsIconic(hwnd);
      if (focus_now) {
        if (_secondary_windows) {
          focus_app_window(hwnd, focus_pids);
        } else {
          focus_window(hwnd);
        }
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
