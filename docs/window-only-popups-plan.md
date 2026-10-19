# Window-only capture: menus, popups and dialogs of the app (host 1.2.0)

Status: implemented for host 1.2.0 (kill switch `window_only_secondary_windows`, default `enabled`). Base: branch `claude/clever-clarke-k7hhhj` at `0aaad3e` (release `window-host-v1.1.1`). Line numbers below refer to that commit. Read together with `docs/window-only-client-plan.md`, which describes the window-only design this plan extends.

## 1. Problem

In a window-only session (client sends `windowOnly=1`) the stream shows exactly one top-level window, the committed target. With a Qt 6 desktop app (the user's case is an emulator front end) this breaks normal use:

- Clicking a menu-bar or toolbar item opens a dropdown menu on the host, but the stream never shows it.
- If the user hits "Emulation > Configure" blind, the Configure dialog does not appear in a usable way either, so controller layouts cannot be changed.

General requirement: every secondary top-level window of the launched app (Qt and Win32 menus and submenus, combo box dropdowns, tooltips, context menus, modal and modeless dialogs, file pickers, owned windows, and windows of the app's child processes) must appear in the stream at its true position and in its true stacking order relative to the app's other windows. The desktop, wallpaper, taskbar, system UI and windows of other apps must stay out of the stream, exactly as today. The secure desktop exception (UAC, lock screen shown through DXGI so prompts can be answered) stays as is.

## 2. Diagnosis: why secondary windows are missing today

### 2.1 Capture is one HWND by construction

- The helper creates exactly one window capture item from `config_data_t::target_hwnd` with `IGraphicsCaptureItemInterop::CreateForWindow` (`tools/sunshine_wgc_capture.cpp:2640-2652`) and runs a single frame pool and session on it (`deps.graphics_item = window_item`, lines 2700-2716; `create_capture_session()`, line 2128). The monitor item created at lines 2582-2625 is only used for the shared texture size; no monitor session runs in window mode.
- `CreateForWindow` captures the DWM visual of that one window. Separate top-level windows (popups, menus, dialogs) are not part of it, even when they are owned by it. This is documented behaviour of single-window capture and the reason OBS users are told to use display capture for menus, and why WebRTC's GDI window capturer collects owned popups separately and composites them itself (see sources, section 13).
- The composite copies only the target's client area (`query_window_blit()`, lines 1469-1507, using `window_policy::compute_window_blit()`, `src/platform/windows/window_capture_policy.h:34-69`; `composite_window_frame()`, lines 1540-1551). Everything outside that rectangle is cleared to black. So even if a popup's pixels were somehow in the frame, they would be cleared.

Conclusion for menus: a Qt dropdown or context menu can never appear with the 1.1.1 helper, whatever the target logic does.

### 2.2 The target logic filters menus out, and switching is far too slow for them

Even as a target, popups are excluded before selection in `enum_candidate()` (`src/platform/windows/window_target.cpp:307-360`):

- Line 315: `WS_EX_TOOLWINDOW` windows are dropped. Qt creates every `Qt::Popup` (QMenu, the QComboBox dropdown container, QToolButton menus) and every `Qt::ToolTip` as `WS_POPUP` with `WS_EX_TOOLWINDOW` (the `else { exStyle |= WS_EX_TOOLWINDOW; }` branch of `WindowCreationData::fromWindow()` in `qwindowswindow.cpp`). Win32 popup menus (class `#32768`), combo list boxes (`ComboLBox`) and `tooltips_class32` windows also carry `WS_EX_TOOLWINDOW`.
- Line 315 also drops anything `foreground_app::is_window_selection_overlay()` accepts (`src/platform/windows/foreground_app.cpp:782-796`): a layered window that is captionless and topmost is an "overlay". Qt makes every popup topmost (`flags |= Qt::WindowStaysOnTopHint; // a popup stays on top, no parent.` and `SetWindowPos(hwnd, HWND_TOPMOST, ...)` in `WindowCreationData::initialize()`), and a popup with a translucent background (rounded menus of the Windows 11 style, any `WA_TranslucentBackground` popup) is layered. So translucent Qt menus are dropped twice.
- Line 320: owned windows are dropped unless they are the foreground window. Qt popups are shown with `SW_SHOWNOACTIVATE` and never become foreground.
- Lines 324-328: client areas under 64x64 are dropped (tooltips, small menus).
- Qt popups are not owned at all (QTBUG-34503: "a popup stays on top, no parent", and `updateTransientParent()` returns early for `Qt::Popup`). Any rule that finds secondary windows through `GW_OWNER` alone misses every Qt menu. Process membership is the only reliable link.
- Even if a popup were eligible, the change is debounced over 3 polls of 250 ms (`window_target.cpp:36-37`, `target_debouncer_t`, `window_capture_policy.h:259-283`), then gated by a 2 s cooldown since the last capture start (`display_wgc.cpp:222-237`), then the whole capture is rebuilt through `capture_e::reinit` (`display_wgc.cpp:314` and `:582`, `video.cpp` reinit path from line 2930, which also resets the encoders). That is 0.5 to 0.75 s of debounce plus 0.5 to 1.5 s of restart, so a menu that lives for a second or two would at best appear as it closes, and the main window would vanish while it is shown.

### 2.3 What happens with the Configure dialog

The app's Configure dialog is a `QDialog` with the main window as parent. Qt turns the transient parent into the Win32 owner (`parentHandle = handleOf(transientParent)`), so it is an owned `WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU` window with a class such as `Qt<version>QWindowIcon`. `exec()` activates it, so it becomes the foreground window. Following the code:

1. `enum_candidate()` keeps it because it is the foreground window (line 320 exception), it is not a tool window, not layered and larger than 64x64.
2. `choose_target()` returns the eligible foreground window first (`window_capture_policy.h:140-144`), so the dialog is chosen over the main window in every mode (it matches the app's job or started after the launch).
3. After 3 equal polls it is committed (`tracker_t::run()`, `window_target.cpp:599-601`), logged as `Window-only target: pid=... class=Qt...QWindowIcon`, placed (only moved, because owned windows are never resizable, lines 433-435) and focused.
4. The next `snapshot()` after the 2 s cooldown returns `reinit`; the helper restarts on the dialog.

So the target does switch to the dialog rather than never switching, but the result is unusable, which matches the report:

- For 1 to 3.5 s after the dialog opens the stream still shows the main window (frozen during the restart), so the dialog "does not appear" when the user looks for it.
- When it does appear, only its client area is drawn (no title bar, no close button), on black, because the main window is no longer captured.
- Every dropdown inside it (controller type, input device, profile combo boxes) is invisible for the reasons in 2.2, so controller settings still cannot be changed.
- Nested dialogs (input profile dialogs owned by the Configure dialog) trigger another switch and restart; closing each one triggers one more (the helper exits on `GraphicsCaptureItem.Closed`, lines 2630-2645).
- A dialog that is shown without activation, or that loses foreground to its own popup or to the user tapping elsewhere, is dropped by line 320 and the stream goes back to the main window, which is disabled while the modal dialog is open.

The exact run on the user's PC can be confirmed from the host log: a `Window-only target:` line with a `QWindowIcon` class followed by `Window-only target changed; restarting capture` means the switch happened; no such line means the dialog never became the foreground window. Implementation step 7.2 adds a debug line for windows that are classified as secondary so the next log answers this directly.

## 3. How the relevant windows are built on Windows

| Window | Owner | Process | Styles | Activation |
| --- | --- | --- | --- | --- |
| Qt menu, toolbar menu, combo dropdown (`Qt::Popup`) | none | app | `WS_POPUP`, `WS_EX_TOOLWINDOW`, topmost; layered when translucent; class `Qt<version>QWindowPopup...SaveBits` (often with `DropShadow`) | `SW_SHOWNOACTIVATE`, Qt keeps the native window and reuses the HWND when the same menu opens again |
| Qt tooltip (`Qt::ToolTip`) | transient parent if any | app | `WS_POPUP`, `WS_EX_TOOLWINDOW`, topmost | no activation |
| Qt dialog (`QDialog` with parent) | parent window | app | `WS_POPUP`, caption, frame, `WS_SYSMENU`, class `Qt<version>QWindowIcon` | activated |
| Win32 popup menu | none or the menu owner | app (created on the app's thread) | class `#32768`, `WS_POPUP`, `WS_EX_TOOLWINDOW`, topmost; drop shadow in a separate `SysShadow` window | no activation |
| Win32 combo dropdown, tooltip | varies | app | `ComboLBox`, `tooltips_class32`, `WS_EX_TOOLWINDOW`, topmost | no activation |
| Common file dialog (`IFileDialog`) | the app window passed in | app (in-process shell code) | normal dialog, its own combo and autocomplete popups | activated |
| Child process window (launcher hands off, helper tools) | none | descendant process | any | any |

Two consequences: secondary windows must be found by process, not only by owner; and the composite must honour the real z-order between them, because topmost popups sit above the dialog that opened them, which sits above the main window.

## 4. Options

### 4.1 (a) Multi-window composite

One WGC window session per relevant window (main target plus every visible secondary window of the app), each delivering frames independently; the helper composites them bottom-up in z-order into the existing display-sized texture.

- Leak guarantee: structural. Every non-black pixel comes from a WGC window item of an HWND that was positively classified as an app window, so nothing of another app or the shell can appear, whatever lies on top of the app. This is the same property 1.1.1 has, extended from one HWND to a set.
- Latency: a new session must start before a newly shown popup appears. Item creation is cheap; the first frame normally arrives with the next DWM composition after `StartCapture()`, but no documented bound exists (WebRTC waits up to 5 s for a first frame to cover slow starts). Mitigations: event-driven discovery (no 250 ms poll), sessions kept alive for hidden-but-alive app windows (Qt reuses menu HWNDs, so from the second opening the session already exists), and first-frame latency logging so it can be measured on the user's PC.
- GPU cost: menus and dialogs are small and static, so their sessions rarely deliver frames. While secondary windows exist, one extra display-sized copy per main-window frame (section 7.4). With no secondary window, the path is the 1.1.1 path, byte for byte.
- Yellow capture border: `IsBorderRequired(false)` is already attempted per session (lines 2179-2190). Where the OS still draws a border, it is on the host monitor only, never in the frame.
- Complexity: a session set in the helper, per-window cached textures, a recomposite trigger when a window hides or moves without new content, and event hooks. Contained in the helper and a pure policy header.

### 4.2 (b) Masked display capture

Capture the whole display and black out everything outside the app's visible region (union of app window rectangles minus foreign windows above them).

- Leak guarantee: none in practice. The frame starts as the desktop, and the mask is computed from window state that is never in sync with the composed frame. A foreign window that disappears or moves between DWM composing the frame and the mask query leaks its last frame (toasts fading out, Alt-Tab closing, a dragged window). DWM animations draw windows outside their logical rectangles: the minimize animation of another app flies across the screen after the window is already iconic, open and close animations scale windows, and shadows extend beyond the frame bounds. Cloaking and uncloaking (Start, Search, virtual desktops) adds more races. Hysteresis (subtract every foreign rectangle seen in the last N ms) narrows but does not close these windows, and a single bug in the region code exposes the desktop.
- Latency: popups appear in the very next frame, which is its only real advantage.
- Other: per-frame region computation, DPI and extended frame bounds handling, and an app window covered by a foreign window turns into a black hole.

### 4.3 (c) Hybrid

Window capture for the main window, masked display capture only while secondary windows exist. It inherits the leak risk of (b) exactly when the user is interacting with menus and dialogs, and adds a source switch with its own glitches. Rejected for the same reason as (b).

### 4.4 Decision

Implement (a). The original hard requirement is that the desktop never reaches the stream; only (a) keeps that guarantee by construction. The cost of (a) is a short delay before a brand-new popup appears, which is reduced by the event-driven design, reuse of sessions for reused HWNDs, and the fast path, and which is measured before release (acceptance criterion 10.3). Games without popups keep the exact 1.1.1 path, so there is no added latency for them.

Contingency, not part of 1.2.0: if the measured first-frame latency for new popups is above about 150 ms, a follow-up may fill only the rectangle of a just-shown app popup from a monitor capture for the frames before its own session delivers, and only when no foreign window intersects that rectangle above it. This would be off by default and needs its own review.

Also part of the decision: the committed target no longer switches to dialogs and popups. They are drawn as secondary layers above the main window, so no capture restart is needed for them (section 6.3).

## 5. Behaviour after the change

- The main target is chosen, placed and focused as today, except that owned windows, popups and tool windows never become the target.
- Every visible app window that intersects the capture display is drawn at its true position in its true z-order among the app's windows. The main target is drawn as today (client area only, frame and title bar are off the display after placement). Secondary windows are drawn whole (their `DWMWA_EXTENDED_FRAME_BOUNDS` rectangle, which is the WGC surface), so dialogs keep their title bar and close button.
- Windows of other processes are never drawn. Where they overlap the app, the app's own content shows through, as in 1.1.1.
- Menus open and close in the stream without a capture restart; dialogs no longer replace the main window.
- When an app window hides, its layer disappears immediately even if no window produces a new frame.
- Dialogs that open entirely or mostly off the capture display are moved onto it once; popups are never moved.

## 6. The app window set

### 6.1 Definitions

Inputs, all in the main process tracker except where noted: committed target `T` with process `t`; capture display rectangle `O`; launch time `L` (`mark_launch()`); app matcher `M` (`foreground_app::active_app_window_matcher()`); selection mode (`window_policy::selection_mode_for()`).

**App process set `P`** (computed in the main process, sent to the helper):

1. Empty when `T == 0` (the stream is black; nothing is drawn).
2. Seeds: `t`, plus in `app_matcher` mode the process of every visible top-level window whose process satisfies `M` (job membership through `proc::proc.running_app_contains_pid()`, or the Playnite install directory).
3. No process-tree closure (manager decision). Processes the app starts inside its job already satisfy `M` and become seeds; descendants that broke away from the job are not added. This removes the Toolhelp snapshot, creation-time checks and their PID-reuse risk.
4. Exclusions: this process, PID 0 and 4, and processes whose executable is Windows desktop UI (`foreground_app::process_is_windows_desktop_ui()` list: explorer, StartMenuExperienceHost, SearchHost, ShellExperienceHost, TextInputHost, LockApp, SystemSettings, ApplicationFrameHost).
5. Deliberately not a membership rule: "process started after launch". It remains a target selection rule (launchers that start the game outside the job), but an unrelated process that merely started later (an updater, a chat app, a notification host) must not get its popups into the stream. Its windows can still become `T` under the existing rules, and then `P` is rebuilt around the new `t`.
6. Desktop app mode (no command, foreground follow): seeds are `t` only.
7. `launched_app` mode (URL launches): seeds are `t` only. If `T` is a new window of a launcher that was already running (for example a big-picture launcher opened by URL), that launcher's other windows count as app windows, which matches what the user launched.

`P` is recomputed at most once per second, or immediately when `T` changes or a new seed PID appears; it is capped at 62 PIDs (IPC message size, section 7.3), seeds first.

**Secondary window**: a top-level window `w != T` (in the helper) such that

- `IsWindowVisible(w)`, not cloaked (`DWMWA_CLOAKED == 0`), not iconic, `GetAncestor(w, GA_ROOT) == w`;
- the process of `w` is in `P` (for `ApplicationFrameWindow`, the hosted CoreWindow's process, as in `content_process_id()`);
- its extended frame bounds intersect `O` with non-zero area;
- it is not a `SysShadow` window (the system's drop-shadow helper, which WebRTC also skips because it captures as solid black bars) and not a layered window with zero alpha;
- no size minimum and no tool-window or topmost filter: menus, tooltips and combo dropdowns are exactly the windows we want.

**Layer order**: the z-order of `EnumWindows` (top-down) restricted to `{T} ∪ secondary windows`, drawn bottom-up. At most 16 secondary layers; when more qualify, the 16 topmost are kept and a warning is logged once.

### 6.2 Foreign windows and system UI

- Windows of processes outside `P`, including other apps, overlays of other processes, notifications, Start, Search, Alt-Tab, the taskbar and the touch keyboard, are never drawn. Because layers come from per-window capture, their position above or below the app does not matter: they cannot leak, and the app's content below them stays visible.
- System-hosted dialogs that run in other processes (credential prompts, the "Open with" picker, emoji panel) stay black. Documented limitation.
- The secure desktop keeps the 1.1.1 behaviour (`window_only_dxgi_fallback_state()`, `display_wgc.cpp:73-81`).

### 6.3 Target selection changes (main process)

- **Owner rule**: replace the "owned unless foreground" filter (`window_target.cpp:319-322`) with a visible owner walk: follow `GW_OWNER` and stop at the first owner that is visible. A window with a visible owner is secondary and never a candidate, foreground or not. If it is the foreground window, the foreground flag is inherited by its visible owner root, so the main window keeps winning while its dialog is active. A window whose owners are all hidden (Delphi/VCL main forms owned by a hidden application window, Win32 apps with a hidden owner) stays a candidate as today.
- **Same-process rule**: in `detail::choose_among()` (`window_capture_policy.h:117-164`) the eligible foreground candidate does not replace an eligible current target of the same process when its client area is smaller than the current target's. This keeps unowned dialogs and message boxes (created without a parent) from forcing a capture restart, while a bigger new main window of the same process, or a window of a different process (launcher to game hand-off), still wins as today.
- Tool windows and overlays stay excluded from candidates (line 315 unchanged).

## 7. Implementation

All new behaviour is gated on `config.window_only` (main process) and on a new IPC flag (helper). Display capture and non-window-only sessions are unchanged.

Kill switch (manager decision): new host config key `window_only_secondary_windows` (boolean, default `enabled`) in `src/config.h/.cpp`, readable from `sunshine.conf` without a web UI field. When disabled, the host behaves exactly as 1.1.1: no `WGC_IPC_FLAG_WINDOW_SET`, no window-set messages, no layer manager, and the 6.3 target-selection changes are skipped (the old owner and foreground rules apply). The release notes say how to set it. Logs never include window titles (they can contain file names); describe windows by PID, executable base name and class only.

### 7.1 Pure policy (`src/platform/windows/window_capture_policy.h`)

Header-only, no Windows headers, same namespace `platf::dxgi::window_policy`. Add `#include <vector>`.

1. `window_candidate_t` (line 71): append `std::uint32_t pid = 0;` (defaulted, so existing aggregate initializers and tests stay valid).
2. `detail::choose_among()`: apply the same-process rule from 6.3 in the foreground loop (lines 140-144): skip a foreground candidate when `current` is eligible, `pid != 0`, `pid == current.pid` and `client_area < current.client_area`.
3. `constexpr window_blit_t compute_layer_blit(rect_i output, rect_i frame_bounds, std::int32_t content_w, std::int32_t content_h)`: the whole WGC surface of a secondary window (`frame_bounds` origin, `content` size) clipped to `output`. It can reuse `compute_window_blit()` with `client_screen = frame_bounds`.
4. Window descriptor and layer planning:

   ```cpp
   struct app_window_t {
     std::uintptr_t id;
     std::uint32_t pid;
     bool visible, cloaked, iconic, root;
     bool shadow;        ///< SysShadow class
     bool transparent;   ///< layered with zero alpha
     rect_i bounds;      ///< DWMWA_EXTENDED_FRAME_BOUNDS
   };
   struct layer_t { std::uintptr_t id; bool is_target; };
   constexpr bool is_secondary_window(const app_window_t &w, std::uintptr_t target, std::span<const std::uint32_t> app_pids, rect_i output) noexcept;
   /// windows_top_down in EnumWindows order; writes layers bottom-up, returns the count
   constexpr std::size_t plan_layers(std::span<const app_window_t> windows_top_down, std::uintptr_t target,
                                     std::span<const std::uint32_t> app_pids, rect_i output,
                                     std::span<layer_t> out) noexcept;  // out.size() = max layers + 1
   ```

   `plan_layers` keeps the target (if present in the list) and the topmost secondary windows up to `out.size() - 1`, and returns them bottom-up.
5. `std::vector<std::uint32_t> build_app_pids(std::uint32_t target_pid, std::span<const std::uint32_t> seeds, std::span<const std::uint32_t> excluded, std::size_t max_pids)`: target first, duplicates and excluded PIDs removed, capped, then sorted for binary search in the helper.
6. The visible owner walk needs live window queries, so it stays in `window_target.cpp`. Add instead `constexpr std::optional<rect_i> plan_secondary_move(rect_i output, rect_i window_rect)` (include `<optional>`): returns the window rectangle centred on `output` (top-left aligned when larger) when less than half of `window_rect`'s area lies inside `output`, otherwise nothing.

### 7.2 Main process tracker (`src/platform/windows/window_target.h/.cpp`)

1. `enum_candidate()` (lines 307-360):
   - Replace lines 319-322 with the visible owner walk (helper `HWND visible_owner_root(HWND)`, loop bounded to 32 steps). When it returns another window: record `context.foreground_root = root` if `is_foreground`, log once per HWND at debug level `Window-only: secondary window <describe_window>`, and return.
   - Fill `.pid = pid` in the candidate.
   - Collect, before the tool-window and overlay filters, the PIDs of visible root windows whose process satisfies the matcher into `context.matched_pids` (seeds for `P`). Use the existing `process_cache()`; no extra process opens.
2. `choose()` (lines 521-537): after `EnumWindows`, set `is_foreground = true` on the candidate whose `id == context.foreground_root`. Return the chosen target and the seed PIDs (change the signature to return a small struct, `choose_result_t { std::uintptr_t target; std::vector<std::uint32_t> seeds; }`).
3. `P` is the sorted, de-duplicated union of `t` and the seed PIDs, minus the exclusions, capped at 62 with `t` first.
4. `tracker_t` (header lines 51-91): add
   - `struct window_set_t { std::uintptr_t target; std::vector<std::uint32_t> pids; std::uint64_t generation; };`
   - `window_set_t window_set() const;` (copy under `_mutex`) and `std::uint64_t window_set_generation() const` (atomic, cheap for the capture thread).
   - In `run()` (lines 577-638), after `observe()`/commit: compute `P` for the committed target (empty for 0) and bump the generation when the sorted PID list or the target changed. `select_now()` computes it synchronously too, so the first helper start has it.
5. Secondary placement in `run()`: once per poll, `EnumWindows` over visible root windows of `P` that have `WS_CAPTION`, are not tool windows, are not `T`, and have not been moved before (a small `std::unordered_set<std::uintptr_t>` cleared when `T` changes). For each, `plan_secondary_move(output, frame bounds)`; when it returns a rectangle, `SetWindowPos(..., SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS)`, skipping hung windows. Never touches popups (no caption) and never resizes.
6. `focus_window()` and `place_step()` unchanged.

### 7.3 IPC (`src/platform/windows/ipc/pipes.h`, `ipc_session.h/.cpp`)

1. `wgc_ipc_config_flags_e` (pipes.h:67-72): add `WGC_IPC_FLAG_WINDOW_SET = 1u << 4` ("the main process sends app window sets; composite secondary windows").
2. New runtime message after `activity_admission_data_t` (pipes.h:120-127):

   ```cpp
   constexpr uint32_t WGC_WINDOW_SET_MESSAGE_MAGIC = 0x57475753;  // "WGWS"
   constexpr uint32_t WGC_WINDOW_SET_MAX_PIDS = 62;
   struct window_set_data_t {
     uint32_t magic;
     uint32_t pid_count;
     uint64_t target_hwnd;  ///< Target this set belongs to; the helper ignores a set for another target
     uint32_t pids[WGC_WINDOW_SET_MAX_PIDS];  ///< Sorted ascending
   };
   static_assert(sizeof(window_set_data_t) == 264);
   static_assert(offsetof(window_set_data_t, target_hwnd) == 8);
   static_assert(offsetof(window_set_data_t, pids) == 16);
   static_assert(sizeof(window_set_data_t) != sizeof(config_data_t));
   static_assert(sizeof(window_set_data_t) != sizeof(activity_admission_data_t));
   ```

   Messages are told apart by size in the helper's `handle_ipc_message()` (sunshine_wgc_capture.cpp:2377); these asserts keep the sizes distinct. `config_data_t` stays 128 bytes and unchanged.
3. `ipc_session_t`: add `bool set_window_set(std::uintptr_t target, std::span<const std::uint32_t> pids)` modelled on `set_activity_admission_fps()` (ipc_session.cpp:489-506): store the latest set under a mutex, send when the pipe is up, and resend right after the helper connects (next to the admission resend at lines 484-486). In the config fill (lines 346-349) also OR `WGC_IPC_FLAG_WINDOW_SET` when `_config.window_only`.
4. `display_wgc.cpp`: new helper `sync_window_set(tracker, ipc_session, last_sent_generation)` in the anonymous namespace next to `window_target_needs_reinit()` (line 222), called in both `snapshot()` implementations right after the reinit check (lines 314 and 582). It compares the atomic generation and only then copies the set and calls `set_window_set()`. Add `std::uint64_t _last_window_set_generation = 0;` to both classes in `display.h` (next to lines 548-549 and 656-661), reset in `init()`.

### 7.4 Helper (`tools/sunshine_wgc_capture.cpp`)

1. Globals (near line 138): `g_window_set` (mutex, PIDs, target, generation atomic). `handle_ipc_message()`: new branch for `sizeof(window_set_data_t)`; validate magic, `pid_count <= WGC_WINDOW_SET_MAX_PIDS` and sorted order; store; log at debug level (counts only). Extend the "Received config data" log with `window_set: yes/no`.
2. New class `WindowLayerManager` (owned by `main()`, used only when `WGC_IPC_FLAG_WINDOW_CAPTURE` and `WGC_IPC_FLAG_WINDOW_SET` are set and a target window item exists):
   - Event hooks on the helper's main thread, same pattern as `setup_desktop_switch_hook()` (line 2476), `WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS`, all processes (`idProcess = 0`): `EVENT_OBJECT_SHOW`, `EVENT_OBJECT_HIDE`, `EVENT_OBJECT_DESTROY`, `EVENT_OBJECT_REORDER`, `EVENT_OBJECT_CLOAKED`, `EVENT_OBJECT_UNCLOAKED`, `EVENT_SYSTEM_MENUPOPUPSTART`, `EVENT_SYSTEM_MENUPOPUPEND`, `EVENT_SYSTEM_FOREGROUND`, `EVENT_SYSTEM_MINIMIZESTART`, `EVENT_SYSTEM_MINIMIZEEND`. `EVENT_OBJECT_LOCATIONCHANGE` is registered separately with `idProcess` set to the target's PID only (re-registered when the target changes), so cursor and caret moves of other processes, including games, never reach the helper; moves of other app processes are picked up by the 100 ms scan. The callback does almost nothing: ignore `idObject != OBJID_WINDOW` or `idChild != CHILDID_SELF` first (LOCATIONCHANGE also fires for the cursor and carets), then mark the state dirty (and remember the HWND for hide and destroy). No window queries and no WinRT calls inside the callback.
   - The main loop (line 2743) calls `layers.update()` when dirty, when the window set generation changed, and every 100 ms as a safety net for lost events. Replace the 1 ms `sleep_for` with `MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT)` so hook events are handled as soon as they are posted.
   - `update()`: `EnumWindows` into `app_window_t` descriptors for windows whose PID is in the set (PID check first, other queries only for matches), call `window_policy::plan_layers()`, then:
     - create a layer for each new secondary HWND: `create_window_capture_item()` (line 751), a free-threaded frame pool of 1 buffer (2 if frames are dropped) in the capture format, `CreateCaptureSession`, `IsCursorCaptureEnabled` default, `IsBorderRequired(false)` and `MinUpdateInterval` as in `create_capture_session()` (lines 2128-2200; factor that tail into a shared function), `StartCapture()`; register `Closed` to drop the layer (it must not end the helper, unlike the target's `Closed`);
     - keep layers of windows that are hidden but alive (marked inactive, never drawn) so a reused HWND (Qt menus) resumes without a new session; destroy on `EVENT_OBJECT_DESTROY` or `Closed`; at most 24 sessions in total, least recently shown inactive ones dropped first;
     - publish the ordered layer list (bottom-up, with each layer's frame bounds) to the capture manager under a small mutex, and request a recomposite when the visible list or any bounds changed.
   - Each layer keeps `com_ptr<ID3D11Texture2D> cached` (its last frame, content-sized) and `shown_at`/`frame_at` QPC stamps. A layer is drawn only after it delivered a frame newer than its latest show event, or 100 ms after the show when the HWND was reused and nothing new arrived (then the cached content is used). On a layer frame: copy the content box into `cached` under `_d3d_context_mutex`, resize the pool after release when `ContentSize` changed (same pattern as `resize_window_frame_pool()`, line 1447), then request a recomposite. Log the first-frame delay of each new layer at info level (`Secondary window 0x... first frame after N ms`).
3. `WgcCaptureManager` changes (class from line 1091):
   - New members: `_base` (display-sized texture plus RTV, same format and bind flags as scratch), `_base_valid`, `_last_composited_scratch` (index), a pointer to the published layer list, and `_recomposite_pending` (atomic).
   - `queue_frame_for_delivery()` (line 1898), target frame:
     - No visible layer: exactly today's path (`composite_window_frame()` into scratch). Remember the scratch index; set `_base_valid = false`.
     - Visible layers: composite the target into `_base` (clear plus copy, `composite_window_frame()` on `_base`), set `_base_valid = true`, then `compose_layers(scratch)`.
   - `compose_layers(scratch)` under `_d3d_context_mutex`: `CopyResource(scratch, _base)`, then for each visible secondary layer bottom-up `CopySubresourceRegion` from `cached` with `compute_layer_blit()`. Layers that sit below the target in z-order are drawn into `_base` before the target instead (the plan marks them), so the target covers them correctly.
   - `request_recomposite()` (called from layer frames and from `update()`): if `_base_valid` is false and the last published frame had no layers, seed `_base` with `CopyResource` from `_last_composited_scratch` (that frame is exactly the target alone on black), then reserve a scratch slot, `compose_layers()`, enqueue with the current QPC. If no scratch slot is free, leave `_recomposite_pending` set; the main loop retries every millisecond.
   - When the last visible layer goes away, recomposite once from `_base` alone (so a closed menu disappears at once), then return to the fast path.
   - At compose time re-check `IsWindowVisible` and the PID of each secondary HWND (cheap, outside the context lock, as `query_window_blit()` does today), so a hidden window or a reused HWND of another process is never drawn even before its event is processed.
4. Alpha: secondary layers are copied opaque. Transparent corners and the transparent margin of translucent Qt popups come out black (premultiplied zero), which is safe. Alpha blending of layered popups through a small pixel shader is a follow-up, not part of 1.2.0, to keep the helper free of a shader pipeline.
5. Black mode (no target) and display mode: no `WindowLayerManager`, no change.
6. `tools/CMakeLists.txt` (lines 101-114): no new libraries needed (`user32`, `dwmapi` already linked).

### 7.5 Documentation and release

1. `.github/window-only-host-version.txt`: `1.2.0`.
2. `.github/workflows/window-only-release.yml` release body: add a "What's new in 1.2.0" section (menus, dropdowns, tooltips, context menus and dialogs of the launched app now appear at their true position; dialogs no longer replace the main window and no longer restart capture; other apps and the desktop stay hidden); replace the first "Known limitations" bullet (line 185) with: "Windows of the app are shown, including menus, dropdowns and dialogs. Prompts hosted by Windows itself in other processes (credential prompts, the Open with picker, the emoji panel) stay black. Translucent menu edges may show a thin black border. A brand-new popup can appear a few frames after it opens on the host."
3. `docs/window-only-client-plan.md`: update the "Launchers and popups" row in 2.3, the popups row in section 9 and the last follow-up in section 10 to point to this plan.

## 8. Unit tests (`tests/unit/test_window_capture_policy.cpp`)

Registered already as `test_component_window_capture_policy` (`tests/CMakeLists.txt:392`), built on Linux and run on the Windows runner by `ci-windows.yml:672-682`. New cases:

- Selection: an owned-but-smaller foreground dialog of the target's process does not replace the target (same-process rule); a larger foreground window of the same process does; a foreground window of another eligible process still wins (launcher hand-off unchanged); a candidate with `pid == 0` behaves as before.
- `compute_layer_blit`: popup fully inside the output; popup straddling the right edge (clipped); dialog larger than the output; window on another display (empty); content smaller than bounds during resize; output away from the desktop origin and with negative coordinates.
- `is_secondary_window` / `plan_layers`: target plus Qt-style unowned topmost popup of the same PID is planned above the target; popup of a foreign PID is never planned; hidden, cloaked, iconic, non-root, `SysShadow` and zero-alpha windows are skipped; window outside the output skipped; z-order preserved bottom-up with a secondary below the target marked as below; more than the cap keeps the topmost; target missing from the list (minimized) still yields the visible secondary windows; empty PID set yields only the target.
- `P` building (pure helper `build_app_pids(t, seeds, excluded, max)`): `t` first, duplicates removed, excluded PIDs dropped, cap respected, result sorted.
- `plan_secondary_move`: dialog fully inside (nothing); dialog on another monitor (centred); dialog larger than the output (top-left aligned); half inside boundary.
- Debouncer, placement and existing blit tests unchanged.

The IPC struct asserts compile into both binaries; `RtspStartupSnapshot` is unaffected.

## 9. CI

- `window-only-release.yml` triggers on the push (paths `src/**`, `tools/**`, `tests/**`, `.github/window-only-host-version.txt`). With version 1.2.0 the `check` job sets `publish=true`, the `windows` job builds host, helper and tests, runs the window-only policy tests and the startup smoke test (sunshine.exe under gdb for 25 s), and the `release` job publishes `window-host-v1.2.0` with `Vibepollo-WindowOnly-1.2.0.msi` and its `.sha256`.
- The Linux test build of `test_component_window_capture_policy` must keep compiling without Windows headers.
- Nothing in the startup path changes (the tracker and the layer manager only run in a window-only stream), so the smoke test must pass unchanged; any failure blocks the release.

## 10. Acceptance criteria

Build:

1. `window-only-release.yml` succeeds on `claude/clever-clarke-k7hhhj` and publishes `window-host-v1.2.0` with the MSI and `.sha256`.
2. `test_component_window_capture_policy` passes on Linux and on the Windows runner, including all new cases; the startup smoke test passes.

Manual test on the user's PC (Thor client, 1920x1080 at 120 FPS, virtual display, window-only on). Host log at debug level.

3. Qt 6 app, menus: tap each menu-bar menu (File, Emulation, View, Tools, Help). The dropdown appears at its true position under the menu-bar item, submenus cascade, hover highlight follows the finger or mouse, tapping an item works, and the menu disappears from the stream as soon as it closes. Log shows `Secondary window ... first frame after N ms`; target for this check: N below 100 ms for the first opening and the menu visible without a perceptible delay from the second opening on. No `Window-only target changed` line while menus open and close.
4. Qt 6 app, Configure: "Emulation > Configure" shows the dialog with its title bar and close button over the still-visible main window, within the time it takes on the host. The Controls page works: the controller type and input device dropdowns open and are visible, a per-button mapping can be changed, input profile sub-dialogs open on top, tooltips show. Closing returns to the main window with no capture restart (no `restarting capture` line).
5. Qt 6 app, file picker: "File > Load File" (or the equivalent open dialog) shows the common file dialog with its file type dropdown, the address bar autocomplete popup and the right-click context menu in the file list.
6. Qt 6 app, toolbar or game-list context menu: right-click (long press) menus appear.
7. Win32 app: Notepad menus, the Save As dialog and the Font dialog with its dropdowns appear. A classic Win32 menu bar (`msinfo32.exe` File and View menus, which use `#32768` menus) and the right-click menu of a text field in `charmap.exe` appear.
8. No regression for games: a borderless-fullscreen game with no popups streams at the same frame rate as 1.1.1 (Artemis performance overlay about 120 FPS when the game renders at 120 or more), the log shows no secondary layer, and the helper copy timing lines are unchanged. Launcher-to-game hand-off still switches the target within about 2 s.
9. Leak tests (the stream must show none of these, while the app's own content stays visible where they overlap it):
   - open Start, Search, Alt-Tab, the notification centre and a toast on the host;
   - start Calculator or another app after the app was launched and drag its window over the app on the virtual display (it started after the launch but is not a descendant, so it must stay invisible);
   - open a context menu of another app (for example on the desktop or the taskbar) over the virtual display;
   - with the app's menu open, open Start on the host;
   - minimize and restore another app whose window lies over the virtual display (minimize animation must not appear).
10. Secure desktop: a UAC prompt is still shown and answerable, and the stream returns to the app afterwards.
11. With the client toggle off, the host streams the whole display exactly as before.
12. With `window_only_secondary_windows = disabled`, behaviour and logs match 1.1.1 (no layer lines, dialogs switch the target as before).
13. During the game test in item 8, helper CPU in Task Manager is within about 1 percentage point of 1.1.1.

## 11. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| First-frame delay of new WGC sessions makes fresh popups appear late. | Event hooks instead of polling, sessions kept for reused HWNDs, measured with a log line; contingency in 4.4. |
| Hook events lost or reordered. | 100 ms reconcile scan; visibility and PID re-checked at compose time. |
| LOCATIONCHANGE flood (cursor, carets) costs CPU in the helper. | First filter on `idObject`, no queries in the callback, coalesced `update()`. |
| A foreign process is wrongly in `P` (matcher too broad, PID reuse). | No process-tree closure; desktop UI processes excluded; HWND PID re-checked per composite; "started after launch" is not a membership rule. |
| Many sessions (apps with dozens of popups). | Caps: 16 drawn layers, 24 sessions. |
| Extra GPU copies. | Only while a secondary layer is visible; games keep the single-copy path. |
| Cursor drawn by two sessions. | Layers are drawn in z-order, so the top layer's cursor covers the one under it; no change when no popup exists. |
| Black edges on translucent popups. | Safe (black), documented; blending is a follow-up. |
| Yellow border per session on Windows builds that refuse `IsBorderRequired(false)`. | Host monitor only, never in the stream; documented. |
| Moving dialogs fights apps that position their own dialogs. | Only once per HWND, only when less than half is on the display, never popups, never resize. |
| Same-process rule keeps a launcher as target while its game window of the same process opens smaller. | Only applies when the new window is smaller than the current target; the game window is still drawn as a secondary layer, and a target switch follows when the launcher window hides. |

## 12. Out of scope

Alpha blending of layered popups, in-place retarget without restarting the helper, popups of system processes, scaling small windows, per-process audio.

## 13. Sources

- Qt `qwindowswindow.cpp` (`WindowCreationData::fromWindow()`, `initialize()`, `updateTransientParent()`, `show_sys()`): https://code.qt.io/cgit/qt/qtbase.git/tree/src/plugins/platforms/windows/qwindowswindow.cpp (popups: `WS_POPUP`, `WS_EX_TOOLWINDOW`, `HWND_TOPMOST`, no owner per QTBUG-34503, `SW_SHOWNOACTIVATE`; dialogs owned by the transient parent).
- Qt window class names and `CS_SAVEBITS`/`CS_DROPSHADOW`: https://contribute.qt-project.org/doc/d5/d32/qwindowswindowclassdescription_8cpp_source.html and https://contribute.qt-project.org/doc/d9/d3c/qwindowswindowclassregistry_8cpp_source.html
- Microsoft, `IGraphicsCaptureItemInterop::CreateForWindow`: https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow
- Microsoft, `GraphicsCaptureSession` (`IsBorderRequired`, `IsCursorCaptureEnabled`, `MinUpdateInterval`): https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession
- Microsoft, `SetWinEventHook` and event constants: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook and https://learn.microsoft.com/en-us/windows/win32/winauto/event-constants
- Microsoft, `DWMWA_EXTENDED_FRAME_BOUNDS` and `DWMWA_CLOAKED`: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
- WebRTC GDI window capturer, owned popup collection and `SysShadow` skip: https://github.com/webrtc-sdk/webrtc/blob/m125_release/modules/desktop_capture/win/window_capturer_win_gdi.cc
- WebRTC cropping window capturer (screen capture cropped to a window only when no other window covers it): https://github.com/webrtc-sdk/webrtc/blob/m125_release/modules/desktop_capture/cropping_window_capturer_win.cc
- WebRTC WGC first-frame timeout (5 s for slow starts): https://webrtc.googlesource.com/src/+/0a53c16218d0cac9784abb2f0b82a8a19c8e0e64%5E%21
- OBS forum, menus not captured by window capture, display capture needed: https://obsproject.com/forum/threads/drop-down-and-context-menus-not-recorded.52784/ and https://obsproject.com/forum/threads/a-captured-menu-dont-visible.53533/
- Local code read: `src/platform/windows/window_target.{h,cpp}`, `window_capture_policy.h`, `foreground_app.cpp`, `display_wgc.cpp`, `display.h`, `ipc/pipes.h`, `ipc/ipc_session.{h,cpp}`, `src/video.cpp`, `src/process.cpp`, `tools/sunshine_wgc_capture.cpp`, `tools/CMakeLists.txt`, `tests/CMakeLists.txt`, `tests/unit/test_window_capture_policy.cpp`, `.github/workflows/window-only-release.yml`, `.github/workflows/ci-windows.yml`.
