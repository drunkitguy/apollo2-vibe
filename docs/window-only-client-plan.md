# Window-only streaming client for Vibepollo: implementation plan

Status: the host side (sections 5 and 7.2) is implemented on branch `claude/clever-clarke-k7hhhj` of this repository and revised after review; the client (section 6 and 7.1) lives in `drunkitguy/vibe-window`. Where the implementation differs from the original plan, this document describes the implementation.

## 1. Goal

Stream from a Windows 11 PC running this Vibepollo fork to an AYN Thor (Android, 1920x1080 at 120 Hz primary screen) so that the picture only ever contains the window of the app launched from the client. The Windows desktop, taskbar, wallpaper, notifications and other windows must never appear. The client app defaults to 1920x1080 at 120 FPS, installs next to the stock Artemis app, and is published as a public APK in a GitHub Release of the separate repository `drunkitguy/vibe-window`; the patched host MSI is released from this repository. No personal information may appear in code, commits, the APK or the release.

## 2. The key constraint and the decision

A Moonlight or Artemis client cannot make the host capture a single window. It only receives the encoded video that the host decides to produce, and today Vibepollo always encodes a whole display. The work therefore has to happen on the host, with only a small client change to ask for it.

### 2.1 Options considered

**(a) Host-side window capture with Windows Graphics Capture (WGC).** `IGraphicsCaptureItemInterop::CreateForWindow(HWND)` builds a capture item for one top-level window. DWM hands over that window's own surface, so nothing drawn on top of it (other windows, the Start menu, toasts) can leak into the frame. This is how OBS and WebRTC window capture work (see sources). Vibepollo already runs WGC in a helper process (`tools/sunshine_wgc_capture.cpp`) and only calls `CreateForMonitor` today, so the plumbing (IPC, shared texture, HDR formats, pacing, DXGI fallback on the secure desktop) already exists.

**(b) Virtual display plus "tidy the desktop".** Vibepollo can already create a virtual display that matches the client (SudoVDA), and can make it the only or primary display. We could add a black wallpaper, hide the taskbar and maximize the game. This is cheap and has zero capture overhead, but the desktop still shows during launch, loading, alt-tab, when the game crashes, and whenever a notification, UAC-less dialog, overlay or other window appears. It does not meet "only the window".

**(c) Client-side crop.** The host sends the window rectangle over a custom control packet and the client crops the decoded video. The desktop around and on top of the window is still encoded and transmitted (it leaks in the bitstream and during every resize race), the encoder wastes resolution on pixels that are thrown away, overlapping windows still show inside the crop, and both the Android renderer (MediaCodec surface scaling) and client input mapping would need rewriting. Rejected.

### 2.2 Decision

Implement **(a) on top of (b)'s virtual display**, using a "masked true-position" composite:

1. The WGC helper captures the chosen window with `CreateForWindow`.
2. The helper copies only the window's client area into the existing full-display-sized shared texture, **at the same position the client area occupies on the capture display**, and fills every other pixel with black.
3. The shared texture keeps exactly the display's size and format, so the main process, encoder, image pools, HDR handling, and above all **input coordinate mapping stay unchanged**. Absolute mouse and touch coordinates from the client already map to display coordinates through `video::make_port()` (`src/video.cpp:5018`) and `input.cpp`; because the window's pixels sit where the window really is, a tap on the stream lands on the same spot in the window with no new math.
4. To make the window fill the stream, the host moves the target window onto the capture display and, if it is resizable, sizes it so its client area fills the whole monitor rectangle (frame and title bar lie outside it, so no taskbar strip is left). With the client's default of a Vibepollo virtual display at 1920x1080 and 120 Hz, a borderless game covers the display exactly, and the composite degrades to a single full-size copy per frame.
5. When no suitable window exists yet (app still starting) or the window is minimized or gone, the stream shows black, never the desktop.

Why this over pure (a) with scaling: scaling or centering the window would require remapping absolute mouse and touch coordinates and the cursor, which touches `input.cpp`, `video::make_port()` and the touch-port mailbox. The true-position approach needs none of that. Scaling a small fixed-size window to fill the screen is listed as a follow-up.

Why the virtual display matters: WGC delivers frames at most at the rate DWM composes the display the window is on. On a 60 Hz physical monitor the stream is capped at 60 FPS no matter what the client asks. The Vibepollo virtual display created at the client's 1920x1080 at 120 Hz removes that cap and gives the window a display exactly the stream's size. The client therefore enables "Use Virtual Display" by default.

### 2.3 Trade-offs of the chosen design

| Topic | Effect |
| --- | --- |
| Latency | Same capture path as today's WGC display capture (one GPU copy into helper scratch, one into the shared texture). Window mode adds at most one `ClearRenderTargetView` when the window does not cover the display. No extra encode latency. |
| Frame pacing | WGC window capture only emits frames when the window changes, exactly like WGC display capture does today, so the existing timeout and cached-frame handling in `display_wgc_ipc_vram_t::snapshot()` applies unchanged. Rate is bounded by the virtual display refresh (120 Hz). |
| HDR | Unchanged. The helper already chooses `R16G16B16A16Float` or `B8G8R8A8` via `wgc_policy::select_capture_surface_format()`; window frames use the same pool format. |
| Cursor | `GraphicsCaptureSession.IsCursorCaptureEnabled` defaults to true and Vibepollo never turns it off, so WGC draws the cursor into window frames when it is over the window, at the right position. No change. |
| Input | Unchanged because pixels stay at true desktop positions. Keyboard, relative mouse and gamepad go to the foreground window, so the host focuses the target window when it selects it. |
| Games that switch windows | The host re-evaluates the target every 250 ms and, after the choice has been stable for 750 ms, restarts WGC capture on the new window through the existing capture reinit path. This costs roughly 0.5 to 1.5 s of frozen picture per switch (launcher to game, game to crash dialog). An in-place retarget is a follow-up. |
| Launchers and popups | Only the chosen top-level window is captured. Context menus, combo-box dropdowns and other separate top-level popups of that app are not visible. Games are rarely affected; desktop apps and launchers can be. The client has a toggle to turn window-only off. |
| Exclusive fullscreen | Legacy exclusive-fullscreen games may produce no WGC window frames. Borderless fullscreen works with WGC window capture. Documented limitation. |
| UAC and lock screen | The existing secure-desktop logic swaps to DXGI duplication, which shows the secure desktop (dimmed desktop plus prompt). That is intentional so the user can answer the prompt. Capture returns to window mode afterwards via the normal reinit. |
| Audio | Still the whole system mix. Per-process loopback is a follow-up. |

## 3. Sources

- Microsoft, `IGraphicsCaptureItemInterop::CreateForWindow` (Windows 10 1903+): https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow
- Microsoft, screen capture overview, including the guidance that frame surfaces are always the frame pool size and that apps should copy the `ContentSize` sub-rectangle: https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture
- Microsoft, `Direct3D11CaptureFramePool.Recreate`: https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.recreate
- Microsoft, `GraphicsCaptureSession` (IsCursorCaptureEnabled, MinUpdateInterval): https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession
- Microsoft, `GraphicsCaptureSession.IsBorderRequired` (10.0.20348+): https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.graphicscapturesession.isborderrequired
- Microsoft, `DWMWA_EXTENDED_FRAME_BOUNDS`: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
- Win32 WGC sample (window and monitor items, resize handling): https://github.com/robmikh/Win32CaptureSample
- WebRTC WGC window source, production use of `CreateForWindow`: https://webrtc.googlesource.com/src/+/main/modules/desktop_capture/win/wgc_capture_source.cc
- OBS WinRT capture (window capture via WGC, border and cursor toggles): https://git.tjdev.de/mirror/obs-studio/src/commit/ee144377dc50b5d9f1fdf0598cea56f7e34eab9b/libobs-winrt/winrt-capture.cpp
- Vibepollo upstream release notes (WGC improvements, virtual display behaviour; no window capture target exists upstream): https://newreleases.io/project/github/Nonary/Vibepollo/release/v1.18.3 and https://newreleases.io/project/github/Nonary/Vibepollo/release/v1.18.0
- Client base: upstream Artemis https://github.com/ClassicOldSong/moonlight-android at `c5cf27f4dc822db0e863c4691e7a70c74bea977a` (GPL-3.0), with submodule https://github.com/ClassicOldSong/moonlight-common-c at `c999436858471dfefa7617af3b7dc03ec1644ce4` (nested `enet` submodule).
- GitHub runner image contents (Android SDK, NDK 27.3/28.2/29.0 installed, JDK 17 default): https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md
- Release action: https://github.com/softprops/action-gh-release ; SDK setup: https://github.com/android-actions/setup-android
- Local code read for this plan: `src/nvhttp.cpp`, `src/rtsp.h`, `src/rtsp.cpp`, `src/video.h`, `src/video.cpp`, `src/input.cpp`, `src/process.cpp`, `src/platform/windows/display_base.cpp`, `display_wgc.cpp`, `foreground_app.cpp`, `ipc/pipes.h`, `ipc/ipc_session.cpp`, `tools/sunshine_wgc_capture.cpp`, `tools/playnite_launcher/focus_utils.cpp`, and in upstream Artemis `NvHTTP.java`, `StreamConfiguration.java`, `PreferenceConfiguration.java`, `StreamSettings.java`, `Game.java`, `AppView.java`, `ServerHelper.java`, `preferences.xml`, `strings.xml`, `app/build.gradle`, and moonlight-common-c `src/ControlStream.c`.

## 4. Protocol: the `windowOnly` launch flag

Moonlight-family clients start and resume streams with HTTPS GET `/launch` or `/resume` and a query string. Artemis builds it in `NvHTTP.launchApp()` (`app/src/main/java/com/limelight/nvstream/http/NvHTTP.java`, line 849; the query is built on lines 880 to 890), and already sends Apollo extensions such as `virtualDisplay=` and `scaleFactor=`. The host reads these in `make_launch_session_from_snapshot()` (`src/nvhttp.cpp:1826`), which both `launch()` (line 3144) and `resume()` (line 3663) call.

New parameter:

- `windowOnly=1` requests window-only capture for this session. `windowOnly=0` or absence means today's behaviour.
- Unknown query parameters are ignored by stock Sunshine, Apollo and upstream Vibepollo, so the new client stays compatible with hosts that do not have this patch (they simply stream the display).
- No new control packet and no RTSP change are needed. The flag lives on `launch_session_t` and is copied into `video::config_t` when the RTSP ANNOUNCE builds the stream config.

## 5. Host changes (apollo2-vibe)

All paths are relative to the repository root. Line numbers refer to commit `0906d00`.

### 5.1 Session flag plumbing

1. `src/rtsp.h`, `struct launch_session_t` (line 41): add `bool window_only = false;` next to `client_vrr_requested` (line 96), with a one-line comment.
2. `src/rtsp.cpp`, `launch_session_t::clone_for_startup()` (line 183): add `snapshot->window_only = window_only;`. This is required by the contract comment in `rtsp.h` and the RtspStartupSnapshot test.
3. `src/nvhttp.cpp`, `make_launch_session_from_snapshot()`: right after the `clientVrrRequested` parse (line 2113) add
   `launch_session->window_only = util::from_view(get_arg(args, "windowOnly", "0")) != 0;`
   and log once at info level when true: "Client requested window-only capture".
4. `src/video.h`, `struct video::config_t` (line 27): append `bool window_only = false;` as the **last** field (the struct comment forbids inserting in the middle).
5. `src/rtsp.cpp`, `cmd_announce()` (line 1550): next to `config.monitor.input_only = session->input_only;` (line 1696) add `config.monitor.window_only = session->window_only;`.
6. `src/webrtc_stream.cpp`: no change (WebRTC keeps `window_only = false`).
7. `tests/unit/test_rtsp_startup_snapshot.cpp`: set `ls.window_only = true;` in `make_populated_launch_session()` and add `EXPECT_EQ(clone->window_only, source.window_only);` in `CopiesAllConsumedFields`.

### 5.2 Force the WGC backend for window-only sessions

`src/platform/windows/display_base.cpp`, `platf::display()` (line 1704): compute
`const bool prefer_wgc_backend = config.window_only || (!user_requested_ddx && (wgc_requested || default_to_wgc));`
If WGC creation fails in a window-only session, log an error and return `nullptr` without falling through to DDX, because DDX would show the desktop. The stream then fails to start and the user can turn the client toggle off. The secure-desktop fallback inside `display_wgc_ipc_vram_t::create()` stays as is (UAC and lock screen only, see 2.3).

### 5.3 Pure policy header (unit-testable on Linux)

New file `src/platform/windows/window_capture_policy.h`, namespace `platf::dxgi::window_policy`, header-only, no Windows headers (same style as `wgc_capture_policy.h`). Contents:

- `struct rect_i { std::int32_t left, top, right, bottom; };`
- `struct window_blit_t { bool empty; bool covers_output; std::uint32_t src_left, src_top, src_right, src_bottom; std::uint32_t dst_x, dst_y; };`
- `constexpr window_blit_t compute_window_blit(rect_i output, rect_i frame_bounds, rect_i client_screen, std::int32_t content_w, std::int32_t content_h);`
  - `output` is the capture display rectangle in desktop coordinates (physical pixels).
  - `frame_bounds` is `DWMWA_EXTENDED_FRAME_BOUNDS` of the window (the WGC surface origin; not `GetWindowRect`, which includes the invisible resize border).
  - `client_screen` is the client rectangle in screen coordinates (`GetClientRect` plus `ClientToScreen`).
  - Source rectangle = `client_screen` shifted by `-frame_bounds.left/top`, clamped to `[0, content_w) x [0, content_h)`. Destination = the matching part of `client_screen` shifted by `-output.left/top`, then both clipped against `[0, output width) x [0, output height)` with the source adjusted by the same amount. `empty` when the result has no area. `covers_output` when the destination is `(0,0)` and the size equals the output size.
- `struct window_candidate_t { std::uintptr_t id; bool matches_app; bool started_after_launch; bool is_foreground; bool iconic; std::int64_t client_area; };`
- `constexpr std::uintptr_t choose_target(std::span<const window_candidate_t>, std::uintptr_t current, bool have_app_matcher);` implementing section 5.5's rules.
- `struct target_debouncer_t { std::uintptr_t pending; int stable_polls; std::uintptr_t committed; bool observe(std::uintptr_t candidate, int polls_required); };` returns true when `committed` changes.

New test `tests/unit/test_window_capture_policy.cpp`, registered in `tests/CMakeLists.txt` next to `test_component_encoder_probe_policy` as `sunshine_register_component(NAME test_component_window_capture_policy TEST_SOURCE unit/test_window_capture_policy.cpp)` so it also runs on non-Windows CI. Cases: borderless window exactly covering the output; bordered window with title bar (source offset by frame, destination below the title bar); window partly off the left edge of the output; window entirely on another display (empty); zero-size client (minimized, empty); content smaller than client during a resize (clamped); output not at desktop origin (virtual display to the right of a physical monitor); `choose_target` foreground-candidate wins, stickiness to current, largest-area fallback, foreground-follow when no matcher, nothing when only shell windows; debouncer needs 3 equal polls and ignores flapping.

### 5.4 WGC helper: capture a window and composite at true position

File `tools/sunshine_wgc_capture.cpp` plus `src/platform/windows/ipc/pipes.h` and `tools/CMakeLists.txt`.

1. `src/platform/windows/ipc/pipes.h`:
   - Add `WGC_IPC_FLAG_WINDOW_CAPTURE = 1u << 3` to `wgc_ipc_config_flags_e` (line 76).
   - Append `std::uint64_t target_hwnd;` as the last member of `config_data_t` (line 84). Messages are told apart by size in `handle_ipc_message()`; the config message stays far larger than the 8-byte `activity_admission_data_t`, so no collision. Both binaries are always built and shipped together.
2. Helper globals (line 136): extend the `g_config` initializer with a trailing `0` for `target_hwnd`. Log `window_capture` and the HWND in the "Received config data" line in `handle_ipc_message()` (line 2135).
3. `DisplayManager` (around line 640):
   - Keep `select_monitor()`, `get_monitor_info()`, `create_graphics_capture_item()` and `configure_capture_resolution()` unchanged. The monitor item is still created because its `Size()` defines the shared texture size, which must equal the main process's `width_before_rotation`/`height_before_rotation` or `snapshot()` forces a reinit (`display_wgc.cpp`, "Capture size changed").
   - Add `bool create_window_capture_item(HWND hwnd, GraphicsCaptureItem &item)` using `activation_factory->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item))` after checking `IsWindow(hwnd)`.
   - Add `RECT monitor_rect() const` returning `_monitor_info.rcMonitor` (physical pixels; the helper is per-monitor DPI aware v2 at line 255).
4. `main()` (around line 2345): after the monitor item and `configure_capture_resolution(item)`:
   - If `(g_config.flags & WGC_IPC_FLAG_WINDOW_CAPTURE) != 0`: set `window_mode = true`. If `target_hwnd != 0` and `create_window_capture_item()` succeeds, use that window item as `deps.graphics_item` and register its `Closed` handler (same `g_capture_item_closed` flag, so a closed window makes the helper exit and the main process reinitializes and picks a new target). If `target_hwnd == 0` or creation fails, run in "black" mode: do not create a capture session; publish one black frame to the shared texture after the handle hand-off, then idle in the existing message loop.
   - Pass `window_mode`, the target HWND, and `monitor_rect()` into `WgcCaptureManager`.
5. `WgcCaptureManager` (class around line 1030; constructor line 1133):
   - New members `_window_mode`, `_target_hwnd`, `_monitor_rect`, and `_pool_width`/`_pool_height`. In display mode the pool size equals `_width`/`_height` exactly as now. In window mode the initial pool size is the window item's `Size()`.
   - `create_or_adjust_frame_pool()` (line 1263): use `_pool_width`/`_pool_height` for both `Recreate` and `CreateFreeThreaded` (lines 1275 and 1291).
   - `process_frame()` (line 1308): before moving `frame` into `queue_frame_for_delivery()`, read `frame.ContentSize()`. After delivery, if window mode and the content size differs from the pool size, update `_pool_width/_pool_height` and call `create_or_adjust_frame_pool(_current_buffer_size)` (this must happen after the frame is released, as in the Win32CaptureSample resize handling).
   - `queue_frame_for_delivery()` (line 1713): in display mode keep the existing `CopyResource` (line 1744). In window mode, under the same `_d3d_context_mutex`:
     - If `IsIconic(_target_hwnd)`: clear the scratch texture to black and enqueue.
     - Otherwise query `DwmGetWindowAttribute(DWMWA_EXTENDED_FRAME_BOUNDS)`, `GetClientRect` and `ClientToScreen`, call `window_policy::compute_window_blit()`. If `empty`, clear to black. If not `covers_output`, clear to black first. Then `CopySubresourceRegion(scratch, 0, dst_x, dst_y, 0, frame_tex, 0, &src_box)`.
     - Clearing: scratch textures are already created with `D3D11_BIND_RENDER_TARGET` (`ensure_scratch_texture()`, line 1614). Create and cache one `ID3D11RenderTargetView` per scratch slot next to `scratch.texture` and call `ClearRenderTargetView` with `{0,0,0,1}`. Zero RGB is black for both BGRA8 and FP16 scRGB.
   - Add `void publish_black_frame()` for black mode: reserve a scratch slot, clear it, enqueue it with the current QPC. The existing delivery thread then copies it into the shared texture (line 1864) and signals the frame event.
   - `create_capture_session()` (line 1927): unchanged. `IsBorderRequired(false)` is already attempted, which keeps the yellow capture border off the host screen where the OS allows it.
6. `tools/CMakeLists.txt` (line 101): add `dwmapi` to `target_link_libraries(sunshine_wgc_capture ...)`. Include `src/platform/windows/window_capture_policy.h` in the helper.

### 5.5 Main process: choose, place and track the target window

New files `src/platform/windows/window_target.h` and `window_target.cpp`, added to `cmake/compile_definitions/windows.cmake` next to `ipc/ipc_session.cpp` (line 255). Namespace `platf::window_target`.

1. Reuse app matching from `foreground_app.cpp`. Factor the matcher construction inside `foreground_app::snapshot()` (lines 552 to 605: Playnite install-dir match, `proc::proc.running_app_contains_pid()` for trackable apps) into an exported function in `foreground_app.h`:
   `std::optional<std::function<bool(DWORD, std::string_view)>> active_app_window_matcher();`
   and make `snapshot()` call it, so behaviour there is unchanged. Also export `bool is_desktop_ui_window(HWND hwnd, std::string_view executable);` wrapping the existing internal checks (`GetDesktopWindow`, `GetShellWindow`, `foreground_window_is_windows_shell()` line 68, `process_is_windows_desktop_ui()` line 312).
2. `std::vector<window_policy::window_candidate_t> enumerate_candidates(...)`: `EnumWindows` over top-level windows keeping those that are visible, not cloaked (`DWMWA_CLOAKED`), root (`GetAncestor(GA_ROOT) == hwnd`), not `WS_EX_TOOLWINDOW`, not an overlay (`foreground_app::is_window_selection_overlay()`: a layered window that is click-through or non-activating, captionless and topmost, or fully see-through, or a frameless non-layered window that is click-through or non-activating; skinned or fading layered windows stay eligible), not owned (`GetWindow(GW_OWNER) == nullptr`) unless it is the foreground window, client area at least 64x64, and not desktop UI. A UWP `ApplicationFrameWindow` is attributed to the process of its hosted `Windows.UI.Core.CoreWindow` child instead of ApplicationFrameHost, so UWP apps can be captured; a frame without a hosted app is skipped. When the foreground window is dropped as shell UI, that is logged once at debug level. Mark `matches_app` with the matcher, `is_foreground` against `GetForegroundWindow()`, and `iconic` with `IsIconic`. `new_since_launch` is set for a window that was not visible when the app was launched. Process lookups cache the executable path by pid and creation time, trust a cached entry for a second and then read the creation time again; the app matcher (which may query Playnite) is refreshed once a second.
3. Selection rules (implemented in `window_policy::choose_target()`):
   - The mode comes from `window_policy::selection_mode_for()`. An app that launches something (a command, detached commands, a Playnite game, or any tracked process) uses one of the two app modes; only the command-less Desktop app follows the foreground window.
   - If an app matcher exists: eligible windows are those that match, plus those whose process was created after the session's launch time (`GetProcessTimes`, fills `started_after_launch`; this catches games a launcher starts outside the job object). Pick the foreground window if eligible; else the current target if still eligible; else the eligible window with the largest client area (ties broken by Z order, which is `EnumWindows` order); else none (black). Windows that existed before the launch and do not match are never chosen, so the stream stays black while the app is starting.
   - If the app launches something but has no matcher (apps started through URLs such as `steam://` whose launcher process exits and auto-detaches): windows of processes created after the launch and windows that appeared after the launch are eligible, with the same order (foreground, current target, largest). The second rule covers a URL that opens a window in a launcher that was already running, such as the default Steam Big Picture app (`steam://open/bigpicture`). A browser or chat window that was already open before the launch is never chosen, even in the foreground.
   - Desktop app (no command): follow the foreground window if it is a valid non-shell candidate; else keep the current target if still valid; else none (black).
   - Minimized candidates are only chosen if nothing else qualifies.
   - Launch time: add `void mark_launch()` and `FILETIME launch_time()` (atomic 64-bit storage) to `window_target`, and call `mark_launch()` in `proc::proc_t::launch_app_commands()` in `src/process.cpp` under `#ifdef _WIN32`, after the prep commands and right before the detached and main app commands spawn (so prep commands, RTSS warm-up and Lossless Scaling set-up do not count as the app). A candidate is `started_after_launch` when its process creation time is later than `launch_time()` and `launch_time()` is non-zero. `mark_launch()` also records the visible top-level windows of the interactive `Default` desktop (`OpenDesktopW` and `EnumDesktopWindows`) for every launch, so a later window-only `/resume` still has the snapshot. When the desktop cannot be enumerated (for example while the host is locked), no snapshot is stored and no window counts as new.
4. `class tracker_t`: constructed with the capture display rectangle (`captured_output_desc.DesktopCoordinates`). Owns a `std::jthread` that calls `syncThreadDesktop()` (`misc.cpp:489`) once, then every 250 ms enumerates, chooses, and feeds `target_debouncer_t` with `polls_required = 3`. Exposes `std::uintptr_t select_now()` (synchronous, used at display init; it only enumerates) and `std::uintptr_t committed_target() const` (atomic read). The tracker thread also places and focuses each newly committed target (item 5), so nothing that can block on another process runs on the capture thread. Logs each committed change at info level with pid, executable base name and window class (no titles, to avoid logging document names).
5. Placement and focus, on the tracker thread, once per newly committed target. The decision is the pure `window_policy::plan_placement()` (unit tested); each poll takes one step, up to 8 steps:
   - A window whose client area already covers `output` (borderless fullscreen) is never touched.
   - A minimized window is restored (`ShowWindowAsync(SW_RESTORE)`). A maximized resizable window that does not cover the display is restored and then resized.
   - A resizable main window (`WS_THICKFRAME` and `WS_MAXIMIZEBOX`, not owned) is sized with `SetWindowPos(..., SWP_ASYNCWINDOWPOS | SWP_NOZORDER | SWP_NOACTIVATE)` so its client area equals the full monitor rectangle (not the work area); frame and title bar lie outside the display. Any other window (fixed size, dialogs, owned windows) is moved client area first onto the display when its client area is not already inside it.
   - Hung windows (`IsHungAppWindow`) are skipped. Focus uses `SetForegroundWindow` only, falling back to `AttachThreadInput` with the foreground thread when that thread is not hung. It never restores the window and never changes its topmost state.
6. `src/platform/windows/ipc/ipc_session.h/.cpp`: add `void set_window_target(std::uintptr_t hwnd)` and `std::uintptr_t window_target() const`. In the helper start-up where `config_data` is filled (`ipc_session.cpp` around line 330), set `config_data.target_hwnd` and, when `_config.window_only`, OR `WGC_IPC_FLAG_WINDOW_CAPTURE` into `config_data.flags` (next to `wgc_ipc_flags(_config)`).
7. `src/platform/windows/display_wgc.cpp` and the class declarations in `display.h` (line 459 onward):
   - Add `std::unique_ptr<window_target::tracker_t> _window_tracker;` and `std::chrono::steady_clock::time_point _last_window_reinit;` to both `display_wgc_ipc_vram_t` and `display_wgc_ipc_ram_t`.
   - In `display_wgc_ipc_vram_t::init()` (line 214) and `display_wgc_ipc_ram_t::init()` (line 470), when `config.window_only`: create the tracker with `captured_output_desc.DesktopCoordinates`, call `select_now()` and `_ipc_session->set_window_target(hwnd)` before the first `initialize_if_needed()`.
   - In both `snapshot()` implementations (line 249 and line 512), right after the existing `should_reinit()` check: if the tracker's committed target differs from `_ipc_session->window_target()` and at least 2 s have passed since the last window-driven reinit, log "Window-only target changed; restarting capture" and `return capture_e::reinit;`. The capture loop in `video.cpp` (line 2906) then rebuilds the display, whose `init()` picks up the new target.
   - Factor the shared init and snapshot logic into two small helpers in the anonymous namespace so the VRAM and RAM variants do not duplicate it.
8. `src/video.cpp`: concurrent sessions share one capture that is created from the first session's config. A session whose `window_only` differs from the running capture is refused with a warning and shut down while another session still streams, so a window-only session never receives the desktop and a regular session never receives a black picture. When no session uses the capture any more (or every session on it is ending), the new session's mode is adopted and the capture is reinitialized.
9. `src/process.h/.cpp`: `running_app_state()`, `running_app_contains_pid()` and `foreground_window_matches_running_app()` read the running app under a new `_running_state_mutex`, which also guards the writes of `_app`, `_app_id`, `_process` and `_process_group`, because the tracker polls them from its own thread. App commands are spawned into a local child and job group that are swapped in under the lock, and `placebo` is atomic.
10. `src/update.cpp`: a new CMake option `SUNSHINE_DISABLE_UPDATE_CHECK` (default OFF) turns the upstream update check off. The window-only release sets it, because upstream releases are not updates for this build. With it set, `/api/metadata` also returns `update_check_disabled` and `releases_url` (CMake cache string `SUNSHINE_RELEASES_URL`), and the web UI's "Check for updates" button links there instead of the upstream releases.

### 5.6 Things deliberately not changed

`src/input.cpp`, `video::make_port()`, the touch-port mailbox, `stream.cpp`, audio, the control stream, and the 0x3003 text-field packet all stay as they are. No config file keys or web UI settings are added in this first version; the client flag is the only switch.

## 6. Client changes (Android), repository `drunkitguy/vibe-window`

The client lives in its own public repository `drunkitguy/vibe-window`. Work is pushed to branch `claude/clever-clarke-k7hhhj` there. Nothing from any other client fork is used.

### 6.1 Repository layout

Commit 1, "Import upstream Artemis c5cf27f":

- `git -C <clone of ClassicOldSong/moonlight-android> archive c5cf27f4dc822db0e863c4691e7a70c74bea977a | tar -x -C <clone of drunkitguy/vibe-window>`, after moving the repository's initial `README.md` aside. This copies the tracked files without upstream history and includes `.gitmodules`. Rename upstream's `README.md` to `README.upstream.md`.
- Register the submodule as a real gitlink: `git update-index --add --cacheinfo 160000,c999436858471dfefa7617af3b7dc03ec1644ce4,app/src/main/jni/moonlight-core/moonlight-common-c`. Keep upstream `.gitmodules` (URL https://github.com/ClassicOldSong/moonlight-common-c). CI checks out with `submodules: recursive`, which also pulls the nested `enet`.
- The prebuilt `libopus`/`openssl` `.a` files (12) are tracked upstream and not ignored by upstream `.gitignore`; confirm with `git ls-files | grep -c '\.a$'` = 12. Keep `gradlew` mode 100755.
- No moonlight-common-c changes are needed (the flag is a Java query parameter), so nothing is vendored.

Commit 2 and later, "Vibe Window" changes (6.2), plus:

- `README.md`: what Vibe Window is, that it requires the patched host from https://github.com/drunkitguy/apollo2-vibe/releases (install the `window-host-v*` MSI on the PC first), defaults, the toggle, limitations, build and signing notes.
- `NOTICE`: based on Artemis (ClassicOldSong/moonlight-android) and Moonlight (moonlight-stream), GPL-3.0; moonlight-common-c GPL-3.0; enet MIT; bundled OpenSSL and Opus under their licences; list of local modifications. Keep `LICENSE.txt` and all headers.

### 6.2 Identity, name and defaults (upstream line numbers at c5cf27f)

1. `app/build.gradle`
   - Flavor `root` (line 28, `applicationId` line 43): `app.vibewindow.client.root`. Flavor `nonRoot_game` (line 48, `applicationId` line 59): `app.vibewindow.client`. Replace the `nonRoot_game` `obtainium_app_url` (lines 55 to 57) with the neutral `data:` URL the root flavor uses (line 40).
   - `release` build type (lines 137 to 140): remove `applicationIdSuffix ".noir"`; labels "Vibe Window", "Vibe Window (Root)", "Vibe Window (Game)"; add `signingConfig signingConfigs.ci`. `debug` (lines 98 to 101): suffix `.debug`, labels "Vibe Window Debug" variants.
   - `defaultConfig` (lines 14 and 15): `versionName (project.findProperty('windowClientVersionName') ?: "1.0.0")` and `versionCode Integer.parseInt((project.findProperty('windowClientVersionCode') ?: "1").toString())`. Never Groovy `as int` on a String.
   - `defaultConfig`: `ndk { abiFilters 'arm64-v8a' }`; `splits.abi` (line 155): `enable false`.
   - `signingConfigs { ci { storeFile file("${rootDir}/signing/vibe-window-ci.p12"); storePassword "vibewindow-public"; keyAlias "vibewindow"; keyPassword "vibewindow-public"; storeType "pkcs12" } }`.
   - `lint {}` (line 71): add `checkReleaseBuilds false` and `abortOnError false`.
   - Keep `ndkVersion "27.0.12077973"` and `compileSdk 36`.
2. `signing/vibe-window-ci.p12` (upstream ignores `key/`, not `signing/`): `keytool -genkeypair -keystore vibe-window-ci.p12 -storetype PKCS12 -alias vibewindow -keyalg RSA -keysize 3072 -validity 10000 -storepass vibewindow-public -keypass vibewindow-public -dname "CN=Vibe Window CI"`. Public on purpose so every CI build has the same signature and updates install over older versions without secrets. Check with `git check-ignore -v`.
3. Defaults:
   - `app/src/main/java/com/limelight/preferences/PreferenceConfiguration.java`: `DEFAULT_RESOLUTION = "1920x1080"` (line 142), `DEFAULT_FPS = "120"` (line 143), `DEFAULT_USE_VIRTUAL_DISPLAY = true` (line 146).
   - `app/src/main/res/xml/preferences.xml`: `android:defaultValue` `"1920x1080"` for `list_resolution` (line 13), `"120"` for `list_fps` (line 21), `"true"` for `checkbox_use_virtual_display` (line 81).
   - The virtual display default takes effect: a normal app tap calls `ServerHelper.doStart(..., prefConfig.useVirtualDisplay)` (`AppView.java` lines 755 and 768), which sets `Game.EXTRA_VDISPLAY` (`ServerHelper.java` line 115), read in `Game.java` line 573 and passed with `.setVirtualDisplay(vDisplay)` (line 786). Resume from `PcView` passes `false` (line 780), which is fine because the display already exists.
   - `StreamSettings.java` line 608 removes the 120 option only on screens under 118 Hz; the Thor reports 120 Hz.
4. Window-only flag:
   - `PreferenceConfiguration.java`: `WINDOW_ONLY_PREF_STRING = "checkbox_window_only"` next to `USE_VIRTUAL_DISPLAY_PREF_STRING` (line 49), `DEFAULT_WINDOW_ONLY = true`, public field `windowOnly` on line 249, read next to line 884.
   - `preferences.xml`: `CheckBoxPreference` key `checkbox_window_only`, default `true`, right after the `checkbox_use_virtual_display` entry.
   - `app/src/main/res/values/strings.xml` after line 568: `title_checkbox_window_only` "Show only the app window", `summary_checkbox_window_only` "Stream only the launched app's window and hide the Windows desktop. Requires the Vibepollo window-only host build."
   - `StreamConfiguration.java`: field next to `virtualDisplay` (line 17), `Builder.setWindowOnly(boolean)` next to line 64, default `false` next to line 161, `getWindowOnly()` next to line 198.
   - `Game.java`: `.setWindowOnly(prefConfig.windowOnly)` after `.setVirtualDisplay(vDisplay)` (line 786).
   - `NvHTTP.java`: `"&windowOnly=" + (context.streamConfig.getWindowOnly() ? 1 : 0) +` after the `virtualDisplay` line (887). Covers launch and resume.
5. `strings.xml` line 145 `email_recipient`: set to an empty string so crash logs from this fork are not mailed to Artemis maintainers.
6. No renderer, input, JNI or moonlight-common-c changes.

### 6.3 Compatibility of upstream Artemis with this host

- The host's 0x3003 "Set Text Field Focus" packet (`src/stream.cpp` line 1801) is sent only when `text_field_detection` is enabled and the watcher runs. Upstream moonlight-common-c at `c999436` handles unknown control types by falling through to `free(ctlHdr)` in `controlReceiveThreadFunc` (`ControlStream.c` around lines 1255 to 1375), so the packet is ignored harmlessly. No gating needed.
- Upstream already sends `virtualDisplay=`, `scaleFactor=` and `mode=WxHxFPS`, which this host reads. Hosts without this patch ignore `windowOnly=`.

## 7. CI workflows

### 7.1 Client release, in `drunkitguy/vibe-window`

New file `.github/workflows/release.yml` (upstream has no workflows):

- Trigger: `push` to branches `claude/clever-clarke-k7hhhj` and `main`. Top-level `permissions: {}`, `concurrency` per ref with cancel-in-progress.
- Version: root file `VERSION` (for example `1.0.0`); tag `v<VERSION>`.
- Job `build` (`ubuntu-latest`, `contents: read`, 60 min): `actions/checkout@v4` with `submodules: recursive`; `actions/setup-java@v4` temurin 17 with `cache: gradle`; `android-actions/setup-android@v3`; `sdkmanager --install "ndk;27.0.12077973"`; `VERSION_CODE=$((1000 + GITHUB_RUN_NUMBER))`; `./gradlew :app:assembleNonRoot_gameRelease --no-daemon --stacktrace -PwindowClientVersionName=... -PwindowClientVersionCode=...`; copy to `dist/VibeWindow-<VERSION>-arm64-v8a.apk`; `apksigner verify --print-certs` using the newest `$ANDROID_HOME/build-tools/*`; write `.sha256`; upload artifact `vibe-window-apk` (`if-no-files-found: error`).
- Job `release` (`needs: build`, `contents: write`): skip if `gh release view v<VERSION>` succeeds; download the artifact; `softprops/action-gh-release@v2` with `tag_name: v<VERSION>`, `target_commitish: ${{ github.sha }}`, `name: Vibe Window <VERSION>`, `files: dist/*`, `fail_on_unmatched_files: true`, `make_latest: true`, and a fixed body: requires the patched host (link https://github.com/drunkitguy/apollo2-vibe/releases), sideload steps, defaults, toggle, limitations, GPL-3.0 source in this repo. No names, emails or local paths.

### 7.2 Host installer, in `drunkitguy/apollo2-vibe`

`.github/workflows/window-only-release.yml`, triggered by `push` to `claude/clever-clarke-k7hhhj` and `vibepollo-base` with `paths: [src/**, tools/**, tests/**, cmake/**, packaging/**, third-party/**, CMakeLists.txt, .github/actions/**, .github/workflows/ci-windows.yml, .github/window-only-host-version.txt, .github/workflows/window-only-release.yml]`. Version from `.github/window-only-host-version.txt`; tag `window-host-v<VERSION>` (does not match `ci.yml`'s release gate, and tags created by `GITHUB_TOKEN` trigger no workflows). No android job.

- Job `check` (`ubuntu-latest`, `contents: read`): reads the version, and sets `publish=false` when release `window-host-v<VERSION>` already has the asset `Vibepollo-WindowOnly-<VERSION>.msi`, which skips the Windows build. Bump the version file for a new release.
- Job `windows` (`needs: check`, only when publishing; job concurrency group `window-only-build-<ref>` with `cancel-in-progress: true`): `uses: ./.github/workflows/ci-windows.yml` with `build_only: false`, `build_tests: true`, `run_window_only_tests: true` (runs `test_component_window_capture_policy.exe` on the Windows runner), `disable_update_check: true`, `run_startup_smoke: true` (right after the build: a self-test program that throws from a static initializer must be reported by the harness, then the unstripped `sunshine.exe` must stay up for 25 s under gdb, which prints a backtrace for every throw and for abort; any failure fails the job, so a build that crashes at startup is never packaged or released), `release_commit: ${{ github.sha }}`, `release_version: <VERSION>`, `release_artifact_retention_days: 7`, `symbol_product_name: Vibepollo`, `symbol_release_prefix: polo`, `publish_symbols: false`, `require_truehdr_runtime: false`, `permissions: actions: read, contents: read`, no secrets. It builds the host, the helper and the tests and uploads the artifact `unsigned-msi-package-Windows`.
- Job `release` (`needs: [check, windows]`, `contents: write`, job concurrency group `window-only-publish-<ref>` with `cancel-in-progress: false`, so publishing is never cancelled midway): checks again that the asset is missing; downloads `unsigned-msi-package-Windows`; renames the MSI to `Vibepollo-WindowOnly-<VERSION>.msi` plus `.sha256`; `softprops/action-gh-release` pinned to `3bb12739c298aeb8a4eeaf626c5b8d85266b0e65` (v2.6.2), which also uploads to a release that exists without the asset; `make_latest: true`, body: unsigned MSI (SmartScreen "More info, Run anyway"), pair with the Vibe Window APK from https://github.com/drunkitguy/vibe-window/releases, limitations. No personal data.

## 8. Acceptance criteria

Build and release:

1. `vibe-window` `release.yml` and `apollo2-vibe` `window-only-release.yml` succeed on branch `claude/clever-clarke-k7hhhj`.
2. Public release `v1.0.0` in `drunkitguy/vibe-window` with the APK and `.sha256` (`apksigner verify` passed), and public release `window-host-v1.0.0` in `drunkitguy/apollo2-vibe` with the MSI and `.sha256`.
3. The `windows` job compiles `sunshine`, `sunshine_wgc_capture` and the tests.
4. `test_component_window_capture_policy` and the updated `RtspStartupSnapshot` test pass.

Client behaviour (verifiable from code and on device):

5. The APK installs beside stock Artemis (package `app.vibewindow.client`, launcher label "Vibe Window").
6. On a fresh install the settings show 1080p, 120 FPS, Use Virtual Display on, Show only the app window on.
7. The host log shows "Client requested window-only capture" on launch and on resume; with the toggle off the flag is `0` and the host behaves exactly as before.

Host behaviour (manual test on the user's PC, since CI cannot run a GPU session):

8. Launching a normal windowed app (for example Notepad added as an app) streams only that window at its true position, maximized onto the virtual display; everything else is black. Opening the Start menu, a toast, or dragging another window over it on the host does not show in the stream.
9. A borderless-fullscreen game fills the 1920x1080 stream and Artemis's performance overlay shows about 120 FPS when the game renders at 120 or more.
10. Before the app's window exists the stream is black, not the desktop. After closing the app the stream goes black.
11. Launcher-to-game hand-off switches the stream to the game window within about 2 s.
12. A touch or absolute mouse click on a visible control activates that control.
13. A UAC prompt is visible and answerable, and the stream returns to the window afterwards.
14. No personal information: `git diff` of the change contains no email addresses or real names outside pre-existing upstream attributions; the keystore DN is "CN=Vibe Window CI"; the release body names no person. Commits use the session's configured neutral identity.

## 9. Risks and mitigations

| Risk | Mitigation |
| --- | --- |
| No local Windows or Android toolchain; host code is only compiled in CI and never run before the user tries it. | Keep all geometry and selection logic in a pure header with Linux-runnable tests. Reuse existing helper and reinit paths instead of new threading in the helper. Keep changes additive and gated on `window_only`, so non-window sessions are unaffected. |
| Target window on another display (virtual display not isolated). | The tracker moves it onto the capture display. If a game fights the move, the stream shows its part that lies on the display, black elsewhere. |
| Wrong window chosen (launcher kept alive, overlay processes, games that spawn outside the job object). | Foreground-follow fallback, stickiness, debounce; logs name exe and class for each change so the rules can be tuned. The client toggle turns the feature off. |
| Capture restart on every target change causes a 0.5 to 1.5 s freeze. | Debounce (750 ms) and 2 s cooldown. Follow-up: in-place retarget message to the helper. |
| Taps on the black margin around a fixed-size window reach what lies under it on the host. | Resizable windows are sized to fill the display, so only fixed-size windows leave a margin. Absolute input is not clamped: mouse, touch and pen use separate scaling paths, and relative mouse and keyboard input can reach other windows anyway, so a partial clamp would not make the margin safe. Documented in the release notes. |
| Menus and popups not visible for desktop apps. | Documented; games are the target use. Follow-up: composite owned popups as extra WGC items. |
| Exclusive-fullscreen games deliver no window frames. | Documented; ask the user to use borderless. Follow-up: fall back to display capture when `fullscreen_detector` reports exclusive D3D fullscreen. |
| Yellow capture border drawn on the host monitor on some Windows builds. | `IsBorderRequired(false)` is already attempted. The border is never part of the captured frame, so the stream is unaffected. |
| Publicly known signing key allows a third party to build an APK that installs over this one. | Only matters for APKs obtained elsewhere; documented in the vibe-window README. A private key via repository secrets is a follow-up. |
| Jitpack or Maven outages break the Android build. | Gradle cache in `setup-java`; rerun. |
| Release-type build trips lint or R8 issues never seen in upstream's CI-less release builds. | Lint made non-blocking; upstream's own `proguard-rules.pro` is used. If R8 still fails, fall back to `assembleNonRoot_gameDebug` with the same signing config and debug label changes, and note the switch in the release body. |
| `config_data_t` size change between main and helper. | Both are built together and installed together; the helper already rejects messages of unexpected size. |

## 10. Follow-ups (out of scope for the first version)

- In-place retarget: a runtime IPC message carrying a new HWND so the helper swaps the capture item and session without restarting the encoder.
- Optional scale-to-fit for small fixed-size windows, with matching input remapping through `touch_port_t`.
- Per-app `window-only` setting in `apps.json` and the web UI, and a global host config key.
- Per-process audio via WASAPI process loopback.
- Composite owned popup windows of the target.
