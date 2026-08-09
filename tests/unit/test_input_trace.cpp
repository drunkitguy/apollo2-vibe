/**
 * @file tests/unit/test_input_trace.cpp
 * @brief Test src/input_trace.*.
 *
 * The wire layout tests are the important ones: the client parses these bodies by fixed
 * offset and length, so a silent layout change here is a silent protocol break. They mirror
 * `artifacts/wire-contract-frame-trace.md` §8 and should fail if anyone edits the structs
 * without editing the contract.
 */
#include "../tests_common.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <src/config.h>
#include <src/frame_trace.h>
#include <src/input_trace.h>
#include <src/platform/common.h>

namespace {

  /**
   * @brief Restores the input trace configuration after each test so an enabled trace can't
   *        leak into another test and start writing files.
   */
  struct InputTraceTest: testing::Test {
    void SetUp() override {
      saved_enabled = config::input.input_trace;
      saved_capacity = config::input.input_trace_capacity;
      saved_path = config::input.input_trace_path;
    }

    void TearDown() override {
      input_trace::set_echo_sink(nullptr);
      config::input.input_trace = saved_enabled;
      config::input.input_trace_capacity = saved_capacity;
      config::input.input_trace_path = saved_path;
    }

    bool saved_enabled {};
    int saved_capacity {};
    std::string saved_path;
  };

}  // namespace

TEST(InputProbeWireTests, ProbeLayout) {
  // 24 bytes, little-endian, offsets fixed by contract §8.
  static_assert(sizeof(input_trace::input_probe_t) == 24);

  EXPECT_EQ(0u, offsetof(input_trace::input_probe_t, version));
  EXPECT_EQ(1u, offsetof(input_trace::input_probe_t, flags));
  EXPECT_EQ(2u, offsetof(input_trace::input_probe_t, reserved));
  EXPECT_EQ(4u, offsetof(input_trace::input_probe_t, sequence_number));
  EXPECT_EQ(8u, offsetof(input_trace::input_probe_t, client_event_us));
  EXPECT_EQ(16u, offsetof(input_trace::input_probe_t, client_send_us));
}

TEST(InputProbeWireTests, EchoLayout) {
  // The host added `coalesced_count` by splitting the client's proposed uint16 `reserved`.
  // These offsets are what make that a non-breaking change: everything after byte 3 is where
  // the original proposal put it.
  static_assert(sizeof(input_trace::input_probe_echo_t) == 24);

  EXPECT_EQ(0u, offsetof(input_trace::input_probe_echo_t, version));
  EXPECT_EQ(1u, offsetof(input_trace::input_probe_echo_t, flags));
  EXPECT_EQ(2u, offsetof(input_trace::input_probe_echo_t, coalesced_count));
  EXPECT_EQ(3u, offsetof(input_trace::input_probe_echo_t, reserved));
  EXPECT_EQ(4u, offsetof(input_trace::input_probe_echo_t, sequence_number));
  EXPECT_EQ(8u, offsetof(input_trace::input_probe_echo_t, host_rx_us));
  EXPECT_EQ(16u, offsetof(input_trace::input_probe_echo_t, host_inject_us));
}

TEST(InputProbeWireTests, EchoIsLittleEndianOnTheWire) {
  input_trace::input_probe_echo_t echo {};
  echo.sequence_number = 0x01020304u;
  echo.host_rx_us = 0x0102030405060708ull;

  std::uint8_t raw[sizeof(echo)];
  std::memcpy(raw, &echo, sizeof(echo));

  // Least significant byte first, regardless of the host's own endianness.
  EXPECT_EQ(0x04, raw[4]);
  EXPECT_EQ(0x03, raw[5]);
  EXPECT_EQ(0x02, raw[6]);
  EXPECT_EQ(0x01, raw[7]);
  EXPECT_EQ(0x08, raw[8]);
  EXPECT_EQ(0x01, raw[15]);
}

TEST(InputProbeWireTests, ProtocolConstants) {
  EXPECT_EQ(0x3030, input_trace::INPUT_PROBE_PTYPE);
  EXPECT_EQ(0x3031, input_trace::INPUT_PROBE_ECHO_PTYPE);
  EXPECT_EQ(1, input_trace::INPUT_PROBE_VERSION);
  EXPECT_EQ(0x01, input_trace::ECHO_FLAG_NO_INPUT);
  EXPECT_EQ(0, input_trace::COALESCED_UNKNOWN);
}

TEST(InputProbeWireTests, FeatureFlagBits) {
  EXPECT_EQ(0x10u, input_trace::CLIENT_FF_INPUT_PROBE);
  EXPECT_EQ(0x10u, input_trace::HOST_FF_INPUT_PROBE);

  // Must not collide with the frame trace's bit, or with the platform capability bits that
  // share the same featureFlags field.
  EXPECT_NE(input_trace::HOST_FF_INPUT_PROBE, frame_trace::HOST_FF_LATENCY_TRACE);
  EXPECT_EQ(0u, input_trace::HOST_FF_INPUT_PROBE & frame_trace::HOST_FF_LATENCY_TRACE);
  EXPECT_EQ(0u, input_trace::HOST_FF_INPUT_PROBE & platf::platform_caps::pen_touch);
  EXPECT_EQ(0u, input_trace::HOST_FF_INPUT_PROBE & platf::platform_caps::controller_touch);
  EXPECT_EQ(0u, input_trace::HOST_FF_INPUT_PROBE & platf::platform_caps::reserved_text_focus);
}

TEST(InputProbeWireTests, ProbeAndClockSyncTypesAreDistinct) {
  // Everything sharing the 0x3xxx Apollo extension range.
  EXPECT_NE(input_trace::INPUT_PROBE_PTYPE, input_trace::INPUT_PROBE_ECHO_PTYPE);
  EXPECT_NE(input_trace::INPUT_PROBE_PTYPE, frame_trace::CLOCK_SYNC_REQUEST_PTYPE);
  EXPECT_NE(input_trace::INPUT_PROBE_PTYPE, frame_trace::CLOCK_SYNC_RESPONSE_PTYPE);
  EXPECT_NE(input_trace::INPUT_PROBE_ECHO_PTYPE, frame_trace::CLOCK_SYNC_RESPONSE_PTYPE);
}

TEST(InputProbeWireTests, ClockIsSharedWithTheFrameTrace) {
  // The whole point of delegating: input and video stamps have to be comparable, and the
  // client converts both with the one offset it already estimates.
  const auto a = input_trace::now_us();
  const auto b = frame_trace::now_us();
  EXPECT_NEAR(static_cast<double>(a), static_cast<double>(b), 50000.0);
}

TEST_F(InputTraceTest, DisabledIsInert) {
  config::input.input_trace = false;
  ASSERT_FALSE(input_trace::enabled());

  bool sink_called = false;
  input_trace::set_echo_sink([&sink_called](const input_trace::input_probe_echo_t &) {
    sink_called = true;
  });

  input_trace::input_probe_t probe {};
  probe.version = input_trace::INPUT_PROBE_VERSION;
  probe.sequence_number = 7;
  input_trace::mark_next_input(probe, 1234);

  // Nothing was retained, so nothing can be attributed to an input packet.
  EXPECT_FALSE(input_trace::take_pending().valid);
  EXPECT_FALSE(sink_called);
}

TEST_F(InputTraceTest, DisabledRefusesToStart) {
  config::input.input_trace = false;

  input_trace::metadata_t meta;
  meta.client_name = "test";
  meta.gamepad_type = "x360";

  EXPECT_FALSE(input_trace::begin_session(meta));
  EXPECT_FALSE(input_trace::enabled());

  // Safe to call even though no session was ever started.
  input_trace::end_session();
}

TEST_F(InputTraceTest, PendingProbeDefaultsToInvalid) {
  input_trace::pending_t pending;
  EXPECT_FALSE(pending.valid);
  EXPECT_EQ(0u, pending.sequence_number);
  EXPECT_EQ(0, pending.host_rx_us);
}

TEST_F(InputTraceTest, CompleteIgnoresAnInvalidProbe) {
  // The common path: input arrives with no probe attached, because the client only probes
  // 20 times a second. This must not record anything or call the sink.
  bool sink_called = false;
  input_trace::set_echo_sink([&sink_called](const input_trace::input_probe_echo_t &) {
    sink_called = true;
  });

  input_trace::pending_t none;
  input_trace::complete(none, 0x0206, 1, 2, 3, 1);

  EXPECT_FALSE(sink_called);
}
