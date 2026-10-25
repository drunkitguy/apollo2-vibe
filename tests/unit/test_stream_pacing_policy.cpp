/**
 * @file tests/unit/test_stream_pacing_policy.cpp
 * @brief Tests for the video send pacing rate and per-frame send schedule.
 */
#include "../tests_common.h"
#include "src/stream_pacing_policy.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using stream_pacing::pacing_decision_t;
  using stream_pacing::pacing_input_t;
  using stream_pacing::pacing_mode_e;
  using stream_pacing::pacing_mode_from_view;
  using stream_pacing::pacing_source_e;
  using stream_pacing::resolve_pacing_bps;
  using stream_pacing::scheduled_batch_t;
  using stream_pacing::send_schedule_t;

  constexpr std::uint64_t mbps = 1'000'000;
  constexpr int default_multiplier = stream_pacing::default_auto_multiplier_percent;
  constexpr std::size_t blocksize = 1408;  // 1392-byte packets plus the RTP header.
  constexpr std::size_t max_batch_bytes = 64 * 1024;

  pacing_decision_t resolve(const int configured_kbps, const pacing_mode_e mode, const int encoder_kbps, const int fec = 20, const int multiplier = default_multiplier) {
    return resolve_pacing_bps({
      .configured_kbps = configured_kbps,
      .mode = mode,
      .encoder_bitrate_kbps = encoder_kbps,
      .fec_percentage = fec,
      .multiplier_percent = multiplier,
    });
  }

  /// The 1.2.2 pacing rate code in stream.cpp, kept here to prove legacy and explicit caps are unchanged.
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

  /// The 1.2.2 per-frame send loop of stream.cpp, reduced to the batch and pacing decisions.
  std::vector<scheduled_batch_t> schedule_122(const std::uint64_t pacing_bps, const std::size_t block, const std::size_t batch_bytes, const std::vector<std::size_t> &shards_per_fec_block, std::chrono::nanoseconds &frame_end) {
    size_t ratecontrol_packets_in_1ms = pacing_bps / 1000 / block / 8;
    if (ratecontrol_packets_in_1ms == 0) {
      ratecontrol_packets_in_1ms = 1;
    }
    size_t send_batch_size = std::max<size_t>(1, batch_bytes / block);
    send_batch_size = std::min<size_t>(64, send_batch_size);

    std::vector<scheduled_batch_t> batches;
    size_t ratecontrol_frame_packets_sent = 0;
    size_t ratecontrol_group_packets_sent = 0;
    for (std::size_t fec_block = 0; fec_block < shards_per_fec_block.size(); ++fec_block) {
      const std::size_t shards = shards_per_fec_block[fec_block];
      size_t next_shard_to_send = 0;
      for (std::size_t x = 0; x < shards; ++x) {
        if (x - next_shard_to_send + 1 >= send_batch_size || x + 1 == shards) {
          scheduled_batch_t batch {fec_block, x - next_shard_to_send + 1, false, 0ns};
          if (ratecontrol_group_packets_sent >= ratecontrol_packets_in_1ms || ratecontrol_frame_packets_sent == 0) {
            batch.paced = true;
            batch.due = std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) * ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;
            ratecontrol_group_packets_sent = 0;
          }
          batches.push_back(batch);
          ratecontrol_group_packets_sent += batch.packets;
          ratecontrol_frame_packets_sent += batch.packets;
          next_shard_to_send = x + 1;
        }
      }
    }
    frame_end = std::chrono::duration_cast<std::chrono::nanoseconds>(1ms) * ratecontrol_frame_packets_sent / ratecontrol_packets_in_1ms;
    return batches;
  }

  std::vector<scheduled_batch_t> schedule_of(const send_schedule_t &schedule, const std::vector<std::size_t> &shards_per_fec_block, std::chrono::nanoseconds &frame_end) {
    std::vector<scheduled_batch_t> batches;
    const auto packets = stream_pacing::for_each_scheduled_batch(schedule, shards_per_fec_block, [&](const scheduled_batch_t &batch) {
      batches.push_back(batch);
    });
    frame_end = stream_pacing::due_offset(schedule, packets);
    return batches;
  }

  /// Shards per FEC block of an average frame, roughly as stream.cpp splits it (20% FEC,
  /// at most 255 shards per FEC block).
  std::vector<std::size_t> average_frame_shards(const int encoder_kbps, const int fps, const int fec = 20) {
    const std::size_t payload = blocksize - 16;
    const std::size_t bytes = static_cast<std::size_t>(encoder_kbps) * 1000 / 8 / static_cast<std::size_t>(fps);
    std::size_t data = (bytes + payload - 1) / payload;
    const std::size_t max_data_per_block = 255 * 100 / (100 + fec);
    std::vector<std::size_t> blocks;
    while (data > 0) {
      const std::size_t block_data = std::min(data, max_data_per_block);
      blocks.push_back(block_data + (block_data * fec + 99) / 100);
      data -= block_data;
    }
    return blocks;
  }
}  // namespace

TEST(StreamPacingPolicy, ParsesModes) {
  EXPECT_EQ(pacing_mode_from_view("auto"), pacing_mode_e::auto_rate);
  EXPECT_EQ(pacing_mode_from_view("legacy"), pacing_mode_e::legacy);
  EXPECT_FALSE(pacing_mode_from_view("").has_value());
  EXPECT_FALSE(pacing_mode_from_view("Auto").has_value());
  EXPECT_EQ(stream_pacing::default_pacing_mode, pacing_mode_e::legacy);
  EXPECT_EQ(stream_pacing::to_string(pacing_mode_e::auto_rate), "auto");
  EXPECT_EQ(stream_pacing::to_string(pacing_mode_e::legacy), "legacy");
}

TEST(StreamPacingPolicy, LegacyIs800Mbps) {
  EXPECT_EQ(resolve(0, pacing_mode_e::legacy, 100'000), (pacing_decision_t {800 * mbps, pacing_source_e::legacy, false}));
}

TEST(StreamPacingPolicy, LegacyAndExplicitCapsMatch122ForEveryBitrate) {
  for (int encoder_kbps = 0; encoder_kbps <= 1'500'000; encoder_kbps += 2'500) {
    for (const int fec : {0, 20, 80, 255}) {
      EXPECT_EQ(resolve(0, pacing_mode_e::legacy, encoder_kbps, fec).bps, pacing_bps_122(0, encoder_kbps)) << encoder_kbps;
      for (const int configured_kbps : {1, 50'000, 130'000, 200'000, 1'000'000, 10'000'000}) {
        for (const auto mode : {pacing_mode_e::legacy, pacing_mode_e::auto_rate}) {
          EXPECT_EQ(resolve(configured_kbps, mode, encoder_kbps, fec).bps, pacing_bps_122(configured_kbps, encoder_kbps))
            << configured_kbps << " kbps at " << encoder_kbps;
        }
      }
    }
  }
}

TEST(StreamPacingPolicy, ExplicitCapWinsOverAuto) {
  // max(200, 150 * 1.1 = 165) = 200 Mbps.
  EXPECT_EQ(resolve(200'000, pacing_mode_e::auto_rate, 150'000), (pacing_decision_t {200 * mbps, pacing_source_e::configured, false}));
}

TEST(StreamPacingPolicy, ExplicitCapBelowTheEncoderIsRaisedTo110Percent) {
  // The explicit floor stays the 1.2.2 one: 110% of the encoder bitrate, without FEC.
  EXPECT_EQ(resolve(50'000, pacing_mode_e::auto_rate, 100'000), (pacing_decision_t {110 * mbps, pacing_source_e::configured, true}));
}

TEST(StreamPacingPolicy, AutoIsFourTimesTheEncoderBitrateWithin300To800Mbps) {
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 100'000), (pacing_decision_t {400 * mbps, pacing_source_e::auto_rate, false}));
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 150'000).bps, 600 * mbps);
  // A 100 Mbps client setting with 20% FEC gives an encoder bitrate of about 79 Mbps.
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 79'000).bps, 316 * mbps);
  // Floor.
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 20'000).bps, 300 * mbps);
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 50'000).bps, 300 * mbps);
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 0).bps, 300 * mbps);
  // Ceiling (the legacy rate).
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 300'000).bps, 800 * mbps);
}

TEST(StreamPacingPolicy, AutoFloorIs110PercentOfTheWireRate) {
  EXPECT_EQ(stream_pacing::wire_floor_bps(100'000, 20), 132 * mbps);
  EXPECT_EQ(stream_pacing::wire_floor_bps(100'000, 0), 110 * mbps);
  // 650 Mbps encoder with 20% FEC puts 780 Mbps on the wire: the floor (858 Mbps) beats the ceiling.
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 650'000, 20).bps, 858 * mbps);
  // With FEC 100% and a low multiplier the wire floor beats the target.
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 200'000, 100, 150).bps, 440 * mbps);
  for (int encoder_kbps = 0; encoder_kbps <= 1'500'000; encoder_kbps += 1'000) {
    for (const int fec : {0, 20, 50, 100, 255}) {
      const auto decision = resolve(0, pacing_mode_e::auto_rate, encoder_kbps, fec);
      EXPECT_GE(decision.bps, stream_pacing::wire_floor_bps(encoder_kbps, fec)) << encoder_kbps << " fec " << fec;
      EXPECT_GE(decision.bps, 300 * mbps) << encoder_kbps;
      if (stream_pacing::wire_floor_bps(encoder_kbps, fec) <= 800 * mbps) {
        EXPECT_LE(decision.bps, 800 * mbps) << encoder_kbps << " fec " << fec;
      }
    }
  }
}

TEST(StreamPacingPolicy, MultiplierIsClampedToItsRange) {
  EXPECT_EQ(stream_pacing::clamp_auto_multiplier_percent(100), 150);
  EXPECT_EQ(stream_pacing::clamp_auto_multiplier_percent(5000), 1000);
  EXPECT_EQ(stream_pacing::clamp_auto_multiplier_percent(250), 250);
  // 250 Mbps at 100% is raised to 150%: 375 Mbps.
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 250'000, 20, 100).bps, 375 * mbps);
  // 50 Mbps at 5000% is clamped to 1000%: 500 Mbps.
  EXPECT_EQ(resolve(0, pacing_mode_e::auto_rate, 50'000, 20, 5000).bps, 500 * mbps);
}

TEST(StreamPacingSchedule, LegacyAndExplicitSchedulesMatch122) {
  for (const std::uint64_t pacing_bps : {std::uint64_t {1}, 50 * mbps, 110 * mbps, 300 * mbps, 800 * mbps, 1'100 * mbps, 10'000 * mbps}) {
    for (const std::size_t block : {std::size_t {300}, std::size_t {1040}, std::size_t {1408}, std::size_t {1500}}) {
      for (const std::size_t batch_bytes : {std::size_t {16 * 1024}, std::size_t {32 * 1024}, max_batch_bytes}) {
        const auto schedule = stream_pacing::plan_send_schedule(pacing_bps, block, batch_bytes, false);
        EXPECT_FALSE(schedule.pace_every_batch);
        for (const auto &frame : std::vector<std::vector<std::size_t>> {{1}, {7}, {36}, {46}, {47}, {120}, {255}, {255, 255, 40}, {64, 1, 63}}) {
          std::chrono::nanoseconds end_122 {};
          std::chrono::nanoseconds end_now {};
          EXPECT_EQ(schedule_of(schedule, frame, end_now), schedule_122(pacing_bps, block, batch_bytes, frame, end_122))
            << pacing_bps << " bps, block " << block << ", batch " << batch_bytes;
          EXPECT_EQ(end_now, end_122);
        }
      }
    }
  }
}

TEST(StreamPacingSchedule, LegacySendsASmallFrameAsOneUnpacedBurst) {
  // 50 Mbps at 120 fps with 20% FEC is 46 shards: one 64 KB batch at line rate.
  const auto shards = average_frame_shards(50'000, 120);
  ASSERT_EQ(shards, (std::vector<std::size_t> {46}));
  const auto schedule = stream_pacing::plan_send_schedule(resolve(0, pacing_mode_e::legacy, 50'000).bps, blocksize, max_batch_bytes, false);
  EXPECT_EQ(schedule, (send_schedule_t {71, 46, false}));
  std::chrono::nanoseconds end {};
  EXPECT_EQ(schedule_of(schedule, shards, end), (std::vector<scheduled_batch_t> {{0, 46, true, 0ns}}));
}

TEST(StreamPacingSchedule, AutoSpreadsA50MbpsFrameOverPacedBatches) {
  const auto shards = average_frame_shards(50'000, 120);
  const auto schedule = stream_pacing::plan_send_schedule(resolve(0, pacing_mode_e::auto_rate, 50'000).bps, blocksize, max_batch_bytes, true);
  // 300 Mbps is 26 packets per ms: batches of 13 packets every 0.5 ms.
  EXPECT_EQ(schedule, (send_schedule_t {26, 13, true}));
  std::chrono::nanoseconds end {};
  EXPECT_EQ(schedule_of(schedule, shards, end), (std::vector<scheduled_batch_t> {
                                                  {0, 13, true, 0ns},
                                                  {0, 13, true, 500'000ns},
                                                  {0, 13, true, 1'000'000ns},
                                                  {0, 7, true, 1'500'000ns},
                                                }));
  EXPECT_EQ(end, 1'769'230ns);
}

TEST(StreamPacingSchedule, AutoPacesEveryBatchForTypicalBitratesAndFrameRates) {
  for (const int encoder_kbps : {20'000, 50'000, 100'000, 150'000}) {
    for (const int fps : {60, 120}) {
      const auto shards = average_frame_shards(encoder_kbps, fps);
      const auto pacing = resolve(0, pacing_mode_e::auto_rate, encoder_kbps);
      const auto schedule = stream_pacing::plan_send_schedule(pacing.bps, blocksize, max_batch_bytes, true);
      const std::size_t expected_per_ms = pacing.bps / 1000 / blocksize / 8;
      EXPECT_EQ(schedule.packets_per_ms, expected_per_ms);
      EXPECT_EQ(schedule.batch_size, std::max<std::size_t>(4, expected_per_ms / 2));

      std::chrono::nanoseconds end {};
      const auto batches = schedule_of(schedule, shards, end);
      std::size_t sent = 0;
      std::size_t total = 0;
      for (const auto block_shards : shards) {
        total += block_shards;
      }
      ASSERT_FALSE(batches.empty());
      for (const auto &batch : batches) {
        EXPECT_TRUE(batch.paced) << encoder_kbps << " kbps at " << fps;
        EXPECT_LE(batch.packets, schedule.batch_size);
        EXPECT_EQ(batch.due, std::chrono::nanoseconds(1'000'000ull * sent / schedule.packets_per_ms));
        sent += batch.packets;
      }
      EXPECT_EQ(sent, total);
      // The frame takes as long on the wire as the pacing rate allows; it never leaves in one burst
      // unless it fits in a single batch.
      EXPECT_EQ(end, std::chrono::nanoseconds(1'000'000ull * total / schedule.packets_per_ms));
      if (total > schedule.batch_size) {
        EXPECT_GT(batches.size(), 1u) << encoder_kbps << " kbps at " << fps;
      }
      // A frame is sent within one frame interval.
      EXPECT_LT(end, std::chrono::nanoseconds(1'000'000'000 / fps)) << encoder_kbps << " kbps at " << fps;
    }
  }
}

TEST(StreamPacingSchedule, AutoBatchesRespectTheByteAndGsoLimits) {
  // 800 Mbps would allow 35 packets per 0.5 ms; a 16 KB batch limit holds 11 packets of 1408 bytes.
  EXPECT_EQ(stream_pacing::plan_send_schedule(800 * mbps, blocksize, 16 * 1024, true).batch_size, 11u);
  // Tiny packets: 0.5 ms at 10 Gbps is far more than the 64-packet GSO limit.
  EXPECT_EQ(stream_pacing::plan_send_schedule(10'000 * mbps, 300, max_batch_bytes, true).batch_size, 64u);
  // Very low rates still send at least 4 packets per batch.
  EXPECT_EQ(stream_pacing::plan_send_schedule(1 * mbps, blocksize, max_batch_bytes, true), (send_schedule_t {1, 4, true}));
}

TEST(StreamPacingSchedule, DefaultsReproduceThe122RateAndSchedule) {
  // A default-constructed input is the default config: legacy pacing, no explicit cap.
  for (int encoder_kbps = 0; encoder_kbps <= 300'000; encoder_kbps += 5'000) {
    pacing_input_t input;
    input.encoder_bitrate_kbps = encoder_kbps;
    input.fec_percentage = 20;
    const auto decision = resolve_pacing_bps(input);
    EXPECT_EQ(decision.bps, pacing_bps_122(0, encoder_kbps));
    EXPECT_EQ(decision.source, pacing_source_e::legacy);
    const auto schedule = stream_pacing::plan_send_schedule(decision.bps, blocksize, max_batch_bytes, decision.source == pacing_source_e::auto_rate);
    for (const int fps : {60, 120}) {
      const auto frame = average_frame_shards(encoder_kbps == 0 ? 1 : encoder_kbps, fps);
      std::chrono::nanoseconds end_122 {};
      std::chrono::nanoseconds end_now {};
      EXPECT_EQ(schedule_of(schedule, frame, end_now), schedule_122(decision.bps, blocksize, max_batch_bytes, frame, end_122));
      EXPECT_EQ(end_now, end_122);
    }
  }
}
