#include "../tests_common.h"
#include "src/platform/windows/window_capture_policy.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {
  using platf::dxgi::window_policy::app_window_t;
  using platf::dxgi::window_policy::build_app_pids;
  using platf::dxgi::window_policy::choose_target;
  using platf::dxgi::window_policy::compute_layer_blit;
  using platf::dxgi::window_policy::compute_window_blit;
  using platf::dxgi::window_policy::is_secondary_window;
  using platf::dxgi::window_policy::layer_content_ready;
  using platf::dxgi::window_policy::layer_t;
  using platf::dxgi::window_policy::max_secondary_layers;
  using platf::dxgi::window_policy::placement_action_e;
  using platf::dxgi::window_policy::plan_layers;
  using platf::dxgi::window_policy::plan_placement;
  using platf::dxgi::window_policy::plan_secondary_move;
  using platf::dxgi::window_policy::rect_contains;
  using platf::dxgi::window_policy::rect_i;
  using platf::dxgi::window_policy::selection_mode_e;
  using platf::dxgi::window_policy::selection_mode_for;
  using platf::dxgi::window_policy::started_after_launch;
  using platf::dxgi::window_policy::target_debouncer_t;
  using platf::dxgi::window_policy::window_blit_t;
  using platf::dxgi::window_policy::window_candidate_t;

  constexpr rect_i primary_output {0, 0, 1920, 1080};

  void expect_blit(
    const window_blit_t &blit,
    const std::uint32_t src_left,
    const std::uint32_t src_top,
    const std::uint32_t src_right,
    const std::uint32_t src_bottom,
    const std::uint32_t dst_x,
    const std::uint32_t dst_y
  ) {
    ASSERT_FALSE(blit.empty);
    EXPECT_EQ(blit.src_left, src_left);
    EXPECT_EQ(blit.src_top, src_top);
    EXPECT_EQ(blit.src_right, src_right);
    EXPECT_EQ(blit.src_bottom, src_bottom);
    EXPECT_EQ(blit.dst_x, dst_x);
    EXPECT_EQ(blit.dst_y, dst_y);
  }

  window_candidate_t candidate(
    const std::uintptr_t id,
    const std::int64_t area,
    const bool matches_app = true,
    const bool is_foreground = false,
    const bool iconic = false,
    const bool started_after = false
  ) {
    return window_candidate_t {
      .id = id,
      .matches_app = matches_app,
      .started_after_launch = started_after,
      .is_foreground = is_foreground,
      .iconic = iconic,
      .client_area = area,
    };
  }

  window_candidate_t candidate_of(
    const std::uintptr_t id,
    const std::uint32_t pid,
    const std::int64_t area,
    const bool is_foreground = false,
    const bool matches_app = true
  ) {
    auto c = candidate(id, area, matches_app, is_foreground);
    c.pid = pid;
    return c;
  }

  app_window_t app_window(const std::uintptr_t id, const std::uint32_t pid, const rect_i bounds) {
    return app_window_t {
      .id = id,
      .pid = pid,
      .visible = true,
      .cloaked = false,
      .iconic = false,
      .root = true,
      .shadow = false,
      .transparent = false,
      .bounds = bounds,
    };
  }

  constexpr std::array<std::uint32_t, 2> app_pids {100, 200};
  constexpr std::uintptr_t target_id = 1;
  constexpr rect_i main_bounds {0, 0, 1920, 1080};

  std::vector<layer_t> plan(const std::vector<app_window_t> &windows, const std::uintptr_t target, const std::span<const std::uint32_t> pids, const std::size_t cap = max_secondary_layers) {
    std::vector<layer_t> out(cap + 1);
    out.resize(plan_layers(windows, target, pids, primary_output, out));
    return out;
  }
}  // namespace

TEST(WindowCapturePolicy, BorderlessWindowCoveringOutputIsOneFullCopy) {
  const auto blit = compute_window_blit(primary_output, primary_output, primary_output, 1920, 1080);

  expect_blit(blit, 0, 0, 1920, 1080, 0, 0);
  EXPECT_TRUE(blit.covers_output);
}

TEST(WindowCapturePolicy, BorderedWindowCopiesOnlyTheClientAreaBelowTheTitleBar) {
  constexpr rect_i frame {100, 50, 900, 650};
  constexpr rect_i client {101, 81, 899, 649};

  const auto blit = compute_window_blit(primary_output, frame, client, 800, 600);

  expect_blit(blit, 1, 31, 799, 599, 101, 81);
  EXPECT_FALSE(blit.covers_output);
}

TEST(WindowCapturePolicy, WindowPartlyOffTheLeftEdgeIsClipped) {
  constexpr rect_i frame {-200, 100, 600, 700};
  constexpr rect_i client {-200, 130, 600, 700};

  const auto blit = compute_window_blit(primary_output, frame, client, 800, 600);

  expect_blit(blit, 200, 30, 800, 600, 0, 130);
  EXPECT_FALSE(blit.covers_output);
}

TEST(WindowCapturePolicy, WindowOnAnotherDisplayIsEmpty) {
  constexpr rect_i output {1920, 0, 3840, 1080};
  constexpr rect_i frame {0, 0, 800, 600};

  const auto blit = compute_window_blit(output, frame, frame, 800, 600);

  EXPECT_TRUE(blit.empty);
  EXPECT_FALSE(blit.covers_output);
}

TEST(WindowCapturePolicy, ZeroSizeClientIsEmpty) {
  constexpr rect_i frame {100, 100, 260, 130};
  constexpr rect_i client {100, 100, 100, 100};

  EXPECT_TRUE(compute_window_blit(primary_output, frame, client, 160, 30).empty);
  EXPECT_TRUE(compute_window_blit(primary_output, frame, frame, 0, 0).empty);
}

TEST(WindowCapturePolicy, ContentSmallerThanClientDuringResizeIsClamped) {
  const auto blit = compute_window_blit(primary_output, primary_output, primary_output, 1280, 720);

  expect_blit(blit, 0, 0, 1280, 720, 0, 0);
  EXPECT_FALSE(blit.covers_output);
}

TEST(WindowCapturePolicy, ContentLargerThanClientIsLimitedToTheClientArea) {
  constexpr rect_i frame {0, 0, 1000, 800};
  constexpr rect_i client {8, 31, 992, 792};

  const auto blit = compute_window_blit(primary_output, frame, client, 1200, 900);

  expect_blit(blit, 8, 31, 992, 792, 8, 31);
}

TEST(WindowCapturePolicy, OutputAwayFromDesktopOriginUsesOutputRelativeDestination) {
  constexpr rect_i virtual_output {2560, 0, 4480, 1080};

  const auto full = compute_window_blit(virtual_output, virtual_output, virtual_output, 1920, 1080);
  expect_blit(full, 0, 0, 1920, 1080, 0, 0);
  EXPECT_TRUE(full.covers_output);

  constexpr rect_i frame {2660, 100, 3460, 700};
  constexpr rect_i client {2661, 131, 3459, 699};
  const auto bordered = compute_window_blit(virtual_output, frame, client, 800, 600);
  expect_blit(bordered, 1, 31, 799, 599, 101, 131);
  EXPECT_FALSE(bordered.covers_output);
}

TEST(WindowCapturePolicy, OutputLeftOfDesktopOriginHandlesNegativeCoordinates) {
  constexpr rect_i left_output {-1920, 0, 0, 1080};
  constexpr rect_i frame {-1000, 200, 200, 900};
  constexpr rect_i client {-999, 231, 199, 899};

  const auto blit = compute_window_blit(left_output, frame, client, 1200, 700);

  expect_blit(blit, 1, 31, 1000, 699, 921, 231);
}

TEST(WindowCapturePolicy, StartedAfterLaunchNeedsARecordedLaunchAndALaterProcess) {
  EXPECT_FALSE(started_after_launch(500, 0));
  EXPECT_FALSE(started_after_launch(100, 200));
  EXPECT_FALSE(started_after_launch(200, 200));
  EXPECT_TRUE(started_after_launch(201, 200));
}

TEST(WindowCapturePolicy, EligibleForegroundWindowWins) {
  const std::array candidates {
    candidate(1, 4'000'000),
    candidate(2, 1'000, true, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, CurrentTargetIsKeptWhileStillEligible) {
  const std::array candidates {
    candidate(1, 4'000'000),
    candidate(2, 1'000),
    candidate(3, 9'000'000, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 2, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, LargestEligibleWindowIsTheFallback) {
  const std::array candidates {
    candidate(1, 10'000),
    candidate(2, 2'000'000),
    candidate(3, 9'000'000, false),
    candidate(4, 2'000'000),
  };

  EXPECT_EQ(choose_target(candidates, 99, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, WindowFromProcessStartedAfterLaunchIsEligibleWithoutAMatch) {
  const std::array candidates {
    candidate(1, 10'000, false, false, false, true),
    candidate(2, 9'000'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::app_matcher), 1u);
}

TEST(WindowCapturePolicy, ForegroundGameStartedOutsideTheJobIsFollowed) {
  const std::array candidates {
    candidate(1, 2'000'000),
    candidate(2, 2'073'600, false, true, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, PreexistingUnrelatedWindowsAreNeverChosenWithAMatcher) {
  const std::array candidates {
    candidate(1, 2'073'600, false, true),
    candidate(2, 500'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::app_matcher), 0u);
  EXPECT_EQ(choose_target(candidates, 2, selection_mode_e::app_matcher), 0u);
}

TEST(WindowCapturePolicy, DesktopModeFollowsTheForegroundWindow) {
  const std::array candidates {
    candidate(1, 4'000'000, false),
    candidate(2, 1'000, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::desktop), 2u);
}

TEST(WindowCapturePolicy, DesktopModeKeepsTheCurrentTargetWhenNothingIsForeground) {
  const std::array candidates {
    candidate(1, 4'000'000, false),
    candidate(2, 1'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 2, selection_mode_e::desktop), 2u);
  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::desktop), 0u);
  EXPECT_EQ(choose_target(candidates, 7, selection_mode_e::desktop), 0u);
}

TEST(WindowCapturePolicy, LaunchedAppModeOnlyAcceptsWindowsStartedAfterTheLaunch) {
  const std::array candidates {
    candidate(1, 2'073'600, false, true),
    candidate(2, 500'000, false),
    candidate(3, 1'000'000, false, false, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::launched_app), 3u);
  EXPECT_EQ(choose_target(candidates, 2, selection_mode_e::launched_app), 3u);
}

TEST(WindowCapturePolicy, LaunchedAppModeAcceptsANewWindowOfAnOldProcess) {
  // steam://open/bigpicture: the running Steam client opens the Big Picture window.
  auto big_picture = candidate(2, 2'073'600, false, true);
  big_picture.new_since_launch = true;
  const std::array candidates {
    candidate(1, 4'000'000, false),
    big_picture,
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::launched_app), 2u);
}

TEST(WindowCapturePolicy, LaunchedAppModeRejectsAnOldWindowOfAnOldProcess) {
  const std::array candidates {
    candidate(1, 4'000'000, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::launched_app), 0u);
  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::launched_app), 0u);
}

TEST(WindowCapturePolicy, AppMatcherModeIgnoresNewWindowsOfUnrelatedOldProcesses) {
  auto chat_popup = candidate(2, 500'000, false, true);
  chat_popup.new_since_launch = true;
  const std::array candidates {
    candidate(1, 2'073'600),
    chat_popup,
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 1u);
}

TEST(WindowCapturePolicy, LaunchedAppModeNeverFollowsPreexistingForegroundWindows) {
  const std::array candidates {
    candidate(1, 2'073'600, false, true),
    candidate(2, 500'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::launched_app), 0u);
  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::launched_app), 0u);
}

TEST(WindowCapturePolicy, LaunchedAppModeFollowsANewForegroundWindow) {
  const std::array candidates {
    candidate(1, 2'073'600, false, false, false, true),
    candidate(2, 500'000, false, true, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::launched_app), 2u);
}

TEST(WindowCapturePolicy, DesktopModeFollowsAnyForegroundCandidate) {
  const std::array candidates {
    candidate(1, 2'073'600, false, false, false, true),
    candidate(2, 500'000, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::desktop), 2u);
}

TEST(WindowCapturePolicy, OnlyTheCommandLessDesktopAppFollowsTheForeground) {
  EXPECT_EQ(selection_mode_for(false, false, false), selection_mode_e::desktop);
  EXPECT_EQ(selection_mode_for(true, false, false), selection_mode_e::desktop);
  EXPECT_EQ(selection_mode_for(true, true, true), selection_mode_e::app_matcher);
  EXPECT_EQ(selection_mode_for(true, true, false), selection_mode_e::launched_app);
}

TEST(WindowCapturePolicy, AnAppWithACommandNeverFollowsAPreexistingForegroundWindow) {
  // A browser that was open before the launch is in the foreground; the launched app has no window yet.
  const std::array candidates {
    candidate(1, 2'073'600, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 0, selection_mode_for(true, true, false)), 0u);
  EXPECT_EQ(choose_target(candidates, 0, selection_mode_for(true, true, true)), 0u);
  EXPECT_EQ(choose_target(candidates, 0, selection_mode_for(true, false, false)), 1u);
}

TEST(WindowCapturePolicy, NothingIsChosenWhenOnlyShellWindowsExist) {
  // Shell and desktop windows are filtered out before policy evaluation.
  const std::vector<window_candidate_t> none;

  EXPECT_EQ(choose_target(none, 0, selection_mode_e::desktop), 0u);
  EXPECT_EQ(choose_target(none, 5, selection_mode_e::desktop), 0u);
  EXPECT_EQ(choose_target(none, 5, selection_mode_e::app_matcher), 0u);
}

TEST(WindowCapturePolicy, MinimizedWindowsOnlyWinWhenNothingElseQualifies) {
  const std::array mixed {
    candidate(1, 4'000'000, true, true, true),
    candidate(2, 1'000),
  };
  EXPECT_EQ(choose_target(mixed, 1, selection_mode_e::app_matcher), 2u);

  const std::array only_minimized {
    candidate(1, 4'000'000, true, false, true),
  };
  EXPECT_EQ(choose_target(only_minimized, 0, selection_mode_e::app_matcher), 1u);

  const std::array follow {
    candidate(3, 1'000, false, true, true),
    candidate(4, 1'000, false),
  };
  EXPECT_EQ(choose_target(follow, 4, selection_mode_e::desktop), 4u);
  EXPECT_EQ(choose_target(follow, 0, selection_mode_e::desktop), 3u);
}

TEST(WindowCapturePolicy, DebouncerNeedsThreeEqualPolls) {
  target_debouncer_t debouncer;

  EXPECT_FALSE(debouncer.observe(10, 3));
  EXPECT_FALSE(debouncer.observe(10, 3));
  EXPECT_TRUE(debouncer.observe(10, 3));
  EXPECT_EQ(debouncer.committed, 10u);
  EXPECT_FALSE(debouncer.observe(10, 3));
  EXPECT_EQ(debouncer.committed, 10u);
}

TEST(WindowCapturePolicy, DebouncerIgnoresFlapping) {
  target_debouncer_t debouncer {.pending = 10, .stable_polls = 3, .committed = 10};

  for (int i = 0; i < 10; ++i) {
    EXPECT_FALSE(debouncer.observe(i % 2 == 0 ? 20 : 10, 3));
  }
  EXPECT_EQ(debouncer.committed, 10u);

  EXPECT_FALSE(debouncer.observe(20, 3));
  EXPECT_FALSE(debouncer.observe(20, 3));
  EXPECT_TRUE(debouncer.observe(20, 3));
  EXPECT_EQ(debouncer.committed, 20u);
}

TEST(WindowCapturePolicy, DebouncerCommitsLossOfTarget) {
  target_debouncer_t debouncer {.pending = 10, .stable_polls = 3, .committed = 10};

  EXPECT_FALSE(debouncer.observe(0, 3));
  EXPECT_FALSE(debouncer.observe(0, 3));
  EXPECT_TRUE(debouncer.observe(0, 3));
  EXPECT_EQ(debouncer.committed, 0u);
}

TEST(WindowCapturePolicy, PlacementLeavesAWindowCoveringTheOutputAlone) {
  const auto placement = plan_placement(primary_output, primary_output, primary_output, true, false, false);

  EXPECT_EQ(placement.action, placement_action_e::none);
}

TEST(WindowCapturePolicy, PlacementRestoresMinimizedAndMaximizedWindowsFirst) {
  constexpr rect_i window {-32000, -32000, -31840, -31972};
  EXPECT_EQ(plan_placement(primary_output, window, {0, 0, 0, 0}, true, true, false).action, placement_action_e::restore);

  constexpr rect_i maximized {-8, -8, 1928, 1040};
  constexpr rect_i maximized_client {0, 23, 1920, 1032};
  EXPECT_EQ(plan_placement(primary_output, maximized, maximized_client, true, false, true).action, placement_action_e::restore);
}

TEST(WindowCapturePolicy, PlacementSizesAResizableWindowSoItsClientAreaFillsTheOutput) {
  constexpr rect_i output {1920, 0, 3840, 1080};
  constexpr rect_i window {100, 100, 916, 739};
  constexpr rect_i client {108, 131, 908, 731};

  const auto placement = plan_placement(output, window, client, true, false, false);

  ASSERT_EQ(placement.action, placement_action_e::set_rect);
  EXPECT_EQ(placement.window.left, 1912);
  EXPECT_EQ(placement.window.top, -31);
  EXPECT_EQ(placement.window.right, 3848);
  EXPECT_EQ(placement.window.bottom, 1088);
}

TEST(WindowCapturePolicy, PlacementMovesAFixedWindowClientAreaFirstOntoTheOutput) {
  constexpr rect_i output {1920, 0, 3840, 1080};
  constexpr rect_i window {100, 100, 916, 739};
  constexpr rect_i client {108, 131, 908, 731};

  const auto placement = plan_placement(output, window, client, false, false, false);

  ASSERT_EQ(placement.action, placement_action_e::set_rect);
  EXPECT_EQ(placement.window.left, 1912);
  EXPECT_EQ(placement.window.top, -31);
  EXPECT_EQ(placement.window.right, 2728);
  EXPECT_EQ(placement.window.bottom, 608);
}

TEST(WindowCapturePolicy, PlacementLeavesAFixedWindowAlreadyOnTheOutput) {
  constexpr rect_i window {100, 100, 916, 739};
  constexpr rect_i client {108, 131, 908, 731};

  EXPECT_EQ(plan_placement(primary_output, window, client, false, false, false).action, placement_action_e::none);
}

TEST(WindowCapturePolicy, SmallerForegroundWindowOfTheTargetProcessDoesNotReplaceTheTarget) {
  // An unowned dialog or message box of the app is drawn as a secondary window instead.
  const std::array candidates {
    candidate_of(1, 100, 2'073'600),
    candidate_of(2, 100, 200'000, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 1u);
  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::desktop), 1u);
}

TEST(WindowCapturePolicy, LargerForegroundWindowOfTheTargetProcessStillWins) {
  const std::array candidates {
    candidate_of(1, 100, 200'000),
    candidate_of(2, 100, 2'073'600, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, ForegroundWindowOfAnotherEligibleProcessStillWins) {
  // Launcher to game hand-off: the game window is smaller but belongs to another process.
  const std::array candidates {
    candidate_of(1, 100, 2'073'600),
    candidate_of(2, 300, 921'600, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, CandidatesWithoutAProcessIdKeepTheOldForegroundRule) {
  const std::array candidates {
    candidate_of(1, 0, 2'073'600),
    candidate_of(2, 0, 200'000, true),
  };
  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 2u);

  const std::array mixed {
    candidate_of(1, 100, 2'073'600),
    candidate_of(2, 0, 200'000, true),
  };
  EXPECT_EQ(choose_target(mixed, 1, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, SameProcessRuleNeedsAnEligibleCurrentTarget) {
  const std::array candidates {
    candidate_of(1, 100, 2'073'600, false, false),
    candidate_of(2, 100, 200'000, true),
  };

  // The current target is not eligible (it no longer matches the app), so the dialog wins.
  EXPECT_EQ(choose_target(candidates, 1, selection_mode_e::app_matcher), 2u);
  // Without a current target the foreground window wins as before.
  EXPECT_EQ(choose_target(candidates, 0, selection_mode_e::app_matcher), 2u);
}

TEST(WindowCapturePolicy, LayerBlitCopiesAPopupInsideTheOutputWhole) {
  constexpr rect_i popup {300, 120, 520, 400};

  const auto blit = compute_layer_blit(primary_output, popup, 220, 280);

  expect_blit(blit, 0, 0, 220, 280, 300, 120);
  EXPECT_FALSE(blit.covers_output);
}

TEST(WindowCapturePolicy, LayerBlitClipsAPopupStraddlingTheRightEdge) {
  constexpr rect_i popup {1800, 500, 2000, 700};

  const auto blit = compute_layer_blit(primary_output, popup, 200, 200);

  expect_blit(blit, 0, 0, 120, 200, 1800, 500);
}

TEST(WindowCapturePolicy, LayerBlitClipsADialogLargerThanTheOutput) {
  constexpr rect_i dialog {-10, -20, 2000, 1200};

  const auto blit = compute_layer_blit(primary_output, dialog, 2010, 1220);

  expect_blit(blit, 10, 20, 1930, 1100, 0, 0);
  EXPECT_TRUE(blit.covers_output);
}

TEST(WindowCapturePolicy, LayerBlitOfAWindowOnAnotherDisplayIsEmpty) {
  constexpr rect_i popup {2000, 100, 2200, 300};

  EXPECT_TRUE(compute_layer_blit(primary_output, popup, 200, 200).empty);
}

TEST(WindowCapturePolicy, LayerBlitUsesOnlyTheValidContentDuringAResize) {
  constexpr rect_i dialog {100, 100, 900, 700};

  expect_blit(compute_layer_blit(primary_output, dialog, 640, 480), 0, 0, 640, 480, 100, 100);
  // Content larger than the bounds is limited to the bounds.
  expect_blit(compute_layer_blit(primary_output, dialog, 1000, 1000), 0, 0, 800, 600, 100, 100);
}

TEST(WindowCapturePolicy, LayerBlitHandlesOutputsAwayFromTheOriginAndNegativeCoordinates) {
  constexpr rect_i virtual_output {2560, 0, 4480, 1080};
  expect_blit(compute_layer_blit(virtual_output, {2600, 40, 2800, 240}, 200, 200), 0, 0, 200, 200, 40, 40);

  constexpr rect_i left_output {-1920, 0, 0, 1080};
  expect_blit(compute_layer_blit(left_output, {-100, 50, 100, 250}, 200, 200), 0, 0, 100, 200, 1820, 50);
}

TEST(WindowCapturePolicy, AppPopupIsPlannedAboveTheTarget) {
  // Qt menus are unowned topmost popups of the app's process; they come first in z-order.
  const std::vector windows {
    app_window(5, 100, {200, 30, 420, 400}),
    app_window(target_id, 100, main_bounds),
  };

  const auto layers = plan(windows, target_id, app_pids);

  ASSERT_EQ(layers.size(), 2u);
  EXPECT_EQ(layers[0].id, target_id);
  EXPECT_TRUE(layers[0].is_target);
  EXPECT_EQ(layers[1].id, 5u);
  EXPECT_FALSE(layers[1].is_target);
  EXPECT_FALSE(layers[1].below_target);
}

TEST(WindowCapturePolicy, PopupOfAForeignProcessIsNeverPlanned) {
  const std::vector windows {
    app_window(5, 999, {200, 30, 420, 400}),
    app_window(target_id, 100, main_bounds),
  };

  const auto layers = plan(windows, target_id, app_pids);

  ASSERT_EQ(layers.size(), 1u);
  EXPECT_TRUE(layers[0].is_target);
  EXPECT_FALSE(is_secondary_window(windows[0], target_id, app_pids, primary_output));
}

TEST(WindowCapturePolicy, HiddenCloakedIconicChildShadowAndTransparentWindowsAreSkipped) {
  constexpr rect_i bounds {200, 30, 420, 400};
  auto hidden = app_window(10, 100, bounds);
  hidden.visible = false;
  auto cloaked = app_window(11, 100, bounds);
  cloaked.cloaked = true;
  auto iconic = app_window(12, 100, bounds);
  iconic.iconic = true;
  auto child = app_window(13, 100, bounds);
  child.root = false;
  auto shadow = app_window(14, 100, bounds);
  shadow.shadow = true;
  auto transparent = app_window(15, 100, bounds);
  transparent.transparent = true;
  auto empty = app_window(16, 100, {200, 30, 200, 400});
  auto overlay = app_window(18, 100, main_bounds);
  overlay.click_through = true;
  const auto shown = app_window(17, 200, bounds);

  for (const auto &w : {hidden, cloaked, iconic, child, shadow, transparent, empty, overlay}) {
    EXPECT_FALSE(is_secondary_window(w, target_id, app_pids, primary_output)) << w.id;
  }
  EXPECT_TRUE(is_secondary_window(shown, target_id, app_pids, primary_output));

  const std::vector windows {overlay, hidden, cloaked, iconic, child, shadow, transparent, empty, shown, app_window(target_id, 100, main_bounds)};
  const auto layers = plan(windows, target_id, app_pids);
  ASSERT_EQ(layers.size(), 2u);
  EXPECT_EQ(layers[1].id, 17u);
}

TEST(WindowCapturePolicy, WindowOutsideTheOutputIsSkipped) {
  const auto elsewhere = app_window(5, 100, {1920, 0, 2200, 300});
  const auto touching = app_window(6, 100, {-300, 0, 0, 300});
  const auto partly = app_window(7, 100, {1800, 0, 2200, 300});

  EXPECT_FALSE(is_secondary_window(elsewhere, target_id, app_pids, primary_output));
  EXPECT_FALSE(is_secondary_window(touching, target_id, app_pids, primary_output));
  EXPECT_TRUE(is_secondary_window(partly, target_id, app_pids, primary_output));
}

TEST(WindowCapturePolicy, LayersKeepTheZOrderBottomUpAndMarkWindowsBelowTheTarget) {
  const std::vector windows {
    app_window(7, 100, {500, 500, 700, 700}),  // tooltip, topmost
    app_window(6, 200, {400, 400, 900, 900}),  // dropdown of a helper process
    app_window(target_id, 100, main_bounds),
    app_window(4, 100, {100, 100, 600, 600}),  // tool palette below the main window
  };

  const auto layers = plan(windows, target_id, app_pids);

  ASSERT_EQ(layers.size(), 4u);
  EXPECT_EQ(layers[0].id, 4u);
  EXPECT_TRUE(layers[0].below_target);
  EXPECT_EQ(layers[1].id, target_id);
  EXPECT_TRUE(layers[1].is_target);
  EXPECT_EQ(layers[2].id, 6u);
  EXPECT_FALSE(layers[2].below_target);
  EXPECT_EQ(layers[3].id, 7u);
  EXPECT_FALSE(layers[3].below_target);
}

TEST(WindowCapturePolicy, MoreSecondaryWindowsThanTheCapKeepTheTopmost) {
  std::vector<app_window_t> windows;
  for (std::uintptr_t id = 100; id < 100 + max_secondary_layers + 4; ++id) {
    windows.push_back(app_window(id, 100, {10, 10, 50, 50}));
  }
  windows.push_back(app_window(target_id, 100, main_bounds));

  const auto layers = plan(windows, target_id, app_pids);

  ASSERT_EQ(layers.size(), max_secondary_layers + 1);
  EXPECT_TRUE(layers.front().is_target);
  // Bottom-up: the last layer is the topmost window, the first secondary the 16th from the top.
  EXPECT_EQ(layers.back().id, 100u);
  EXPECT_EQ(layers[1].id, 100u + max_secondary_layers - 1);
}

TEST(WindowCapturePolicy, MinimizedTargetMissingFromTheListStillYieldsSecondaryWindows) {
  const std::vector windows {
    app_window(5, 100, {200, 30, 420, 400}),
    app_window(6, 200, {600, 30, 820, 400}),
  };

  const auto layers = plan(windows, target_id, app_pids);

  ASSERT_EQ(layers.size(), 2u);
  EXPECT_EQ(layers[0].id, 6u);
  EXPECT_EQ(layers[1].id, 5u);
  EXPECT_FALSE(layers[0].below_target);
  EXPECT_FALSE(layers[1].below_target);
}

TEST(WindowCapturePolicy, EmptyAppProcessSetPlansOnlyTheTarget) {
  const std::vector windows {
    app_window(5, 100, {200, 30, 420, 400}),
    app_window(target_id, 100, main_bounds),
  };

  const auto layers = plan(windows, target_id, std::span<const std::uint32_t> {});

  ASSERT_EQ(layers.size(), 1u);
  EXPECT_TRUE(layers[0].is_target);
}

TEST(WindowCapturePolicy, LayerPlanWithoutRoomWritesNothing) {
  const std::vector windows {app_window(target_id, 100, main_bounds)};
  std::span<layer_t> none;

  EXPECT_EQ(plan_layers(windows, target_id, app_pids, primary_output, none), 0u);
}

TEST(WindowCapturePolicy, AppProcessSetKeepsTheTargetFirstAndIsSorted) {
  const std::vector<std::uint32_t> seeds {300, 50, 300, 4, 0, 70, 50};
  const std::vector<std::uint32_t> excluded {4, 70};

  EXPECT_EQ(build_app_pids(200, seeds, excluded, 62), (std::vector<std::uint32_t> {50, 200, 300}));
  // The target survives the cap; seeds fill the rest in order.
  EXPECT_EQ(build_app_pids(200, seeds, excluded, 2), (std::vector<std::uint32_t> {200, 300}));
  EXPECT_EQ(build_app_pids(200, seeds, excluded, 1), (std::vector<std::uint32_t> {200}));
  // An excluded target is dropped too, and nothing is left without a target or seeds.
  EXPECT_EQ(build_app_pids(70, {}, excluded, 62), (std::vector<std::uint32_t> {}));
  EXPECT_EQ(build_app_pids(0, {}, {}, 62), (std::vector<std::uint32_t> {}));
}

TEST(WindowCapturePolicy, DialogInsideTheOutputIsNotMoved) {
  EXPECT_FALSE(plan_secondary_move(primary_output, {400, 300, 1000, 800}).has_value());
}

TEST(WindowCapturePolicy, DialogOnAnotherMonitorIsCentredOnTheOutput) {
  constexpr rect_i output {1920, 0, 3840, 1080};

  const auto moved = plan_secondary_move(output, {400, 300, 1000, 800});

  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(moved->left, 2580);
  EXPECT_EQ(moved->top, 290);
  EXPECT_EQ(moved->right, 3180);
  EXPECT_EQ(moved->bottom, 790);
}

TEST(WindowCapturePolicy, DialogLargerThanTheOutputIsAlignedToItsTopLeft) {
  constexpr rect_i output {1920, 0, 3840, 1080};

  const auto moved = plan_secondary_move(output, {0, 0, 2400, 1300});

  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(moved->left, 1920);
  EXPECT_EQ(moved->top, 0);
  EXPECT_EQ(moved->right, 4320);
  EXPECT_EQ(moved->bottom, 1300);

  // Larger on one axis only: aligned on that axis, centred on the other.
  const auto wide = plan_secondary_move(output, {0, 0, 2400, 600});
  ASSERT_TRUE(wide.has_value());
  EXPECT_EQ(wide->left, 1920);
  EXPECT_EQ(wide->top, 240);
}

TEST(WindowCapturePolicy, DialogHalfInsideTheOutputIsLeftAlone) {
  // Exactly half inside stays; less than half inside moves.
  EXPECT_FALSE(plan_secondary_move(primary_output, {1720, 100, 2120, 300}).has_value());
  EXPECT_TRUE(plan_secondary_move(primary_output, {1721, 100, 2121, 300}).has_value());
  EXPECT_FALSE(plan_secondary_move(primary_output, {100, 100, 100, 300}).has_value());
}

TEST(WindowCapturePolicy, LayerIsDrawnOnlyWithContentNewerThanItsShowOrAfterTheReuseDelay) {
  // No frame yet: never drawn.
  EXPECT_FALSE(layer_content_ready(0, 100, 1'000, 100));
  // A frame at or after the show is drawn at once.
  EXPECT_TRUE(layer_content_ready(150, 100, 150, 100));
  EXPECT_TRUE(layer_content_ready(100, 100, 100, 100));
  // A reused window with an older frame waits for the reuse delay.
  EXPECT_FALSE(layer_content_ready(50, 100, 150, 100));
  EXPECT_TRUE(layer_content_ready(50, 100, 200, 100));
}

TEST(WindowCapturePolicy, WindowInsideTheTargetClientAreaIsContained) {
  constexpr rect_i client {0, 0, 1920, 1080};

  EXPECT_TRUE(rect_contains(client, {100, 100, 600, 600}));
  EXPECT_TRUE(rect_contains(client, client));
  EXPECT_FALSE(rect_contains(client, {1800, 100, 2000, 600}));
  EXPECT_FALSE(rect_contains(client, {-1, 0, 100, 100}));
  // An empty target area contains nothing.
  EXPECT_FALSE(rect_contains({0, 0, 0, 0}, {0, 0, 0, 0}));
}
