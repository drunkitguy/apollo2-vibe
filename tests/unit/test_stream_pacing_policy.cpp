/**
 * @file tests/unit/test_stream_pacing_policy.cpp
 * @brief Tests for the video send pacing rate policy.
 */
#include "../tests_common.h"
#include "src/stream_pacing_policy.h"

#include <cstdint>

namespace {
  using stream_pacing::pacing_decision_t;
  using stream_pacing::pacing_mode_e;
  using stream_pacing::pacing_mode_from_view;
  using stream_pacing::pacing_source_e;
  using stream_pacing::resolve_pacing_bps;

  constexpr std::uint64_t mbps = 1'000'000;
  constexpr int default_multiplier = stream_pacing::default_auto_multiplier_percent;

  /// The 1.2.2 code in stream.cpp, kept here to prove legacy and explicit caps are unchanged.
  std::uint64_t pacing_bps_122(const int configured_kbps, const int session_bitrate_kbps) {
    if (configured_kbps > 0) {
      std::uint64_t pacing_bps = static_cast<std::uint64_t>(configured_kbps) * 1000ull;
      const std::uint64_t session_floor_bps = static_cast<std::uint64_t>(session_bitrate_kbps) * 1000ull * 110 / 100;
      if (pacing_bps < session_floor_bps) {
        pacing_bps = session_floor_bps;
      }
      return pacing_bps;
    }
    return 1'000'000'000ull * 80 / 100;
  }
}  // namespace

TEST(StreamPacingPolicy, ParsesModes) {
  EXPECT_EQ(pacing_mode_from_view("auto"), pacing_mode_e::auto_rate);
  EXPECT_EQ(pacing_mode_from_view("legacy"), pacing_mode_e::legacy);
  EXPECT_FALSE(pacing_mode_from_view("").has_value());
  EXPECT_FALSE(pacing_mode_from_view("Auto").has_value());
  EXPECT_EQ(stream_pacing::default_pacing_mode, pacing_mode_e::auto_rate);
  EXPECT_EQ(stream_pacing::to_string(pacing_mode_e::auto_rate), "auto");
  EXPECT_EQ(stream_pacing::to_string(pacing_mode_e::legacy), "legacy");
}

TEST(StreamPacingPolicy, LegacyIs800Mbps) {
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::legacy, 100'000, default_multiplier), (pacing_decision_t {800 * mbps, pacing_source_e::legacy, false}));
}

TEST(StreamPacingPolicy, LegacyAndExplicitCapsMatch122ForEveryBitrate) {
  for (int session_kbps = 0; session_kbps <= 1'500'000; session_kbps += 2'500) {
    EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::legacy, session_kbps, default_multiplier).bps, pacing_bps_122(0, session_kbps)) << session_kbps;
    for (const int configured_kbps : {1, 50'000, 130'000, 200'000, 1'000'000, 10'000'000}) {
      for (const auto mode : {pacing_mode_e::legacy, pacing_mode_e::auto_rate}) {
        EXPECT_EQ(resolve_pacing_bps(configured_kbps, mode, session_kbps, default_multiplier).bps, pacing_bps_122(configured_kbps, session_kbps))
          << configured_kbps << " kbps at " << session_kbps;
      }
    }
  }
}

TEST(StreamPacingPolicy, ExplicitCapWinsOverAuto) {
  // max(200, 150 * 1.1 = 165) = 200 Mbps.
  EXPECT_EQ(resolve_pacing_bps(200'000, pacing_mode_e::auto_rate, 150'000, default_multiplier), (pacing_decision_t {200 * mbps, pacing_source_e::configured, false}));
}

TEST(StreamPacingPolicy, ExplicitCapBelowTheStreamIsRaisedTo110Percent) {
  EXPECT_EQ(resolve_pacing_bps(50'000, pacing_mode_e::auto_rate, 100'000, default_multiplier), (pacing_decision_t {110 * mbps, pacing_source_e::configured, true}));
}

TEST(StreamPacingPolicy, AutoIsFourTimesTheStreamWithin300To800Mbps) {
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 100'000, default_multiplier), (pacing_decision_t {400 * mbps, pacing_source_e::auto_rate, false}));
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 150'000, default_multiplier).bps, 600 * mbps);
  // Floor.
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 20'000, default_multiplier).bps, 300 * mbps);
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 50'000, default_multiplier).bps, 300 * mbps);
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 0, default_multiplier).bps, 300 * mbps);
  // Ceiling (the legacy rate).
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 300'000, default_multiplier).bps, 800 * mbps);
}

TEST(StreamPacingPolicy, AutoNeverPacesBelow110PercentOfTheStream) {
  for (int session_kbps = 0; session_kbps <= 1'500'000; session_kbps += 1'000) {
    const auto decision = resolve_pacing_bps(0, pacing_mode_e::auto_rate, session_kbps, default_multiplier);
    EXPECT_GE(decision.bps, stream_pacing::session_floor_bps(session_kbps)) << session_kbps;
    EXPECT_GE(decision.bps, 300 * mbps) << session_kbps;
    if (stream_pacing::session_floor_bps(session_kbps) <= 800 * mbps) {
      EXPECT_LE(decision.bps, 800 * mbps) << session_kbps;
    }
  }
  // Above ~727 Mbps the 110% rule wins over the ceiling.
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 1'000'000, default_multiplier).bps, 1'100 * mbps);
}

TEST(StreamPacingPolicy, MultiplierIsClampedToItsRange) {
  EXPECT_EQ(stream_pacing::clamp_auto_multiplier_percent(100), 150);
  EXPECT_EQ(stream_pacing::clamp_auto_multiplier_percent(5000), 1000);
  EXPECT_EQ(stream_pacing::clamp_auto_multiplier_percent(250), 250);
  // 150 Mbps at 100% would be 150 Mbps; the multiplier is raised to 150% (225), then the floor wins.
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 150'000, 100).bps, 300 * mbps);
  // 250 Mbps at 150% is 375 Mbps.
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 250'000, 100).bps, 375 * mbps);
  // 50 Mbps at 5000% is clamped to 1000% (500 Mbps).
  EXPECT_EQ(resolve_pacing_bps(0, pacing_mode_e::auto_rate, 50'000, 5000).bps, 500 * mbps);
}
