/**
 * @file tests/unit/test_focus_hints.cpp
 * @brief Test src/focus_hints.*.
 */
#include "../tests_common.h"

#include <cstddef>
#include <cstring>

#include <src/config.h>
#include <src/focus_hints.h>
#include <src/platform/common.h>

// These values are shared with moonlight-common-c and a mismatch is silent on the wire,
// so they are asserted rather than trusted. See artifacts/wire-contract-frame-trace.md §7.
TEST(FocusHintWireTests, MessageLayout) {
  ASSERT_EQ(sizeof(focus_hints::focus_hint_t), 8u);
  ASSERT_EQ(offsetof(focus_hints::focus_hint_t, version), 0u);
  ASSERT_EQ(offsetof(focus_hints::focus_hint_t, focus_type), 1u);
  ASSERT_EQ(offsetof(focus_hints::focus_hint_t, reserved), 2u);
  ASSERT_EQ(offsetof(focus_hints::focus_hint_t, sequence_number), 4u);

  focus_hints::focus_hint_t msg {};
  msg.version = focus_hints::FOCUS_HINT_VERSION;
  msg.focus_type = (std::uint8_t) focus_hints::kind_t::numeric;
  msg.reserved = 0;
  msg.sequence_number = 0x01020304u;

  EXPECT_EQ(msg.sequence_number, 0x01020304u);

  // Little-endian on the wire, like every other Apollo control payload
  const auto *raw = reinterpret_cast<const unsigned char *>(&msg);
  EXPECT_EQ(raw[0], 1);
  EXPECT_EQ(raw[1], 2);
  EXPECT_EQ(raw[2], 0);
  EXPECT_EQ(raw[3], 0);
  EXPECT_EQ(raw[4], 0x04);
  EXPECT_EQ(raw[5], 0x03);
  EXPECT_EQ(raw[6], 0x02);
  EXPECT_EQ(raw[7], 0x01);
}

TEST(FocusHintWireTests, ProtocolConstants) {
  EXPECT_EQ(focus_hints::FOCUS_HINT_PTYPE, 0x3020);
  EXPECT_EQ(focus_hints::FOCUS_HINT_VERSION, 1);

  // 0x3000-0x3002 are Apollo's own extensions and 0x3010/0x3011 are the clock sync
  EXPECT_NE(focus_hints::FOCUS_HINT_PTYPE, 0x3000);
  EXPECT_NE(focus_hints::FOCUS_HINT_PTYPE, 0x3010);
  EXPECT_NE(focus_hints::FOCUS_HINT_PTYPE, 0x3011);
}

TEST(FocusHintWireTests, EnumValuesAreTheWireValues) {
  EXPECT_EQ((std::uint8_t) focus_hints::kind_t::none, 0);
  EXPECT_EQ((std::uint8_t) focus_hints::kind_t::text, 1);
  EXPECT_EQ((std::uint8_t) focus_hints::kind_t::numeric, 2);
  EXPECT_EQ((std::uint8_t) focus_hints::kind_t::password, 3);
}

TEST(FocusHintWireTests, FeatureFlagBits) {
  EXPECT_EQ(focus_hints::CLIENT_FF_TEXT_FOCUS, 0x08u);
  EXPECT_EQ(focus_hints::HOST_FF_TEXT_FOCUS, 0x08u);

  // Must not collide with the platform input capabilities carried in the same field, nor
  // with the latency trace bit.
  EXPECT_EQ(focus_hints::HOST_FF_TEXT_FOCUS & platf::platform_caps::pen_touch, 0u);
  EXPECT_EQ(focus_hints::HOST_FF_TEXT_FOCUS & platf::platform_caps::controller_touch, 0u);
  EXPECT_EQ(focus_hints::HOST_FF_TEXT_FOCUS & 0x04u, 0u);
}

// The lock screen hint is declared by the operator, so parsing it has to be exact and has
// to fail safe: anything unrecognised must mean "send nothing", never a guessed keyboard.
TEST(FocusHintTests, LockScreenHintParsing) {
  EXPECT_EQ(focus_hints::kind_from_view("none"), focus_hints::kind_t::none);
  EXPECT_EQ(focus_hints::kind_from_view("text"), focus_hints::kind_t::text);
  EXPECT_EQ(focus_hints::kind_from_view("numeric"), focus_hints::kind_t::numeric);
  EXPECT_EQ(focus_hints::kind_from_view("password"), focus_hints::kind_t::password);

  // Anything else falls back to none rather than to a keyboard nobody asked for
  EXPECT_EQ(focus_hints::kind_from_view(""), focus_hints::kind_t::none);
  EXPECT_EQ(focus_hints::kind_from_view("pin"), focus_hints::kind_t::none);
  EXPECT_EQ(focus_hints::kind_from_view("Numeric"), focus_hints::kind_t::none);
  EXPECT_EQ(focus_hints::kind_from_view("enabled"), focus_hints::kind_t::none);
  EXPECT_EQ(focus_hints::kind_from_view("2"), focus_hints::kind_t::none);
}

TEST(FocusHintTests, LockScreenHintDefaultsToNone) {
  // Nobody should get a keyboard on their lock screen without asking for it
  EXPECT_EQ(config::video.lock_screen_focus_hint, focus_hints::kind_t::none);
}

TEST(FocusHintTests, NamesForEveryKind) {
  EXPECT_STREQ(focus_hints::to_string(focus_hints::kind_t::none), "none");
  EXPECT_STREQ(focus_hints::to_string(focus_hints::kind_t::text), "text");
  EXPECT_STREQ(focus_hints::to_string(focus_hints::kind_t::numeric), "numeric");
  EXPECT_STREQ(focus_hints::to_string(focus_hints::kind_t::password), "password");
}

// The zero-cost-when-disabled promise: start() must refuse before it touches the platform,
// so no hook is installed and no UI Automation client is created.
TEST(FocusHintTests, DisabledRefusesToStart) {
  const auto saved = config::video.focus_hints;
  config::video.focus_hints = false;

  bool called = false;
  EXPECT_FALSE(focus_hints::start([&called](focus_hints::kind_t) {
    called = true;
  }));
  EXPECT_FALSE(called);

  // Harmless when nothing was started
  focus_hints::stop();
  focus_hints::stop();

  config::video.focus_hints = saved;
}
