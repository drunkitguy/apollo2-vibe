/**
 * @file tests/unit/test_fec_adaptive.cpp
 * @brief Test the adaptive FEC control law in src/fec_adaptive.*.
 *
 * The law is asymmetric the opposite way round from a congestion controller: the dangerous
 * direction is *down*, because less FEC means a lost packet costs a whole frame. So loss must
 * restore protection instantly, and reduction must be slow and floored.
 */
#include "../tests_common.h"

#include <src/fec_adaptive.h>

using fec_adaptive::CLEAN_REPORTS_PER_STEP;
using fec_adaptive::next_percentage;
using fec_adaptive::STEP_DOWN_PCT;

namespace {
  constexpr int CEIL = 20;
  constexpr int FLOOR = 10;
}

TEST(FecAdaptiveTests, AnyLossRestoresTheCeilingImmediately) {
  int clean = 0;
  // Even a single lost packet, and even from the floor, goes straight back to full protection.
  EXPECT_EQ(CEIL, next_percentage(FLOOR, 1, clean, CEIL, FLOOR));
  EXPECT_EQ(0, clean) << "a loss must reset the clean streak";
}

TEST(FecAdaptiveTests, LossResetsAnAlmostCompleteCleanStreak) {
  int clean = CLEAN_REPORTS_PER_STEP - 1;
  EXPECT_EQ(CEIL, next_percentage(CEIL, 3, clean, CEIL, FLOOR));
  EXPECT_EQ(0, clean);
}

TEST(FecAdaptiveTests, CleanReportsBelowTheHoldChangeNothing) {
  int clean = 0;
  int pct = CEIL;
  for (int i = 0; i < CLEAN_REPORTS_PER_STEP - 1; i++) {
    pct = next_percentage(pct, 0, clean, CEIL, FLOOR);
    EXPECT_EQ(CEIL, pct) << "stepped down after only " << (i + 1) << " clean reports";
  }
  EXPECT_EQ(CLEAN_REPORTS_PER_STEP - 1, clean);
}

TEST(FecAdaptiveTests, StepsDownOnceTheHoldIsMet) {
  int clean = 0;
  int pct = CEIL;
  for (int i = 0; i < CLEAN_REPORTS_PER_STEP; i++) {
    pct = next_percentage(pct, 0, clean, CEIL, FLOOR);
  }
  EXPECT_EQ(CEIL - STEP_DOWN_PCT, pct);
  EXPECT_EQ(0, clean) << "the streak must restart after a step";
}

TEST(FecAdaptiveTests, NeverGoesBelowTheFloor) {
  int clean = 0;
  int pct = CEIL;
  // Far more clean reports than are needed to reach the floor.
  for (int i = 0; i < CLEAN_REPORTS_PER_STEP * 50; i++) {
    pct = next_percentage(pct, 0, clean, CEIL, FLOOR);
    ASSERT_GE(pct, FLOOR) << "crossed the floor on iteration " << i;
  }
  EXPECT_EQ(FLOOR, pct);
}

TEST(FecAdaptiveTests, RecoversFromTheFloorInOneReport) {
  int clean = 0;
  int pct = CEIL;
  for (int i = 0; i < CLEAN_REPORTS_PER_STEP * 50; i++) {
    pct = next_percentage(pct, 0, clean, CEIL, FLOOR);
  }
  ASSERT_EQ(FLOOR, pct);

  // The whole point of the asymmetry: getting down took dozens of reports, getting back takes one.
  EXPECT_EQ(CEIL, next_percentage(pct, 1, clean, CEIL, FLOOR));
}

TEST(FecAdaptiveTests, DegenerateEnvelopeIsStable) {
  // floor == ceiling: the controller must be inert rather than oscillate.
  int clean = 0;
  int pct = CEIL;
  for (int i = 0; i < CLEAN_REPORTS_PER_STEP * 3; i++) {
    pct = next_percentage(pct, 0, clean, CEIL, CEIL);
    ASSERT_EQ(CEIL, pct);
  }
  EXPECT_EQ(CEIL, next_percentage(pct, 5, clean, CEIL, CEIL));
}

TEST(FecAdaptiveTests, StepDownIsMonotonicAndBounded) {
  // Walk the whole envelope and assert it only ever moves one step at a time downward.
  int clean = 0;
  int pct = CEIL;
  int previous = pct;
  for (int i = 0; i < CLEAN_REPORTS_PER_STEP * 20; i++) {
    pct = next_percentage(pct, 0, clean, CEIL, FLOOR);
    ASSERT_LE(pct, previous) << "adaptation increased without any loss being reported";
    ASSERT_GE(previous - pct, 0);
    ASSERT_LE(previous - pct, STEP_DOWN_PCT) << "took a larger step than the clamp allows";
    previous = pct;
  }
}
