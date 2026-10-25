# Plan: "VoidLink feel" for v1.3.0 (Android client and window-only host)

Status: the host items (H1 to H4 and H6) are implemented for window-only host 1.3.0 on branch `claude/clever-clarke-k7hhhj` of this repository. The client items live in `drunkitguy/vibe-window`. Evidence for every item comes from a performance audit of both code bases that is not part of this repository.

Line numbers refer to the revisions read on 2026-10-19:

- Client: `drunkitguy/vibe-window` at d45ca75 (VERSION 1.2.2).
- Host: this repository at fa51a47 (.github/window-only-host-version.txt 1.2.2).

The Tizen TV client is paused and is not part of this plan.

Goal: make the Android client present the newest decoded frame at each display refresh with at most one frame held, decode with the strongest low-latency settings the Snapdragon decoder accepts, and keep Wi-Fi awake. On the host, smooth the send bursts for Wi-Fi, stop putting audio in the Wi-Fi voice queue, and remove a startup failure in the capture helper.

Scope decisions, stated up front:

- **Window-only copy reduction (a double-buffered shared texture) is NOT in 1.3.0.** It changes the helper-to-host IPC protocol (`ipc/pipes.h` `config_data_t` has fixed offsets with `static_assert`s), the keyed-mutex hand-off, and every reinit, resize and HDR path. It cannot be exercised on Linux. 1.3.0 only adds timing instrumentation (host item H4) so the later change can be justified with numbers. The design sketch is at the end.
- **Native thread priority (audit item 8, C7) is NOT in 1.3.0** (manager decision). The audit rates it weak on Android with unmeasured impact, and it adds native code that CI can only compile, not exercise. Revisit after the 1.3.0 measurements if decode time or network latency degrade under CPU load.

---

## CLIENT: drunkitguy/vibe-window, VERSION 1.2.2 to 1.3.0

### Constraints

- `Game.java` changes are limited to (manager decision, every other line stays byte for byte):
  - the latency-profile block (691 to 706): only the two `framePacing` assignments are removed;
  - the cap-fps fallback block (757 to 773): replaced by one call to `PacingModeResolver.resolve(...)` with identical results, proven by its tests;
  - the keepalive period in the `backgroundPing` runnable (333 to 338) and the keepalive start condition (3718 to 3720);
  - one read of the new pacing mode where the renderer is configured.
- Do not touch:
  - the window-only flag plumbing;
  - the AutoResolution hook;
  - the launch query;
  - the warp factor (776 to 777);
  - the surface `setFrameRate` code (3833 to 3845), which already covers API 30 and 31+.
- New logic goes into new small classes that do not depend on Android where possible, so that it can be tested on the JVM.

### C1. Honour the user's frame pacing choice

Where:

- `app/src/main/java/com/limelight/Game.java` 691 to 706. Both branches currently set `prefConfig.framePacing = PreferenceConfiguration.FRAME_PACING_BALANCED`.
- `MediaCodecDecoderRenderer.java`:
  - `startChoreographerThread()` 1135 to 1152 (only starts for BALANCED);
  - the LATEST_ONLY block 1212 to 1247;
  - the render branches 1262 to 1450.

Behaviour:

- Remove the two `prefConfig.framePacing = FRAME_PACING_BALANCED` assignments. Keep the `setPreferLowerDelays(...)` and `setPreferLowerDelaysTimeoutUs(...)` calls exactly as they are (500 us when preferLowerDelays, else 2000 us), so Balanced users get the same timings as today.
- Add a pure resolver `com.limelight.binding.video.PacingModeResolver`:
  - Signature: `static int resolve(int userMode, float fps, int displayHz)`.
  - It contains the existing cap-fps fallback rules, moved unchanged (FPS more than 3 above Hz becomes Balanced; Hz of 49 or less becomes Balanced).
  - Game.java 757 to 773 calls it instead of the inline code, so the block's behaviour stays identical.
- Keep each existing mode's behaviour as users experience it today. Today everyone gets "Balanced plus the LATEST_ONLY block", because of the override. Rules per mode:
  - **Balanced:** unchanged. LATEST_ONLY block plus the 2-deep Choreographer queue.
  - **Old "Prefer lowest latency" (`latency`):** the upstream immediate path (1262 to 1290 and 1350 to 1420), plus the LATEST_ONLY block.
  - **Smoothest (`smoothness`) and Cap FPS (`cap-fps`):** skip the LATEST_ONLY block. That block releases frames without pacing, which breaks those modes' never-drop contract. Their own render branch then applies.
  - **New `newest` mode:** described in C2.

Tests (JVM, `app/src/test/java/com/limelight/binding/video/PacingModeResolverTest.java`):

- Each user mode passes through unchanged when fps is at most Hz plus 3.
- cap-fps at fps 120 on a 60 Hz panel becomes Balanced.
- cap-fps on a 48 Hz panel becomes Balanced.
- `newest` is never rewritten.

Measure:

- logcat shows a new line, "Frame pacing mode: <name>", printed where the 698/704 lines are today.
- Overlay: pick each mode in turn and confirm the behaviour changes (rendered fps vs received fps).

Risk: users who picked Smoothest or Cap FPS without noticing (the setting had no effect) now get those modes. Acceptable, because it is their stored choice.

Kill switch: the setting itself.

### C2. New default mode "Lowest latency (newest frame)"

New files:

- `app/src/main/java/com/limelight/binding/video/NewestFramePacer.java`. Pure Java, no Android imports. It is driven through a small `Releaser` interface: `release(int index, boolean render)`.

Pacer state (guarded by one lock object):

- `pendingIndex` (-1 when empty), `pendingPtsUs`.
- `presentedThisVsync` (boolean).
- Counters: `presented`, `droppedByPacing`, `releasedImmediately`, `releasedAtVsync`.

Rules:

1. `onDecodedFrame(index, ptsUs)`, called on the renderer thread for every output buffer:
   - If `presentedThisVsync` is false (nothing presented since the last vsync), release it now with `render=true`. Set `presentedThisVsync = true` and count `releasedImmediately`. This is the "decoder delivers right after vsync" case: the frame goes out at once and makes the next latch.
   - Otherwise, if a frame is already pending, release the pending one with `render=false` and count `droppedByPacing`. Then store the new one as pending (a 1-frame queue that always holds the newest frame).
2. `onVsync(frameTimeNanos)`, called on the Choreographer thread:
   - If a frame is pending, release it with `render=true`, set `presentedThisVsync = true`, count `releasedAtVsync`.
   - Otherwise set `presentedThisVsync = false`. This arms the immediate path for the coming interval.
3. `clear()`, for flush, recovery, or stop: forget the pending index without releasing it. The codec flush invalidates it. Reset `presentedThisVsync = false`.

Why this shape:

- It is the Android equivalent of VoidLink's queue mode (see `FrameQueue.m`, and `renderModeAVSB` in `VideoDecoderRenderer.m` 539 to 613): at most one frame per refresh, always the newest, and no frame waits for a vsync if the current interval has not shown anything yet.
- When the stream fps is lower than the panel Hz (60 on 120), every frame takes the immediate path, so nothing is added.
- When two frames land in one interval, the older one is dropped explicitly and counted, instead of being silently replaced inside SurfaceFlinger.

Wiring in `MediaCodecDecoderRenderer.java`:

- `startChoreographerThread()` (1135): also start the thread for `FRAME_PACING_NEWEST`. The existing thread runs at `THREAD_PRIORITY_URGENT_DISPLAY`, and codec recovery already handles that thread (`doCodecRecoveryIfRequired`, 808 to 850).
- `doFrame()` (1068): when the mode is `newest`:
  - skip the 80% gate and the `outputBufferQueue`;
  - call `pacer.onVsync(frameTimeNanos)`;
  - then call `doCodecRecoveryIfRequired(CR_FLAG_CHOREOGRAPHER)` and re-post, as the current code does.
- Renderer thread loop (1200 onward): when the mode is `newest`:
  - skip the LATEST_ONLY block;
  - block on `dequeueOutputBuffer(info, getOutputDequeueTimeoutUs())`;
  - for each output buffer index, call `pacer.onDecodedFrame(index, info.presentationTimeUs)`;
  - keep the existing stats calls (`numFramesOut++`, `updateDecodeLatencyStats` when rendered).
- The `Releaser` maps `render=true` to `videoDecoder.releaseOutputBuffer(index, true)` and `render=false` to `releaseOutputBuffer(index, false)`. On `IllegalStateException`, fall back to `releaseOutputBuffer(index, false)`, as the current code does at 1115.
- Codec recovery (829 to 831): next to `outputBufferQueue.clear()`, also call `pacer.clear()`. Do the same in `stop()` / `cleanup()` before the codec is released.
- Stats:
  - `activeWindowVideoStats.totalFramesRendered++` on each `render=true`;
  - a new field `framesDroppedByPacing` in `VideoStats` (`binding/video/VideoStats.java`), with add and copy kept in step with the other fields.

Preferences:

- `PreferenceConfiguration.java`:
  - add `FRAME_PACING_NEWEST = 4` next to 242 to 245;
  - map the string `"newest"` in `getFramePacingValue` (691 to 707);
  - change `DEFAULT_FRAME_PACING` (206) to `"newest"`.
- `res/values/arrays.xml` 130 to 145: add the entry first in both arrays. Name `@string/pacing_newest`, value `newest`.
- `res/xml/preferences.xml` 61: `android:defaultValue="newest"`.
- `res/values/strings.xml`: add `pacing_newest` = "Lowest latency (newest frame)". Rename the label of `pacing_latency` to "Lowest latency (legacy)", keeping the value `latency`.

One-time migration:

- `PreferenceConfiguration.migrateFramePacingToNewest(SharedPreferences)`, modelled on `migrateToAutoDisplayDefaults` (763 to 793).
- Flag key `migrated_frame_pacing_newest_v130`.
- If the flag is not set, and the stored `frame_pacing` is `latency` or missing, store `newest`. Always set the flag afterwards.
- Every install has `latency` stored, because `PreferenceManager.setDefaultValues` runs at `PcView.java` 169, and the old setting had no effect anyway. So `latency` counts as the old default.
- Base preferences only. Profile overlays are not migrated, which matches the existing migration's rule.
- Call it in `PcView.java` next to line 171.

Tests:

- `NewestFramePacerTest.java` (JVM), using a fake Releaser that records calls:
  - one frame per interval is released immediately every time;
  - two frames in one interval: the first is released immediately and the second is held, then released at vsync;
  - three frames in one interval: the middle one is dropped with `render=false` and the counter goes up;
  - a vsync with nothing pending arms the immediate path;
  - after `clear()`, the next frame is released immediately and nothing stale is released;
  - a concurrency smoke test with two threads and 10,000 iterations: every index is released exactly once.
- Migration, Robolectric, added to `app/src/test/java/com/limelight/preferences/AutoPreferencesTest.java` or a new `FramePacingMigrationTest.java`:
  - `latency` becomes `newest`;
  - missing becomes `newest`;
  - `balanced`, `smoothness`, `cap-fps`, `warp` and `warp2` stay unchanged;
  - a second run is a no-op;
  - a profile overlay value is untouched.
- `StreamSettingsUiTest` (existing): the list shows the new entry first, and the default selection is `newest`.

Measure on the Thor (1080p120 HEVC, Wi-Fi):

- Overlay full mode: received fps and rendered fps should be within 0.5 at 120 fps. Compare with the 115 to 119 rendered that Balanced shows in Artemis #246. The new line "Frames dropped by pacing" (C6) should be below 1%.
- `adb shell dumpsys SurfaceFlinger --latency "<SurfaceView layer name>"` before and after: present intervals should sit at 8.3 ms with no 16.7 ms gaps.
- Press-to-photon: a 240 fps camera, 20 presses per mode (Balanced vs newest). The newest-mode median should be lower.

Risks:

- Uneven host cadence shows as small judder instead of being smoothed. Balanced is still selectable.
- Every MediaCodec call from two threads is already the pattern used today (the Choreographer releases buffers in Balanced).

Kill switch: choose Balanced. The 1.3.1 fallback is to change the default back.

### C3. Always apply the Qualcomm low-latency keys on Snapdragon

Where:

- `binding/video/MediaCodecHelper.java` `setDecoderLowLatencyOptions` (533 to 690). The early return is at 549 to 556.
- Retry loop: `MediaCodecDecoderRenderer.java` 756 to 777.

Behaviour:

1. Refactor the body into `static boolean applyLowLatencyOptions(MediaFormat format, String decoderName, boolean featureLowLatency, boolean ultraLowLatency, boolean vendorKeysEnabled, int tryNumber)`. The current method becomes a thin wrapper that computes `featureLowLatency` with `decoderSupportsAndroidRLowLatency`.
2. Change the early return so it is skipped for Qualcomm decoders when `vendorKeysEnabled` is true:
   - The condition becomes: `!ultraLowLatency && featureLowLatency && !(vendorKeysEnabled && isDecoderInList(qualcommDecoderPrefixes, decoderName))`.
   - Try 0 on the Thor then sets: `low-latency=1`, `vdec-lowlatency=1` (ignored by c2.qti), `KEY_OPERATING_RATE=Short.MAX_VALUE` (allowed by `decoderSupportsMaxOperatingRate`, since Adreno 740 is not 620), `vendor.qti-ext-dec-picture-order.enable=1`, `vendor.qti-ext-dec-low-latency.enable=1`, and the three output-fence keys (617 to 625).
3. The existing ladder then drops options try by try. Add a final safe try for Qualcomm with `featureLowLatency`: only `low-latency=1`, which is exactly today's configuration. The helper returns `true` (new format) for it, then `false` after it, so the loop at 756 ends with today's behaviour in the worst case. The try number for this extra step is `5` (after the qti block at `tryNumber < 5`). It must be added explicitly, because by then `low-latency` was already dropped at try 1.
4. Record the applied path:
   - A new field `MediaCodecDecoderRenderer.lowLatencyPath` (String), set after `tryConfigureDecoder` succeeds. Values: `qti-full` (try 0), `qti-ladder-<n>`, `android-feature-only`, or `none`.
   - Log it as "Decoder low-latency path: <path> (try <n>)".
5. New preference `checkbox_vendor_decoder_keys`, default `true`, in the advanced video section next to "Ultra low latency". Title "Use vendor decoder low-latency keys". Summary "Turn off if video fails to start or shows corruption." Field `PreferenceConfiguration.vendorDecoderKeys`.

Tests (JVM or Robolectric, `MediaCodecHelperLowLatencyTest.java`, using real `MediaFormat` under Robolectric):

- Qualcomm name `c2.qti.hevc.decoder` with featureLowLatency, vendorKeys on, try 0: the format contains `vendor.qti-ext-dec-low-latency.enable` and `vendor.qti-ext-output-sw-fence-enable.value`, and the call returns true.
- Same with vendorKeys off: only `low-latency` is set, and it returns true at try 0 (today's behaviour).
- Non-Qualcomm name with featureLowLatency: still returns early at try 0 (unchanged).
- At try 5 for Qualcomm with featureLowLatency: only `low-latency` is set and the call returns true. At try 6 it returns false.
- Static state such as `isAdreno620` defaults to false. Keep that default and document it in the test.

Measure:

- logcat "Low latency decoding mode supported (FEATURE_LowLatency)" and the new "Decoder low-latency path" line.
- Overlay "Average decoding time" over 2 minutes of the same scene, with vendor keys on vs off. Expect lower, or equal at worst.
- Watch for green frames or corruption after packet loss (forced with a brief Wi-Fi drop).

Risks:

- Some firmware rejects the fence keys in configure(). This is covered by the ladder and the final safe try.
- Firmware that accepts them but misbehaves needs the user switch.

Kill switch: `checkbox_vendor_decoder_keys`.

### C4. Wi-Fi keepalive on by default

Where:

- `Game.java` 333 to 338 (the `backgroundPing` runnable, 20 ms period) and 3718 to 3720 (start).
- `PreferenceConfiguration.java` 107, 216 and 1141.
- `res/xml/preferences.xml` 1003 to 1008.

Behaviour:

- `DEFAULT_PREVENT_PACKET_LOSS` becomes `true`, and `preferences.xml` gets `android:defaultValue="true"`.
- Title "Keep Wi-Fi awake while streaming". Summary "Sends a tiny packet every frame so the Wi-Fi radio does not doze. Only used on Wi-Fi."
- One-time migration `migrated_prevent_packet_loss_v130`: stored `false` becomes `true`, base preferences only, flag always set. The stored `false` comes from `setDefaultValues`, so for almost every user it is not a deliberate choice. A user who turns it off after 1.3.0 keeps it off.
- Wi-Fi gating:
  - New `utils/NetHelper.isActiveNetworkWifi(Context)`: `ConnectivityManager.getNetworkCapabilities(getActiveNetwork()).hasTransport(TRANSPORT_WIFI)` on API 23 and later, `false` otherwise.
  - At 3718: `if (prefConfig.preventPacketLoss && NetHelper.isActiveNetworkWifi(this))`.
  - Ethernet and cellular get no keepalive.
- Period: send every `max(8 ms, round(1000 / fps))` ms, capped at 20 ms. That is 8 ms at 120 fps and 16 ms at 60 fps, close to VoidLink's per-frame timer (`StreamFrameViewController.m` 2455 to 2463).
- The runnable keeps using `MoonBridge.sendEmptyPayload()`. The host ignores the unknown control type at debug level (`src/stream.cpp` in this repository, 1033 to 1039).

Tests (Robolectric):

- Migration: false becomes true once; an explicit false after the flag is set stays false; profiles are untouched.
- `NetHelper` with a ShadowConnectivityManager: Wi-Fi gives true, Ethernet gives false.
- The period helper `keepalivePeriodMs(fps)`: 120 gives 8, 60 gives 16, 30 gives 20.

Measure:

- Overlay "Average network latency (variance)" and "Frames dropped by your network connection" over 10 minutes idle desktop plus 10 minutes game, on vs off.
- Host debug log: unknown control type lines arrive at the expected rate. Use debug log level only for this check.

Risks:

- Extra uplink of 60 to 125 small reliable ENet packets per second. That is negligible on LAN.

Kill switch: the preference.

### C5. Remove the conflicting setFrameRate call

Where:

- `MediaCodecDecoderRenderer.java` 611 calls `applySurfaceFrameRate(renderTarget, targetFps)`, defined at 2412 to 2420, with `FRAME_RATE_COMPATIBILITY_DEFAULT`.
- `Game.java` 3833 to 3845 already sets the right rate (the display mode rate, `FIXED_SOURCE`, `CHANGE_FRAME_RATE_ALWAYS` on API 31 and later).

Behaviour: delete the call at 611 and the now unused helper. Game.java remains the single owner of the surface frame rate. Do not change Game.java here.

Test: a source-level guard test (JVM) that greps `MediaCodecDecoderRenderer.java` for `setFrameRate(`. This follows the existing pattern of structural tests in the suite, if one exists; otherwise skip it and rely on the measurement.

Measure: during a 60 fps stream on the 120 Hz panel, Developer options "Show refresh rate" or `adb shell dumpsys display | grep -i -E "refresh|mode"` should stay at 120 Hz. Today it can fall to 60.

Risk: none known. Kill switch: not needed.

### C6. Stats overlay additions

Where: `MediaCodecDecoderRenderer.java` 1780 to 1880 (overlay text) and `res/values/strings.xml` 128 to 136.

Full mode additions:

- Existing lines kept: `perf_overlay_incomingfps` and `perf_overlay_renderingfps` already exist (strings 130 and 131).
- New `perf_overlay_pacing`: "Frame pacing: %1$s, dropped by pacing: %2$.2f%%". The mode name comes from C1 and the percentage is `framesDroppedByPacing / totalFramesReceived`.
- New `perf_overlay_decoder_path`: "Decoder low-latency path: %1$s", from C3.
- Existing "Average decoding time" kept.

Lite mode addition: append "R/I" as rendered/incoming fps, for example "119.8/120.0".

Tests: Robolectric format test with a fake `VideoStats` (exact string for given numbers), only if the overlay formatting can be called without a live codec. Otherwise extract `PerfOverlayFormatter` as a pure helper and test that.

Measure: the overlay values match what `dumpsys SurfaceFlinger --latency` shows.

### C7. Native thread priorities

Cut from 1.3.0 (see scope decisions).

### C8. Release

- `VERSION` 1.2.2 to 1.3.0.
- Release notes in `.github/workflows/release.yml`, following the pattern of commit 03cd88b. Summarise:
  - the new default pacing mode and how to go back;
  - vendor decoder keys and their switch;
  - keepalive on Wi-Fi;
  - the refresh-rate fix;
  - the new overlay lines.

### Client commit order

1. `PacingModeResolver` and its tests; Game.java honours the user's pacing choice (C1). Old modes keep today's effective behaviour, with the LATEST_ONLY gating described in C1.
2. `NewestFramePacer` and its tests only, not wired in (C2 part 1).
3. Wire the `newest` mode into the renderer; arrays, strings and the preference default; migration and tests (C2 part 2).
4. Vendor keys: refactor, Qualcomm rule, final safe try, path reporting, preference, tests (C3).
5. Remove the renderer's setFrameRate (C5).
6. Keepalive default on Wi-Fi: migration, NetHelper, period, tests (C4).
7. Overlay lines (C6).
8. VERSION 1.3.0 and release notes (C8).

Each commit must pass `./gradlew testDebugUnitTest` (or the configuration CI already uses). Commits 3, 4 and 6 must be revertable on their own.

---

## HOST: this repository, version 1.2.2 to 1.3.0

### H1. WGC helper: GPU priority is best effort

Where: `tools/sunshine_wgc_capture.cpp`:

- `SystemInitializer::initialize_gpu_scheduling_priority()` (384 to 408);
- `initialize_all()` (437 to 445).

Behaviour (as implemented in 1.3.2, after stutter reports with 1.3.1):

1. The helper first asks for REALTIME exactly as 1.2.2 did, with no token privilege change. A setup where 1.2.2 got REALTIME (where streams worked, since failure was fatal then) keeps it, so a heavy game cannot starve the capture copies.
2. If REALTIME is refused, it asks for HIGH (`D3DKMT_SchedulingPriorityClass::HIGH`). If HIGH is refused too, it logs a warning and keeps running at the default GPU priority.
3. In `initialize_all()`, the GPU priority step is no longer fatal, like DPI awareness. The log keeps "GPU scheduling priority set: YES/NO" from `_gpu_priority_set`.

History: 1.3.0 also enabled `SeIncreaseBasePriorityPrivilege` before the REALTIME call; 1.3.1 asked only for HIGH, which is one suspect for the stutter reported with 1.3.1. 1.3.2 restores the 1.2.2 call and only adds the HIGH fallback.

Tests:

- Windows only, so there is no Linux unit test of the D3DKMT call.
- The pure helper `platf::dxgi::wgc_policy::helper_init_step_required()` in `src/platform/windows/wgc_capture_policy.h` says which init steps are fatal. `tests/unit/test_window_capture_policy.cpp` checks that GPU priority and DPI awareness are not fatal, and that thread priority, MMCSS and the WinRT apartment are. `initialize_all()` uses that table.

Measure: the helper log shows "GPU scheduling priority set to REALTIME for optimal capture performance" where 1.2.2 worked. Started as a normal user (the steps in Vibepollo#563), the stream must start and the log shows the REALTIME warning followed by "GPU scheduling priority set to HIGH" (or a second warning).

Risk: none where REALTIME succeeds (the 1.2.2 path). Where it is refused, the helper now runs at HIGH instead of exiting. Compare the H4 helper timers under GPU load if capture stutters there.

### H2. Audio QoS class option

Where:

- `src/stream.cpp` 2734 to 2736 (`enable_socket_qos` for audio).
- `src/platform/windows/misc.cpp` 2028 to 2125 (traffic type switch, Voice at 2117).
- `src/platform/linux/misc.cpp` 1022 to 1050, for parity.
- Config: `src/config.h` (`stream_t`), `src/config.cpp` defaults (around 972 to 979) and parsing (around 2076).
- Docs, locale and schema: `docs/configuration.md`, `src_assets/common/assets/web/public/assets/locale/en.json`, `src_assets/common/assets/web/configs/settingsSchema.ts` (next to `fec_percentage` at 95, 429 and 674). The contract test `tests/unit/test_config_catalog_contract.cpp` fails if any of these is missing.

Config key `audio_qos`:

- Values: `voice`, `video` and `off`.
- Default `video`.
- `voice` gives today's behaviour (qWAVE Voice, DSCP class used for the WMM voice queue).
- `video` tags audio like video (qWAVE AudioVideo on Windows, DSCP 40 on Linux).
- `off` sets no tag for audio.

Why the default is `video` and not `off`:

- In Sunshine#5508 the loss was specific to the voice queue, and video-class traffic was unaffected on the same link.
- `video` keeps audio above best effort on congested networks.

The client still decides whether any QoS applies. Only clients that send `qosTrafficType != 0`, which is LAN, are tagged.

Behaviour:

- A new header-only policy `src/qos_policy.h`:
  - `enum class audio_qos_e { voice, video, off }`;
  - `audio_qos_e audio_qos_from_view(std::string_view)` (unknown values log and return `video`);
  - `struct qos_plan_t { bool tag; platf::qos_data_type_e type; }`;
  - `qos_plan_t plan_audio_qos(audio_qos_e, bool client_requested)`.
- At stream.cpp 2736, call `enable_socket_qos(..., plan.type, plan.tag)`.
- Log once per session at info level: "Audio QoS: <class> (client requested: yes/no)".

Tests (Linux runnable, `tests/unit/test_qos_policy.cpp`, header only, same harness as `test_window_capture_policy.cpp`):

- The three values parse; an unknown value gives video.
- `plan_audio_qos(voice, true)` gives `{true, audio}`.
- `video` gives `{true, video}`.
- `off` gives `{false, *}`.
- `client_requested=false` always gives `tag=false`.

Measure:

- Client logcat and overlay: common-c audio FEC failure lines ("Unable to recover audio") per 10 minutes on the test access point, with `audio_qos=voice` vs `video`. Listen for crackle.
- Optional: Wireshark on the client side of the AP, checking the DSCP field of audio packets.

Risk: on some networks voice priority may have helped audio under heavy congestion.

Kill switch: `audio_qos=voice`.

### H3. Wi-Fi friendly video pacing by default

Where:

- `src/stream.cpp` 2136 to 2160 (`pacing_bps` selection; the session floor clamp at 2149 to 2158 stays).
- `src/config.cpp` 978 and 2076.
- `src/config.h` 278.
- Docs, locale and schema, as in H2.

Config:

- New key `pacing_mode`:
  - Values: `auto` (default) and `legacy`.
  - `legacy` keeps today's behaviour exactly (80% of 1 Gbps when `pacing_max_bitrate_kbps` is 0).
- Existing key `pacing_max_bitrate_kbps` keeps its meaning. When it is greater than 0 it always wins, with the existing floor clamp.
- New key `pacing_auto_multiplier_percent`, default 400, range 150 to 1000.

Auto behaviour, in a new header-only policy `src/stream_pacing_policy.h`, function `size_t resolve_pacing_bps(int configured_kbps, pacing_mode_e mode, int session_bitrate_kbps, int multiplier_percent)`:

- If `configured_kbps > 0`: return `max(configured_kbps * 1000, session_bitrate_kbps * 1000 * 110 / 100)`. This is today's rule, moved.
- If `mode == legacy`: return `800,000,000`.
- If `mode == auto`: `target = session_bitrate_kbps * 1000 * multiplier_percent / 100`, then clamp it to the range from `300,000,000` (floor) to `800,000,000` (ceiling, today's value). Also never go below 110% of the session bitrate.

Worked example at 120 fps:

- A 100 Mbps stream paces at 400 Mbps. An average 104 KB frame then takes about 2.1 ms on the wire instead of about 1.0 ms, and a 500 KB IDR takes about 10 ms instead of about 5 ms.
- A 50 Mbps stream paces at 300 Mbps.

The added send spread is the cost. The benefit is that the Wi-Fi AP receives an even stream instead of one 1 Gbps burst per frame. This is the AMPDU jitter problem the code's own comment describes at 2136 to 2140.

Logging:

- Once per session at info level: "Video pacing: <mode>, <N> Mbps (stream <M> Mbps)".
- Per frame: keep the existing debug loggers (1988 to 1993).

Tests (Linux runnable, `tests/unit/test_stream_pacing_policy.cpp`):

- legacy gives 800 Mbps.
- An explicit 200000 kbps with a 150 Mbps session gives max(200, 165) = 200 Mbps.
- An explicit cap below the session bitrate is clamped to 110%.
- auto: 100 Mbps gives 400 Mbps; 20 Mbps gives 300 Mbps (the floor); 300 Mbps gives 800 Mbps (the ceiling).
- auto stays at least 110% of the session bitrate for every value up to 800 Mbps.
- The multiplier is clamped to its range.

Measure:

- Client overlay "Frames dropped by your network connection" and latency variance, 5 minutes of high-motion content: legacy vs auto, same bitrate.
- Host debug log "Network: rate control sleep/late" and "frame packets sent" (`stream.cpp` 1988 to 1993).
- Press-to-photon on Ethernet, auto vs legacy. Expect at most about 1 to 2 ms worse at 120 fps. If it is worse than that, lower the multiplier default or keep legacy on wired.

Risk: slightly higher latency on wired clients.

Kill switches: `pacing_mode=legacy`, or set `pacing_max_bitrate_kbps` explicitly.

### H4. Window-only copy path: instrumentation only in 1.3.0

Where:

- Helper: the copy to the shared texture under the keyed mutex (2694 to 2730), the scratch copy (2597), and the composite (1755 to 1765).
- Host: `src/platform/windows/display_wgc.cpp` 398 to 455 (`AcquireSync` and `CopyResource` at 450).

Behaviour:

- Add `min_max_avg_periodic_logger` timers, the same class used in stream.cpp 1988, at debug level, for:
  - helper: AcquireSync wait, publish copy time, composite time;
  - host: shared-mutex wait and copy-to-encoder time.
- No behaviour change, and no IPC change.

Tests: none (logging only). Build check through the normal Windows CI.

Measure: at 4K120 under GPU load, record the averages. If the mutex wait plus the extra copy is more than about 0.5 ms per frame, schedule the double-buffer change below for 1.4.0.

Later item (NOT 1.3.0), design sketch for double buffering:

- Two shared textures with keyed mutexes and a frame sequence in the metadata.
- The helper writes slot `(seq + 1) % 2`; the host reads the newest completed slot.
- This removes the scratch-to-shared copy when no compositing is needed, and the cross-process wait.
- It needs a protocol version field in `config_data_t` (`ipc/pipes.h`, offsets checked by `static_assert` at 113), with the helper and host refusing mismatched versions.
- Test plan: resize, HDR toggle, desktop switch, and helper crash and restart.

### H5. Native thread priority on the host

Out of scope. The capture and helper threads already use MMCSS and `THREAD_PRIORITY_HIGHEST` (helper 336 to 358 and 2618 to 2624), so there is nothing cheap left to gain.

### H6. Release

- `.github/window-only-host-version.txt` 1.2.2 to 1.3.0, following the pattern of commit fa51a47.
- Release notes (wherever the host release body is kept): the auto pacing default and `pacing_mode=legacy`, `audio_qos`, the helper startup fix, and the new debug timers.

### Host implementation notes (1.3.0)

Where the implementation differs from or refines the items above:

- 1.3.2 (after stutter reports with host 1.3.1 and client 1.3.3, on Ethernet and Wi-Fi): the defaults are the 1.2.2 behaviour again. `pacing_mode` defaults to `legacy` and `auto` is experimental and opt-in; `audio_qos` defaults to `voice` and `video` is opt-in. The H2 and H3 defaults written above (`video`, `auto`) apply to 1.3.0 and 1.3.1 only.
- H1: the fatal-step table is `platf::dxgi::wgc_policy::helper_init_step_required()` in `src/platform/windows/wgc_capture_policy.h`, tested in `tests/unit/test_window_capture_policy.cpp`. Since 1.3.2 the helper tries REALTIME exactly as 1.2.2 did, then HIGH, and starts either way (see H1 above for 1.3.0 and 1.3.1).
- H2: the policy lives in namespace `stream_qos` (`src/qos_policy.h`). The plan's `qos_plan_t` returns its own `traffic_type_e` so the test does not need `platform/common.h`; `stream.cpp` maps it to `platf::qos_data_type_e`. Without a client request, and with `audio_qos=off`, the socket is set up exactly like a 1.2.2 session whose client asked for no QoS (type audio, no DSCP tag).
- H2 also documents the existing `text_field_detection` and `text_field_numeric_hints` options, which were missing from `docs/configuration.md` and `en.json` and kept the config catalog contract test from passing.
- H2 and H3: `audio_qos`, `pacing_mode` and `pacing_auto_multiplier_percent` are in a new "Stream traffic" group on the Network page of the web UI.
- H3: the policy lives in namespace `stream_pacing` (`src/stream_pacing_policy.h`) and returns where the rate came from, so the existing one-time warning for an explicit cap below the stream bitrate is kept. The pacing base is the encoder bitrate (`session->config.monitor.bitrate` after the FEC, audio and overhead adjustments), read from an atomic copy. In 1.3.0 auto only lowered the rate, which changed nothing for frames smaller than one 64 KB batch, because the 1.2.2 pacer only waits before a frame's first batch and after each millisecond of packets. Since 1.3.1 auto also sends every frame in batches of about 0.5 ms of packets (at least 4) and waits before every batch (`plan_send_schedule()`, `pace_before_batch()`, `due_offset()`, tested per frame for 20 to 150 Mbps at 60 and 120 fps), and its floor is 110% of the wire rate including FEC. The info line "Video pacing: <source>, <N> Mbps (encoder bitrate <M> Mbps), batches of up to <B> packets" is logged when a session starts sending and again only if its rate changes.
- H4: the helper does not link `config.cpp`, so it cannot use `logging::min_max_avg_periodic_logger`; it has a small equivalent (`helper_debug_timer_t`) enabled by the log level the host sends. Helper lines: "WGC helper: shared texture keyed mutex wait", "WGC helper: copy and publish submit (shared texture held)", "WGC helper: scratch copy or composite submit". Host lines: "WGC host: shared texture keyed mutex wait", "WGC host: copy to encoder image submit". All at debug level, every 20 seconds. "Submit" times are CPU time to queue the GPU work, not GPU time. Since 1.3.1 the mutex waits include waits that time out, and no clock is read for the timers below debug level.
- The new policy tests run in the window-only Windows CI ("Test stream network policy" step).
- None of the benefits are measured yet; they need the measurement protocol below on the target network.

### Host commit order

1. H1: helper GPU priority best effort, plus the policy table and test.
2. H2: `audio_qos` option, policy header, config, docs, locale, schema, tests.
3. H3: `pacing_mode` auto, policy header, config, docs, locale, schema, tests, session log line.
4. H4: debug timers for the window-only copy path.
5. H6: version 1.3.0 and release notes.

Each commit must pass the Linux unit harness for the new policy tests and the config catalog contract test, and the Windows CI build.

---

## Shared measurement protocol (run before and after 1.3.0)

Setup:

- Thor at 1080p120 HEVC on the usual Wi-Fi, with a fixed bitrate (for example 80 Mbps).
- The host is the window-only build, with the same app window each time.

Runs: 2 minutes each of an idle desktop, a high-motion scene and a camera pan.

Record:

- From the overlay (full mode): incoming fps, rendered fps, dropped by pacing, network drops, decode time, network latency and variance, host processing latency.
- `adb logcat` lines for the pacing mode and the decoder low-latency path.
- `dumpsys SurfaceFlinger --latency`.
- The host log at debug level for one run.

Press-to-photon: a 240 fps camera filming the Thor and a full-screen colour toggle on the host bound to a controller button. 20 presses per configuration; report medians.

Pass criteria for 1.3.0:

- Rendered fps is within 0.5 of incoming at 120 fps.
- Pacing drops are below 1%.
- Decode time is not worse with vendor keys.
- Network drops and variance are not worse with auto pacing on Wi-Fi.
- Press-to-photon median is lower than 1.2.2 on Wi-Fi.
- On Ethernet, press-to-photon is not worse by more than 2 ms.
- The helper starts without the privilege.
