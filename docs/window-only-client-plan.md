# Window-only streaming client for Vibepollo: implementation plan

Status: plan only. Nothing in this document has been implemented yet.

## 1. Goal

Stream from a Windows 11 PC running this Vibepollo fork to an AYN Thor (Android, 1920x1080 at 120 Hz primary screen) so that the picture only ever contains the window of the app launched from the client. The Windows desktop, taskbar, wallpaper, notifications and other windows must never appear. The client app defaults to 1920x1080 at 120 FPS, installs next to the stock Artemis app, and is published as a public APK in a GitHub Release of this repository. No personal information may appear in code, commits, the APK or the release.

## 2. The key constraint and the decision

A Moonlight or Artemis client cannot make the host capture a single window. It only receives the encoded video that the host decides to produce, and today Vibepollo always encodes a whole display. The work therefore has to happen on the host, with only a small client change to ask for it.

### 2.1 Options considered

**(a) Host-side window capture with Windows Graphics Capture (WGC).** `IGraphicsCaptureItemInterop::CreateForWindow(HWND)` builds a capture item for one top-level window. DWM hands over that window's own surface, so nothing drawn on top of it (other windows, the Start menu, toasts) can leak into the frame. The user's own prior-art project `thorstream` proved this on real hardware: a topmost full-screen magenta window covering the game produced a magenta screenshot but a clean game frame (see `FEASIBILITY.md` in that repo). Vibepollo already runs WGC in a helper process (`tools/sunshine_wgc_capture.cpp`) and only calls `CreateForMonitor` today, so the plumbing (IPC, shared texture, HDR formats, pacing, DXGI fallback on the secure desktop) already exists.

**(b) Virtual display plus "tidy the desktop".** Vibepollo can already create a virtual display that matches the client (SudoVDA), and can make it the only or primary display. We could add a black wallpaper, hide the taskbar and maximize the game. This is cheap and has zero capture overhead, but the desktop still shows during launch, loading, alt-tab, when the game crashes, and whenever a notification, UAC-less dialog, overlay or other window appears. It does not meet "only the window".

**(c) Client-side crop.** The host sends the window rectangle over a custom control packet and the client crops the decoded video. The desktop around and on top of the window is still encoded and transmitted (it leaks in the bitstream and during every resize race), the encoder wastes resolution on pixels that are thrown away, overlapping windows still show inside the crop, and both the Android renderer (MediaCodec surface scaling) and client input mapping would need rewriting. Rejected.

### 2.2 Decision

Implement **(a) on top of (b)'s virtual display**, using a "masked true-position" composite:

1. The WGC helper captures the chosen window with `CreateForWindow`.
2. The helper copies only the window's client area into the existing full-display-sized shared texture, **at the same position the client area occupies on the capture display**, and fills every other pixel with black.
3. The shared texture keeps exactly the display's size and format, so the main process, encoder, image pools, HDR handling, and above all **input coordinate mapping stay unchanged**. Absolute mouse and touch coordinates from the client already map to display coordinates through `video::make_port()` (`src/video.cpp:5018`) and `input.cpp`; because the window's pixels sit where the window really is, a tap on the stream lands on the same spot in the window with no new math.
4. To make the window fill the stream, the host moves the target window onto the capture display and maximizes it if it is resizable. With the client's default of a Vibepollo virtual display at 1920x1080 and 120 Hz, a borderless game covers the display exactly, and the composite degrades to a single full-size copy per frame.
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
| Exclusive fullscreen | Legacy exclusive-fullscreen games may produce no WGC window frames. Borderless fullscreen is confirmed working (thorstream). Documented limitation. |
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
- Prior art by the same owner: https://github.com/drunkitguy/thorstream (`FEASIBILITY.md`, `host/src/window_capture.cpp` `ComputeCrop()`, `host/src/window_list.cpp`, `host/src/session.cpp` window placement)
- Client base: https://github.com/drunkitguy/artemis-apollo2 at `4d5eb3b`, a fork of https://github.com/ClassicOldSong/moonlight-android
- Client protocol library: https://github.com/drunkitguy/moonlight-common-c-apollo2 at `5efdcee` (adds 0x3003), fork of https://github.com/ClassicOldSong/moonlight-common-c; nested `enet` from https://github.com/cgutman/enet at `115a10b`
- GitHub runner image contents (Android SDK, NDK 27.3/28.2/29.0 installed, JDK 17 default): https://github.com/actions/runner-images/blob/main/images/ubuntu/Ubuntu2404-Readme.md
- Release action: https://github.com/softprops/action-gh-release ; SDK setup: https://github.com/android-actions/setup-android
- Local code read for this plan: `src/nvhttp.cpp`, `src/rtsp.h`, `src/rtsp.cpp`, `src/video.h`, `src/video.cpp`, `src/input.cpp`, `src/process.cpp`, `src/platform/windows/display_base.cpp`, `display_wgc.cpp`, `foreground_app.cpp`, `ipc/pipes.h`, `ipc/ipc_session.cpp`, `tools/sunshine_wgc_capture.cpp`, `tools/playnite_launcher/focus_utils.cpp`, and in the client `NvHTTP.java`, `StreamConfiguration.java`, `PreferenceConfiguration.java`, `StreamSettings.java`, `Game.java`, `AppView.java`, `app/build.gradle`.

## 4. Protocol: the `windowOnly` launch flag

Moonlight-family clients start and resume streams with HTTPS GET `/launch` or `/resume` and a query string. Artemis builds it in `NvHTTP.launchApp()` (client `app/src/main/java/com/limelight/nvstream/http/NvHTTP.java`, around line 880), and already sends Apollo extensions such as `virtualDisplay=` and `scaleFactor=`. The host reads these in `make_launch_session_from_snapshot()` (`src/nvhttp.cpp:1826`), which both `launch()` (line 3144) and `resume()` (line 3663) call.

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
  - `frame_bounds` is `DWMWA_EXTENDED_FRAME_BOUNDS` of the window (the WGC surface origin; not `GetWindowRect`, which includes the invisible resize border, as thorstream measured).
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
   - `process_frame()` (line 1308): before moving `frame` into `queue_frame_for_delivery()`, read `frame.ContentSize()`. After delivery, if window mode and the content size differs from the pool size, update `_pool_width/_pool_height` and call `create_or_adjust_frame_pool(_current_buffer_size)` (this must happen after the frame is released, as in thorstream `OnFrameArrived`).
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
2. `std::vector<window_policy::window_candidate_t> enumerate_candidates(...)`: `EnumWindows` over top-level windows keeping those that are visible, not cloaked (`DWMWA_CLOAKED`), root (`GetAncestor(GA_ROOT) == hwnd`), not `WS_EX_TOOLWINDOW`, not owned (`GetWindow(GW_OWNER) == nullptr`) unless it is the foreground window, client area at least 64x64, and not desktop UI. Mark `matches_app` with the matcher, `is_foreground` against `GetForegroundWindow()`, and `iconic` with `IsIconic`.
3. Selection rules (implemented in `window_policy::choose_target()`):
   - If an app matcher exists: eligible windows are those that match, plus those whose process was created after the session's launch time (`GetProcessTimes`, fills `started_after_launch`; this catches games a launcher starts outside the job object). Pick the foreground window if eligible; else the current target if still eligible; else the eligible window with the largest client area (ties broken by Z order, which is `EnumWindows` order); else none (black). Windows that existed before the launch and do not match are never chosen, so the stream stays black while the app is starting.
   - If no matcher exists (the "Desktop" app, or apps started through URLs such as `steam://` whose launcher process exits and auto-detaches): follow the foreground window if it is a valid non-shell candidate; else keep the current target if still valid; else none (black).
   - Minimized candidates are only chosen if nothing else qualifies.
   - Launch time: add `void mark_launch()` and `FILETIME launch_time()` (atomic 64-bit storage) to `window_target`, and call `mark_launch()` at the start of `proc::proc_t::execute()` in `src/process.cpp` under `#ifdef _WIN32`. A candidate is `started_after_launch` when its process creation time is later than `launch_time()` and `launch_time()` is non-zero.
4. `class tracker_t`: constructed with the capture display rectangle (`captured_output_desc.DesktopCoordinates`). Owns a `std::jthread` that calls `syncThreadDesktop()` (`misc.cpp:489`) once, then every 250 ms enumerates, chooses, and feeds `target_debouncer_t` with `polls_required = 3`. Exposes `std::uintptr_t select_now()` (synchronous, used at display init) and `std::uintptr_t committed_target() const` (atomic read). Logs each committed change at info level with pid, executable base name and window class (no titles, to avoid logging document names).
5. `void prepare_target(HWND hwnd, const RECT &output)`, called once per newly committed target:
   - If the window's `DWMWA_EXTENDED_FRAME_BOUNDS` is not fully inside `output`, `ShowWindow(SW_RESTORE)` if maximized elsewhere, then `SetWindowPos` its top-left to `output.left/top` with `SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE`.
   - If it does not already cover `output` and has both `WS_THICKFRAME` and `WS_MAXIMIZEBOX`, `ShowWindow(hwnd, SW_MAXIMIZE)`. Windows that already cover the display (borderless fullscreen) are never touched. Fixed-size windows are only moved.
   - Focus with `playnite_launcher::focus::try_focus_hwnd(hwnd)` (`tools/playnite_launcher/focus_utils.cpp`, already linked into the main binary).
6. `src/platform/windows/ipc/ipc_session.h/.cpp`: add `void set_window_target(std::uintptr_t hwnd)` and `std::uintptr_t window_target() const`. In the helper start-up where `config_data` is filled (`ipc_session.cpp` around line 330), set `config_data.target_hwnd` and, when `_config.window_only`, OR `WGC_IPC_FLAG_WINDOW_CAPTURE` into `config_data.flags` (next to `wgc_ipc_flags(_config)`).
7. `src/platform/windows/display_wgc.cpp` and the class declarations in `display.h` (line 459 onward):
   - Add `std::unique_ptr<window_target::tracker_t> _window_tracker;` and `std::chrono::steady_clock::time_point _last_window_reinit;` to both `display_wgc_ipc_vram_t` and `display_wgc_ipc_ram_t`.
   - In `display_wgc_ipc_vram_t::init()` (line 214) and `display_wgc_ipc_ram_t::init()` (line 470), when `config.window_only`: create the tracker with `captured_output_desc.DesktopCoordinates`, call `select_now()`, `prepare_target()` if non-zero, and `_ipc_session->set_window_target(hwnd)` before the first `initialize_if_needed()`.
   - In both `snapshot()` implementations (line 249 and line 512), right after the existing `should_reinit()` check: if the tracker's committed target differs from `_ipc_session->window_target()` and at least 2 s have passed since the last window-driven reinit, log "Window-only target changed; restarting capture" and `return capture_e::reinit;`. The capture loop in `video.cpp` (line 2906) then rebuilds the display, whose `init()` picks up the new target.
   - Factor the shared init and snapshot logic into two small helpers in the anonymous namespace so the VRAM and RAM variants do not duplicate it.

### 5.6 Things deliberately not changed

`src/input.cpp`, `video::make_port()`, the touch-port mailbox, `stream.cpp`, audio, the control stream, and the 0x3003 text-field packet all stay as they are. No config file keys or web UI settings are added in this first version; the client flag is the only switch.

## 6. Client changes (Android)

### 6.1 Where the client source lives

Vendor a snapshot into this repository under `clients/android/`:

- Base: `drunkitguy/artemis-apollo2` at `4d5eb3b` (Artemis plus the owner's dual-screen keyboard work, which pairs with this host's 0x3003 packet and already has a working APK workflow design), not upstream Artemis `c5cf27f`, which lacks the 0x3003 handling.
- `app/src/main/jni/moonlight-core/moonlight-common-c/` filled with `drunkitguy/moonlight-common-c-apollo2` at `5efdcee` (the commit the fork's submodule pins), and its nested `enet/` filled with `cgutman/enet` at `115a10b`.
- Remove `.gitmodules` from the copy and every nested `.git` directory, so the tree is plain files.

Justification: the APK must be built by this repository's Actions, and the session can only push here. Submodules would point at other repositories this work cannot change. A git subtree would pull tens of megabytes of unrelated client history into the host repo. A plain snapshot is the smallest thing that builds reproducibly. Record provenance in `clients/android/UPSTREAM.txt`: the three upstream URLs and commit SHAs, the licence (GPL-3.0 for the client and moonlight-common-c, MIT for enet, OpenSSL and Opus licences for the bundled prebuilt libraries), and a list of the local modifications. Keep every upstream `LICENSE*` file and source header intact. This repository is itself GPL-3.0, so combining is compatible, and publishing the source here satisfies the GPL source-availability requirement for the released APK.

Vendoring gotchas that must be handled:

- The host's root `.gitignore` ignores `*.a`, `*.so` and `build/`. The client depends on committed prebuilt `libopus.a`, `libssl.a` and `libcrypto.a` under `app/src/main/jni/moonlight-core/{libopus,openssl}/<abi>/`. Add `clients/android/.gitignore` containing the client's own ignore rules plus `!*.a`, and after `git add` verify with `git ls-files clients/android | wc -l` against `find` in the source snapshot that nothing was dropped.
- The client `.gitignore` ignores `key/`, so the signing keystore goes in `clients/android/signing/`.
- Keep `gradlew` executable (`git update-index --chmod=+x clients/android/gradlew`).
- The large `app/src/main/assets/midas-midas-v2-w8a8.tflite` is a normal blob (no LFS), keep it.

### 6.2 Identity, name and defaults

Paths below are inside `clients/android/`.

1. `app/build.gradle`
   - Flavor `nonRoot_game` (line 59): `applicationId "app.vibewindow.client"`; flavor `root` (line 43): `applicationId "app.vibewindow.client.root"`. Replace the `nonRoot_game` `obtainium_app_url` with the same neutral `data:` URL the root flavor uses.
   - `release` build type (lines 137 to 140): remove `applicationIdSuffix ".noir"`; set `app_label` to "Vibe Window", `app_label_root` to "Vibe Window (Root)", `app_label_game` to "Vibe Window (Game)"; add `signingConfig signingConfigs.ci`. `debug` build type (lines 98 to 101): suffix `.debug`, labels "Vibe Window Debug". These changes also satisfy the upstream comment asking forks to change the application id.
   - `defaultConfig` (lines 14 and 15): `versionName (project.findProperty('windowClientVersionName') ?: "1.0.0")` and `versionCode Integer.parseInt((project.findProperty('windowClientVersionCode') ?: "1").toString())`. Do not use Groovy `as int` on a String, which turns a one-character string into its character code.
   - Add `ndk { abiFilters 'arm64-v8a' }` to `defaultConfig` and set `splits.abi.enable false` (line 155). The Thor is arm64; this cuts NDK build time about four times and yields one APK.
   - Add a `signingConfigs { ci { ... } }` block: `storeFile file("${rootDir}/signing/window-client-ci.p12")`, `storePassword` and `keyPassword` "vibewindow-public", `keyAlias` "vibewindow", `storeType "pkcs12"`. No secrets override in this version.
   - In the `lint {}` block add `checkReleaseBuilds false` and `abortOnError false`, because the fork was only ever built in debug and lint-vital must not block a release.
   - Leave `ndkVersion "27.0.12077973"` as upstream pins it; CI installs it.
2. `signing/window-client-ci.p12`: generate once, locally, with
   `keytool -genkeypair -keystore window-client-ci.p12 -storetype PKCS12 -alias vibewindow -keyalg RSA -keysize 3072 -validity 10000 -storepass vibewindow-public -keypass vibewindow-public -dname "CN=Vibe Window CI"`.
   The DN contains no personal data. This key is public on purpose so every CI build has the same signature and new releases install as updates over old ones without a secret having to be configured (this session cannot set repository secrets). Verify with `git check-ignore -v` that the `.p12` is not ignored.
3. Defaults (fresh install, and the new application id means a fresh preferences store):
   - `app/src/main/java/com/limelight/preferences/PreferenceConfiguration.java`: `DEFAULT_RESOLUTION = "1920x1080"` (line 137), `DEFAULT_FPS = "120"` (line 138), `DEFAULT_USE_VIRTUAL_DISPLAY = true` (line 141).
   - `app/src/main/res/xml/preferences.xml`: `android:defaultValue="1920x1080"` for `list_resolution` (line 13), `"120"` for `list_fps` (line 21), `"true"` for `checkbox_use_virtual_display` (line 81). `PcView` applies these via `PreferenceManager.setDefaultValues()`.
   - The default bitrate follows automatically from `getDefaultBitrate("1920x1080", "120")` (line 487).
   - Note: `StreamSettings` removes the 120 option only on screens reporting under 118 Hz (line ~608). The Thor's primary screen reports 120 Hz, so the default holds.
4. Window-only flag:
   - `PreferenceConfiguration.java`: `private static final String WINDOW_ONLY_PREF_STRING = "checkbox_window_only";`, `private static final boolean DEFAULT_WINDOW_ONLY = true;`, public field `windowOnly`, read in `readPreferences()` next to `useVirtualDisplay` (line 873).
   - `preferences.xml`: a `CheckBoxPreference` with key `checkbox_window_only`, default `true`, directly after `checkbox_use_virtual_display` (line 85).
   - `app/src/main/res/values/strings.xml` (near line 540): `title_checkbox_window_only` "Show only the app window" and `summary_checkbox_window_only` "Stream only the launched app's window and hide the Windows desktop. Requires the Vibepollo window-only host build." English only; other locales fall back.
   - `app/src/main/java/com/limelight/nvstream/StreamConfiguration.java`: field `windowOnly` (near line 17), `Builder.setWindowOnly(boolean)`, getter `getWindowOnly()` (near line 198), default `false` in the constructor (line 161).
   - `app/src/main/java/com/limelight/Game.java`: add `.setWindowOnly(prefConfig.windowOnly)` in the `StreamConfiguration.Builder` chain (line 828).
   - `NvHTTP.java`: append `"&windowOnly=" + (context.streamConfig.getWindowOnly() ? 1 : 0)` after the `virtualDisplay` parameter (line 887). This covers both `launch` and `resume`.
5. No changes to the renderer, input, or JNI code.

## 7. CI workflow for the APK and the host installer

This session can only push to branch `claude/clever-clarke-k7hhhj`. Tag pushes may be refused and `workflow_dispatch` only works for workflows that exist on the default branch, so the release must be created from a normal branch push. The APK is useless without the patched host, so the same release also carries the host MSI.

New file `.github/workflows/window-only-release.yml`, independent of `ci.yml`. The release tag prefix `window-client-v` does not match `ci.yml`'s release gate regex, so it never triggers a host SignPath release.

- `name: Window-only release`
- Triggers: `push` to branches `claude/clever-clarke-k7hhhj` and `vibepollo-base` with `paths: [clients/android/**, src/**, tools/**, tests/**, cmake/**, .github/workflows/window-only-release.yml]`. No `pull_request` trigger.
- Top-level `permissions: {}`; `concurrency: { group: window-only-release-${{ github.ref }}, cancel-in-progress: true }`.
- Version: `clients/android/VERSION` holds one line, for example `1.0.0`. Tag is `window-client-v<VERSION>`.
- Job `android` on `ubuntu-latest`, `permissions: contents: read`, `defaults.run.working-directory: clients/android`, `timeout-minutes: 60`:
  1. `actions/checkout` (same SHA `ci.yml` pins for v6.0.2).
  2. `actions/setup-java@v4`, `temurin`, `'17'`, `cache: gradle`, `cache-dependency-path: clients/android/**/*.gradle*`.
  3. `android-actions/setup-android@v3`, then `sdkmanager --install "ndk;27.0.12077973"`.
  4. `VERSION_NAME=$(cat VERSION)`, `VERSION_CODE=$((1000 + GITHUB_RUN_NUMBER))`.
  5. `chmod +x gradlew && ./gradlew :app:assembleNonRoot_gameRelease --no-daemon --stacktrace -PwindowClientVersionName=$VERSION_NAME -PwindowClientVersionCode=$VERSION_CODE`.
  6. Copy the APK to `dist/VibeWindow-$VERSION_NAME-arm64-v8a.apk`. Locate apksigner with `ls -d $ANDROID_HOME/build-tools/* | sort -V | tail -1` and run `apksigner verify --print-certs` (fails the job if unsigned). Write `dist/*.sha256`.
  7. `actions/upload-artifact@v4` named `vibe-window-apk`, `if-no-files-found: error`.
- Job `windows`: `uses: ./.github/workflows/ci-windows.yml` exactly as `ci.yml`'s `build-windows` calls it, with `build_only: false`, `build_tests: true`, `release_commit: ${{ github.sha }}`, `release_version: 0.0.0`, `release_artifact_retention_days: 7`, `symbol_product_name: Vibepollo`, `symbol_release_prefix: polo`, `publish_symbols: false`, `require_truehdr_runtime: false`, `permissions: actions: read, contents: read`, and no secrets. This compiles the host, the helper and the unit tests (including `test_component_window_capture_policy`) and uploads `unsigned-msi-Windows`.
- Job `release`, `needs: [android, windows]`, `permissions: contents: write`:
  1. Checkout, read `VERSION`, and skip the remaining steps if `gh release view window-client-v$VERSION` succeeds (`GH_TOKEN: ${{ github.token }}`), so re-pushes do not fail.
  2. `actions/download-artifact@v4` for `vibe-window-apk` and `unsigned-msi-Windows` into `dist`. Rename the MSI to `Vibepollo-WindowOnly-$VERSION.msi` and add its `.sha256`.
  3. `softprops/action-gh-release@v2` with `tag_name: window-client-v$VERSION`, `target_commitish: ${{ github.sha }}`, `name: Vibe Window $VERSION`, `files: dist/*`, `fail_on_unmatched_files: true`, `make_latest: true`, `generate_release_notes: false`, and a fixed `body`: what it is, install the MSI on the Windows PC first (unsigned, so SmartScreen asks "More info, Run anyway"), sideload the arm64 APK on the Thor, the window-only toggle, known limitations (popups, exclusive fullscreen, UAC shows the secure desktop), and that source is in this repository under GPL-3.0. No names, emails, handles or local paths.

How to publish the first release: set `clients/android/VERSION` to `1.0.0` and push the branch. If `android` or `windows` fails, fix and push again; the release is created by the first fully green run.

If the `windows` job fails for reasons unrelated to this change (for example a missing secret), record the cause, and publish the APK-only release by temporarily making `release` depend on `android` only, stating in the body that the host build is pending.

## 8. Acceptance criteria

Build and release:

1. `.github/workflows/window-only-release.yml` `android` and `windows` jobs succeed on the feature branch.
2. A GitHub Release tagged `window-client-v1.0.0` exists in `drunkitguy/apollo2-vibe`, is public, contains the APK, the host MSI and their `.sha256` files, and `apksigner verify` passed in the log.
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
| Target window on another display (virtual display not isolated). | `prepare_target()` moves it onto the capture display. If a game fights the move, the stream shows its part that lies on the display, black elsewhere. |
| Wrong window chosen (launcher kept alive, overlay processes, games that spawn outside the job object). | Foreground-follow fallback, stickiness, debounce; logs name exe and class for each change so the rules can be tuned. The client toggle turns the feature off. |
| Capture restart on every target change causes a 0.5 to 1.5 s freeze. | Debounce (750 ms) and 2 s cooldown. Follow-up: in-place retarget message to the helper. |
| Menus and popups not visible for desktop apps. | Documented; games are the target use. Follow-up: composite owned popups as extra WGC items. |
| Exclusive-fullscreen games deliver no window frames. | Documented; ask the user to use borderless. Follow-up: fall back to display capture when `fullscreen_detector` reports exclusive D3D fullscreen. |
| Yellow capture border drawn on the host monitor on some Windows builds. | `IsBorderRequired(false)` is already attempted. The border is never part of the captured frame, so the stream is unaffected. |
| Publicly known signing key allows a third party to build an APK that installs over this one. | Only matters for APKs obtained elsewhere; documented in `UPSTREAM.txt`. A private key via repository secrets is a follow-up. |
| Jitpack or Maven outages break the Android build. | Gradle cache in `setup-java`; rerun. |
| Release-type build trips lint or R8 issues never seen in the fork's debug builds. | Lint made non-blocking; the fork's `proguard-rules.pro` is the same one upstream uses for its release builds. If R8 still fails, fall back to `assembleNonRoot_gameDebug` with the same signing config and debug label changes, and note the switch in the release body. |
| `config_data_t` size change between main and helper. | Both are built together and installed together; the helper already rejects messages of unexpected size. |
| Crash-log email string in the client points at the Artemis upstream maintainers. | Left as is (third-party attribution, not personal data of the user). Optional: blank `email_recipient` in `strings.xml` so logs are not sent upstream for a fork. |

## 10. Follow-ups (out of scope for the first version)

- In-place retarget: a runtime IPC message carrying a new HWND so the helper swaps the capture item and session without restarting the encoder.
- Optional scale-to-fit for small fixed-size windows, with matching input remapping through `touch_port_t`.
- Per-app `window-only` setting in `apps.json` and the web UI, and a global host config key.
- Per-process audio via WASAPI process loopback.
- Composite owned popup windows of the target.
