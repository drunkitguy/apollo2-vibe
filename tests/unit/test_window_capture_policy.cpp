#include "../tests_common.h"
#include "src/platform/windows/window_capture_policy.h"

#include <array>
#include <cstdint>
#include <vector>

namespace {
  using platf::dxgi::window_policy::choose_target;
  using platf::dxgi::window_policy::compute_window_blit;
  using platf::dxgi::window_policy::rect_i;
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

  EXPECT_EQ(choose_target(candidates, 1, true), 2u);
}

TEST(WindowCapturePolicy, CurrentTargetIsKeptWhileStillEligible) {
  const std::array candidates {
    candidate(1, 4'000'000),
    candidate(2, 1'000),
    candidate(3, 9'000'000, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 2, true), 2u);
}

TEST(WindowCapturePolicy, LargestEligibleWindowIsTheFallback) {
  const std::array candidates {
    candidate(1, 10'000),
    candidate(2, 2'000'000),
    candidate(3, 9'000'000, false),
    candidate(4, 2'000'000),
  };

  EXPECT_EQ(choose_target(candidates, 99, true), 2u);
}

TEST(WindowCapturePolicy, WindowFromProcessStartedAfterLaunchIsEligibleWithoutAMatch) {
  const std::array candidates {
    candidate(1, 10'000, false, false, false, true),
    candidate(2, 9'000'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 0, true), 1u);
}

TEST(WindowCapturePolicy, ForegroundGameStartedOutsideTheJobIsFollowed) {
  const std::array candidates {
    candidate(1, 2'000'000),
    candidate(2, 2'073'600, false, true, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, true), 2u);
}

TEST(WindowCapturePolicy, PreexistingUnrelatedWindowsAreNeverChosenWithAMatcher) {
  const std::array candidates {
    candidate(1, 2'073'600, false, true),
    candidate(2, 500'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 0, true), 0u);
  EXPECT_EQ(choose_target(candidates, 2, true), 0u);
}

TEST(WindowCapturePolicy, WithoutAMatcherTheForegroundWindowIsFollowed) {
  const std::array candidates {
    candidate(1, 4'000'000, false),
    candidate(2, 1'000, false, true),
  };

  EXPECT_EQ(choose_target(candidates, 1, false), 2u);
}

TEST(WindowCapturePolicy, WithoutAMatcherTheCurrentTargetIsKeptWhenNothingIsForeground) {
  const std::array candidates {
    candidate(1, 4'000'000, false),
    candidate(2, 1'000, false),
  };

  EXPECT_EQ(choose_target(candidates, 2, false), 2u);
  EXPECT_EQ(choose_target(candidates, 0, false), 0u);
  EXPECT_EQ(choose_target(candidates, 7, false), 0u);
}

TEST(WindowCapturePolicy, NothingIsChosenWhenOnlyShellWindowsExist) {
  // Shell and desktop windows are filtered out before policy evaluation.
  const std::vector<window_candidate_t> none;

  EXPECT_EQ(choose_target(none, 0, false), 0u);
  EXPECT_EQ(choose_target(none, 5, false), 0u);
  EXPECT_EQ(choose_target(none, 5, true), 0u);
}

TEST(WindowCapturePolicy, MinimizedWindowsOnlyWinWhenNothingElseQualifies) {
  const std::array mixed {
    candidate(1, 4'000'000, true, true, true),
    candidate(2, 1'000),
  };
  EXPECT_EQ(choose_target(mixed, 1, true), 2u);

  const std::array only_minimized {
    candidate(1, 4'000'000, true, false, true),
  };
  EXPECT_EQ(choose_target(only_minimized, 0, true), 1u);

  const std::array follow {
    candidate(3, 1'000, false, true, true),
    candidate(4, 1'000, false),
  };
  EXPECT_EQ(choose_target(follow, 4, false), 4u);
  EXPECT_EQ(choose_target(follow, 0, false), 3u);
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
