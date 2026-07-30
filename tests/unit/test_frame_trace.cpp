/**
 * @file tests/unit/test_frame_trace.cpp
 * @brief Test src/frame_trace.*.
 */
#include "../tests_common.h"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <src/config.h>
#include <src/frame_trace.h>
#include <src/platform/common.h>

namespace {

  /**
   * @brief Points the recorder at a scratch directory and restores the previous
   *        configuration afterwards, so an enabled trace can't leak into other tests.
   */
  struct FrameTraceTest: testing::Test {
    void SetUp() override {
      saved_enabled = config::video.frame_trace;
      saved_capacity = config::video.frame_trace_capacity;
      saved_path = config::video.frame_trace_path;

      dir = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "frame_trace";
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir);

      config::video.frame_trace_path = (dir / "trace.csv").string();
    }

    void TearDown() override {
      frame_trace::end_session();
      config::video.frame_trace = saved_enabled;
      config::video.frame_trace_capacity = saved_capacity;
      config::video.frame_trace_path = saved_path;
      std::filesystem::remove_all(dir);
    }

    /// @return The lines of the single CSV in the scratch directory, or an empty vector.
    std::vector<std::string> read_output() {
      std::vector<std::string> lines;
      for (const auto &entry : std::filesystem::directory_iterator {dir}) {
        std::ifstream in {entry.path()};
        for (std::string line; std::getline(in, line);) {
          lines.push_back(line);
        }
      }
      return lines;
    }

    std::filesystem::path dir;
    bool saved_enabled = false;
    int saved_capacity = 0;
    std::string saved_path;
  };

  frame_trace::host_stamps_t stamps_for(int i) {
    frame_trace::host_stamps_t s;
    s.capture_requested = 1000 * i;
    s.capture_complete = 1000 * i + 100;
    s.encode_submit = 1000 * i + 200;
    s.encode_complete = 1000 * i + 900;
    s.tx_pipeline_entry = 1000 * i + 920;
    return s;
  }

}  // namespace

// The extension travels on the wire, so its size and byte order are part of the protocol.
// These values mirror SS_FRAME_TIMESTAMP_EXT in moonlight-common-c src/Video.h and must
// not drift from it.
TEST(FrameTraceWireTests, ExtensionLayout) {
  ASSERT_EQ(sizeof(frame_trace::frame_timestamp_ext_t), 48u);
  ASSERT_EQ(offsetof(frame_trace::frame_timestamp_ext_t, validity_mask), 1u);
  ASSERT_EQ(offsetof(frame_trace::frame_timestamp_ext_t, frame_index), 4u);
  ASSERT_EQ(offsetof(frame_trace::frame_timestamp_ext_t, capture_requested_us), 8u);
  ASSERT_EQ(offsetof(frame_trace::frame_timestamp_ext_t, tx_pipeline_entry_us), 40u);

  // Version 2 kept the size so a peer still on version 1 skips the right number of bytes
  // and rejects on the version check rather than mis-locating the picture data.
  EXPECT_EQ(frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MAX, 2);
  EXPECT_EQ(frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MIN, 1);

  frame_trace::frame_timestamp_ext_t ext {};
  ext.ext_version = frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MAX;
  ext.validity_mask = frame_trace::STAMP_VALID_ENCODE_SUBMIT | frame_trace::STAMP_VALID_ENCODE_COMPLETE;
  ext.frame_index = 0xDEADBEEFu;
  ext.capture_requested_us = 0x0102030405060708ull;

  EXPECT_EQ(ext.frame_index, 0xDEADBEEFu);
  EXPECT_EQ(ext.capture_requested_us, 0x0102030405060708ull);

  const auto *raw = reinterpret_cast<const unsigned char *>(&ext);
  EXPECT_EQ(raw[0], 2);
  EXPECT_EQ(raw[1], 0x0C);
  EXPECT_EQ(raw[2], 0);
  EXPECT_EQ(raw[3], 0);
  EXPECT_EQ(raw[4], 0xEF);
  EXPECT_EQ(raw[5], 0xBE);
  EXPECT_EQ(raw[6], 0xAD);
  EXPECT_EQ(raw[7], 0xDE);
  EXPECT_EQ(raw[8], 0x08);
  EXPECT_EQ(raw[15], 0x01);
}

// The negotiation has to cope with a peer that predates the attribute entirely, which is
// the state of the client today.
TEST(FrameTraceWireTests, ExtensionVersionNegotiation) {
  // Peer says nothing -> the RTSP default lands here -> version 1, which it can read
  EXPECT_EQ(frame_trace::negotiate_ext_version(frame_trace::FRAME_TIMESTAMP_EXT_VERSION_ASSUMED), 1);
  EXPECT_EQ(frame_trace::negotiate_ext_version(1), 1);
  EXPECT_EQ(frame_trace::negotiate_ext_version(2), 2);

  // A newer peer must not drag us above what we can actually emit
  EXPECT_EQ(frame_trace::negotiate_ext_version(3), frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MAX);
  EXPECT_EQ(frame_trace::negotiate_ext_version(255), frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MAX);

  // Garbage or a hostile value must not produce a version we can't write
  EXPECT_EQ(frame_trace::negotiate_ext_version(0), frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MIN);
  EXPECT_EQ(frame_trace::negotiate_ext_version(-7), frame_trace::FRAME_TIMESTAMP_EXT_VERSION_MIN);
}

// The fix for round-1 finding A. The bit assignment is extracted from the packetisation
// path precisely so it can be checked here rather than only by reading stream.cpp.
TEST(FrameTraceWireTests, ValidityMaskReflectsWhichStagesWereStamped) {
  EXPECT_EQ(frame_trace::validity_mask_for({}), 0);

  frame_trace::host_stamps_t all;
  all.capture_requested = 1;
  all.capture_complete = 2;
  all.encode_submit = 3;
  all.encode_complete = 4;
  all.tx_pipeline_entry = 5;
  EXPECT_EQ(frame_trace::validity_mask_for(all), 0x1F);

  // The synchronous capture path: no "capture requested" hook, ever
  auto sync_path = all;
  sync_path.capture_requested = 0;
  EXPECT_EQ(frame_trace::validity_mask_for(sync_path), 0x1E);
  EXPECT_EQ(frame_trace::validity_mask_for(sync_path) & frame_trace::STAMP_VALID_CAPTURE_REQUESTED, 0);

  // A repeated frame on the minimum-FPS static-content path: capture never completed
  auto repeat_frame = all;
  repeat_frame.capture_complete = 0;
  EXPECT_EQ(frame_trace::validity_mask_for(repeat_frame), 0x1D);
  EXPECT_EQ(frame_trace::validity_mask_for(repeat_frame) & frame_trace::STAMP_VALID_CAPTURE_COMPLETE, 0);

  // A negative timestamp is still a measurement; only zero means "not stamped"
  frame_trace::host_stamps_t negative;
  negative.encode_submit = -1;
  EXPECT_EQ(frame_trace::validity_mask_for(negative), frame_trace::STAMP_VALID_ENCODE_SUBMIT);
}

TEST(FrameTraceWireTests, ValidityMaskBitsAreDistinct) {
  const std::uint8_t bits[] = {
    frame_trace::STAMP_VALID_CAPTURE_REQUESTED,
    frame_trace::STAMP_VALID_CAPTURE_COMPLETE,
    frame_trace::STAMP_VALID_ENCODE_SUBMIT,
    frame_trace::STAMP_VALID_ENCODE_COMPLETE,
    frame_trace::STAMP_VALID_TX_PIPELINE_ENTRY,
  };

  std::uint8_t all = 0;
  for (auto bit : bits) {
    // Exactly one bit each, and no two stages sharing one
    EXPECT_NE(bit, 0);
    EXPECT_EQ(bit & (bit - 1), 0);
    EXPECT_EQ(all & bit, 0);
    all |= bit;
  }
  EXPECT_EQ(all, 0x1F);
}

TEST(FrameTraceWireTests, ClockMessageLayout) {
  ASSERT_EQ(sizeof(frame_trace::clock_sync_request_t), 16u);
  ASSERT_EQ(sizeof(frame_trace::clock_sync_response_t), 32u);
  ASSERT_EQ(offsetof(frame_trace::clock_sync_request_t, client_tx_us), 8u);
  ASSERT_EQ(offsetof(frame_trace::clock_sync_response_t, host_rx_us), 16u);
  ASSERT_EQ(offsetof(frame_trace::clock_sync_response_t, host_tx_us), 24u);

  EXPECT_EQ(frame_trace::CLOCK_SYNC_REQUEST_PTYPE, 0x3010);
  EXPECT_EQ(frame_trace::CLOCK_SYNC_RESPONSE_PTYPE, 0x3011);

  frame_trace::clock_sync_response_t response {};
  response.sequence_number = 42;
  response.client_tx_us = 5;
  response.host_rx_us = 6;
  response.host_tx_us = 7;
  EXPECT_EQ(response.sequence_number, 42u);
  EXPECT_EQ(response.client_tx_us, 5u);
}

TEST(FrameTraceWireTests, HeaderTypesAreDistinct) {
  // A client that didn't advertise the capability keys off the discriminator byte to size
  // the frame header, so the traced value must never collide with the stock one.
  EXPECT_EQ(frame_trace::FRAME_HDR_DISC_SHORT, 0x01);
  EXPECT_EQ(frame_trace::FRAME_HDR_DISC_SHORT_TRACE, 0x02);
  EXPECT_NE(frame_trace::FRAME_HDR_DISC_SHORT_TRACE, frame_trace::FRAME_HDR_DISC_SHORT);
}

TEST(FrameTraceWireTests, FeatureFlagBits) {
  // Mirrors ML_FF_LATENCY_TRACE / SS_FF_LATENCY_TRACE, and must not collide with the
  // platform capability bits already carried in x-ss-general.featureFlags.
  EXPECT_EQ(frame_trace::CLIENT_FF_LATENCY_TRACE, 0x04u);
  EXPECT_EQ(frame_trace::HOST_FF_LATENCY_TRACE, 0x04u);
  EXPECT_EQ(frame_trace::HOST_FF_LATENCY_TRACE & platf::platform_caps::pen_touch, 0u);
  EXPECT_EQ(frame_trace::HOST_FF_LATENCY_TRACE & platf::platform_caps::controller_touch, 0u);
}

TEST_F(FrameTraceTest, DisabledRecordsNothing) {
  config::video.frame_trace = false;

  frame_trace::metadata_t meta;
  EXPECT_FALSE(frame_trace::begin_session(meta));
  EXPECT_FALSE(frame_trace::enabled());

  frame_trace::submit(1, stamps_for(1), 1950, 4096, true);
  frame_trace::end_session();

  EXPECT_TRUE(read_output().empty());
}

TEST_F(FrameTraceTest, SecondConcurrentSessionDoesNotTakeOwnership) {
  config::video.frame_trace = true;
  config::video.frame_trace_capacity = 8;

  frame_trace::metadata_t meta;
  ASSERT_TRUE(frame_trace::begin_session(meta));
  EXPECT_FALSE(frame_trace::begin_session(meta));
  EXPECT_TRUE(frame_trace::enabled());

  frame_trace::end_session();
  EXPECT_FALSE(frame_trace::enabled());

  // Flushing again must be harmless
  frame_trace::end_session();
}

TEST_F(FrameTraceTest, RingBufferKeepsTheMostRecentFrames) {
  config::video.frame_trace = true;
  config::video.frame_trace_capacity = 4;

  frame_trace::metadata_t meta;
  meta.width = 1920;
  meta.height = 1080;
  meta.framerate = 120;
  meta.video_format = 1;
  meta.chroma_sampling_type = 1;
  meta.trace_ext_negotiated = true;
  meta.trace_ext_version = 2;
  ASSERT_TRUE(frame_trace::begin_session(meta));

  for (int i = 1; i <= 10; i++) {
    frame_trace::submit(i, stamps_for(i), 1000 * i + 950, 4096 + i, i == 1);
  }

  frame_trace::end_session();

  auto lines = read_output();
  ASSERT_FALSE(lines.empty());

  // Metadata block, then the column header, then exactly capacity rows
  std::vector<std::string> data;
  bool past_header = false;
  for (const auto &line : lines) {
    if (line.rfind("#", 0) == 0) {
      continue;
    }
    if (!past_header) {
      EXPECT_EQ(line.rfind("frame_index,frame_id,", 0), 0u);
      past_header = true;
      continue;
    }
    data.push_back(line);
  }

  ASSERT_EQ(data.size(), 4u);
  EXPECT_EQ(data[0].rfind("7,7,0,4103,7000,7100,7200,7900,7920,7950", 0), 0u);
  EXPECT_EQ(data[3].rfind("10,10,0,4106,10000,10100,10200,10900,10920,10950", 0), 0u);

  // The metadata block is what makes runs comparable, so it must be present in full
  auto has = [&lines](const std::string &prefix) {
    for (const auto &line : lines) {
      if (line.rfind(prefix, 0) == 0) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(has("# resolution=1920x1080"));
  EXPECT_TRUE(has("# framerate=120"));
  EXPECT_TRUE(has("# codec=HEVC"));
  EXPECT_TRUE(has("# chroma=4:4:4"));
  EXPECT_TRUE(has("# clock=steady_clock_us"));
  // A joined dataset is uninterpretable without the extension version it was produced with
  EXPECT_TRUE(has("# trace_ext_negotiated=1"));
  EXPECT_TRUE(has("# trace_ext_version=2"));
  EXPECT_TRUE(has("# frames_submitted=10"));
  EXPECT_TRUE(has("# frames_kept=4"));

  // Every submitted frame had both transmit stamps 30 us apart, so the measured bias the
  // client's consumers need is reported and is exactly that.
  EXPECT_TRUE(has("# tx_pipeline_entry_bias_us_p50=30"));
  EXPECT_TRUE(has("# tx_pipeline_entry_bias_us_max=30"));
}

TEST_F(FrameTraceTest, TransmitBiasIsMinusOneWhenNeverMeasured) {
  config::video.frame_trace = true;
  config::video.frame_trace_capacity = 4;

  frame_trace::metadata_t meta;
  ASSERT_TRUE(frame_trace::begin_session(meta));

  // No tx_pipeline_entry stamp at all: the bias is unknown, not zero.
  frame_trace::host_stamps_t s;
  s.encode_complete = 500;
  frame_trace::submit(1, s, 900, 1024, false);

  frame_trace::end_session();

  bool found = false;
  for (const auto &line : read_output()) {
    if (line == "# tx_pipeline_entry_bias_us_p50=-1") {
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST_F(FrameTraceTest, ReferenceClockIsMonotonic) {
  auto a = frame_trace::now_us();
  auto b = frame_trace::now_us();
  EXPECT_LE(a, b);
}
