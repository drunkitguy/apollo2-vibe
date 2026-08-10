/**
 * @file tests/unit/test_input_batching.cpp
 * @brief Test the input message batching rules in src/input.cpp.
 *
 * These lock down the overflow guard. It was inverted: `__builtin_add_overflow()` returns true
 * when the addition DID overflow, but the guard read `if (!__builtin_add_overflow(...)) return
 * terminate_batch;`, so ordinary small deltas refused to merge and only a genuinely overflowing
 * pair proceeded — using the wrapped sum the builtin had written. Every "MergesSmall" case below
 * fails against the old code, and every "RefusesToMerge" case asserts the wrapped value is not
 * silently used.
 */
#include "../tests_common.h"

#include <climits>

#include <src/input_batching_policy.h>
#include <src/utility.h>

namespace {

  NV_REL_MOUSE_MOVE_PACKET rel_mouse(short dx, short dy) {
    NV_REL_MOUSE_MOVE_PACKET p {};
    p.header.magic = util::endian::little(static_cast<int>(MOUSE_MOVE_REL_MAGIC_GEN5));
    p.deltaX = util::endian::big(dx);
    p.deltaY = util::endian::big(dy);
    return p;
  }

  NV_SCROLL_PACKET scroll(short amt) {
    NV_SCROLL_PACKET p {};
    p.header.magic = util::endian::little(static_cast<int>(SCROLL_MAGIC_GEN5));
    p.scrollAmt1 = util::endian::big(amt);
    p.scrollAmt2 = util::endian::big(amt);
    return p;
  }

  SS_HSCROLL_PACKET hscroll(short amt) {
    SS_HSCROLL_PACKET p {};
    p.header.magic = util::endian::little(static_cast<int>(SS_HSCROLL_MAGIC));
    p.scrollAmount = util::endian::big(amt);
    return p;
  }

}  // namespace

TEST(InputBatchingTests, RelativeMouseMergesSmallDeltas) {
  // The regression case. A 1000 Hz mouse produces a stream of small deltas; if these do not
  // merge, every one becomes its own blocking SendInput on the single injection thread.
  auto dest = rel_mouse(10, -20);
  auto src = rel_mouse(5, -3);

  EXPECT_EQ(input::batch_result_e::batched, input::batch(&dest, &src));
  EXPECT_EQ(15, util::endian::big(dest.deltaX));
  EXPECT_EQ(-23, util::endian::big(dest.deltaY));
}

TEST(InputBatchingTests, RelativeMouseMergesRepeatedly) {
  // Batching walks the whole queue, so merging has to stay correct when applied in sequence.
  auto dest = rel_mouse(0, 0);
  for (int i = 0; i < 100; i++) {
    auto src = rel_mouse(3, -1);
    ASSERT_EQ(input::batch_result_e::batched, input::batch(&dest, &src));
  }
  EXPECT_EQ(300, util::endian::big(dest.deltaX));
  EXPECT_EQ(-100, util::endian::big(dest.deltaY));
}

TEST(InputBatchingTests, RelativeMouseRefusesToMergeOnPositiveOverflow) {
  auto dest = rel_mouse(SHRT_MAX, 0);
  auto src = rel_mouse(1, 0);

  EXPECT_EQ(input::batch_result_e::terminate_batch, input::batch(&dest, &src));
  // The wrapped sum must not be written back: SHRT_MAX + 1 wraps to SHRT_MIN, which would
  // invert the direction of motion.
  EXPECT_EQ(SHRT_MAX, util::endian::big(dest.deltaX));
}

TEST(InputBatchingTests, RelativeMouseRefusesToMergeOnNegativeOverflow) {
  auto dest = rel_mouse(0, SHRT_MIN);
  auto src = rel_mouse(0, -1);

  EXPECT_EQ(input::batch_result_e::terminate_batch, input::batch(&dest, &src));
  EXPECT_EQ(SHRT_MIN, util::endian::big(dest.deltaY));
}

TEST(InputBatchingTests, RelativeMouseChecksBothAxes) {
  // Y overflowing must terminate even when X is fine.
  auto dest = rel_mouse(1, SHRT_MAX);
  auto src = rel_mouse(1, 1);

  EXPECT_EQ(input::batch_result_e::terminate_batch, input::batch(&dest, &src));
  EXPECT_EQ(SHRT_MAX, util::endian::big(dest.deltaY));
}

TEST(InputBatchingTests, ScrollMergesSmallDeltas) {
  auto dest = scroll(120);
  auto src = scroll(120);

  EXPECT_EQ(input::batch_result_e::batched, input::batch(&dest, &src));
  EXPECT_EQ(240, util::endian::big(dest.scrollAmt1));
  // Both copies of the amount have to stay in agreement.
  EXPECT_EQ(240, util::endian::big(dest.scrollAmt2));
}

TEST(InputBatchingTests, ScrollRefusesToMergeOnOverflow) {
  auto dest = scroll(SHRT_MAX);
  auto src = scroll(1);

  EXPECT_EQ(input::batch_result_e::terminate_batch, input::batch(&dest, &src));
  EXPECT_EQ(SHRT_MAX, util::endian::big(dest.scrollAmt1));
}

TEST(InputBatchingTests, HScrollMergesSmallDeltas) {
  auto dest = hscroll(-40);
  auto src = hscroll(-25);

  EXPECT_EQ(input::batch_result_e::batched, input::batch(&dest, &src));
  EXPECT_EQ(-65, util::endian::big(dest.scrollAmount));
}

TEST(InputBatchingTests, HScrollRefusesToMergeOnOverflow) {
  auto dest = hscroll(SHRT_MIN);
  auto src = hscroll(-1);

  EXPECT_EQ(input::batch_result_e::terminate_batch, input::batch(&dest, &src));
  EXPECT_EQ(SHRT_MIN, util::endian::big(dest.scrollAmount));
}

TEST(InputBatchingTests, OverflowBuiltinReturnsTrueOnOverflow) {
  // The assumption the guards rest on, asserted directly so that a future edit which flips the
  // condition back has something unambiguous to fail against.
  short out = 0;
  EXPECT_FALSE(__builtin_add_overflow(static_cast<short>(1), static_cast<short>(2), &out));
  EXPECT_EQ(3, out);
  EXPECT_TRUE(__builtin_add_overflow(SHRT_MAX, static_cast<short>(1), &out));
}
