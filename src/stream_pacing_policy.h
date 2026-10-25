/**
 * @file src/stream_pacing_policy.h
 * @brief Platform-neutral choice of the video send pacing rate and per-frame send schedule.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace stream_pacing {
  /**
   * @brief Value of the `pacing_mode` config option.
   */
  enum class pacing_mode_e {
    auto_rate,  ///< `auto` (experimental, opt-in): a multiple of the encoder bitrate, sent in small paced batches.
    legacy,  ///< `legacy` (default): 80% of 1 Gbps with 64 KB batches, as in host 1.2.2.
  };

  inline constexpr pacing_mode_e default_pacing_mode = pacing_mode_e::legacy;  ///< The 1.2.2 behaviour; auto is opt-in.

  inline constexpr int default_auto_multiplier_percent = 400;
  inline constexpr int min_auto_multiplier_percent = 150;
  inline constexpr int max_auto_multiplier_percent = 1000;

  /// 80% of 1 Gbps: the 1.2.2 rate when pacing_max_bitrate_kbps is 0.
  inline constexpr std::uint64_t legacy_pacing_bps = 800'000'000;
  inline constexpr std::uint64_t auto_floor_bps = 300'000'000;
  inline constexpr std::uint64_t auto_ceiling_bps = legacy_pacing_bps;

  /**
   * @brief Parses a `pacing_mode` value.
   * @return The value, or std::nullopt when it is not `auto` or `legacy`.
   */
  constexpr std::optional<pacing_mode_e> pacing_mode_from_view(const std::string_view value) noexcept {
    if (value == "auto") {
      return pacing_mode_e::auto_rate;
    }
    if (value == "legacy") {
      return pacing_mode_e::legacy;
    }
    return std::nullopt;
  }

  constexpr std::string_view to_string(const pacing_mode_e value) noexcept {
    switch (value) {
      case pacing_mode_e::auto_rate:
        return "auto";
      case pacing_mode_e::legacy:
        return "legacy";
    }
    return "auto";
  }

  /**
   * @brief Where the pacing rate came from.
   */
  enum class pacing_source_e {
    configured,  ///< pacing_max_bitrate_kbps is set.
    legacy,  ///< pacing_mode = legacy.
    auto_rate,  ///< pacing_mode = auto.
  };

  constexpr std::string_view to_string(const pacing_source_e value) noexcept {
    switch (value) {
      case pacing_source_e::configured:
        return "pacing_max_bitrate_kbps";
      case pacing_source_e::legacy:
        return "legacy";
      case pacing_source_e::auto_rate:
        return "auto";
    }
    return "auto";
  }

  /**
   * @brief Inputs of resolve_pacing_bps().
   */
  struct pacing_input_t {
    int configured_kbps = 0;  ///< pacing_max_bitrate_kbps; 0 means not set.
    pacing_mode_e mode = default_pacing_mode;  ///< pacing_mode.
    int encoder_bitrate_kbps = 0;  ///< Encoder bitrate of the session, which can change mid-session.
    int fec_percentage = 0;  ///< fec_percentage: FEC shards added on the wire, in percent of the data.
    int multiplier_percent = default_auto_multiplier_percent;  ///< pacing_auto_multiplier_percent.
  };

  struct pacing_decision_t {
    std::uint64_t bps;  ///< Send pacing rate in bits per second.
    pacing_source_e source;
    bool configured_below_stream;  ///< pacing_max_bitrate_kbps was raised to 110% of the encoder bitrate.

    constexpr bool operator==(const pacing_decision_t &) const = default;
  };

  constexpr int clamp_auto_multiplier_percent(const int multiplier_percent) noexcept {
    return std::clamp(multiplier_percent, min_auto_multiplier_percent, max_auto_multiplier_percent);
  }

  /**
   * @brief 110% of the encoder bitrate: the 1.2.2 floor of an explicit pacing_max_bitrate_kbps.
   */
  constexpr std::uint64_t encoder_floor_bps(const int encoder_bitrate_kbps) noexcept {
    return static_cast<std::uint64_t>(std::max(encoder_bitrate_kbps, 0)) * 1000u * 110u / 100u;
  }

  /**
   * @brief 110% of the wire rate (encoder bitrate plus FEC): the floor of `auto`.
   *
   * The pacer must never be slower than what each frame puts on the wire, or the packet
   * queue (and stream latency) grows without bound.
   */
  constexpr std::uint64_t wire_floor_bps(const int encoder_bitrate_kbps, const int fec_percentage) noexcept {
    const std::uint64_t encoder_bps = static_cast<std::uint64_t>(std::max(encoder_bitrate_kbps, 0)) * 1000u;
    const std::uint64_t wire_bps = encoder_bps * static_cast<std::uint64_t>(100 + std::max(fec_percentage, 0)) / 100u;
    return wire_bps * 110u / 100u;
  }

  /**
   * @brief Picks the send pacing rate for one frame.
   *
   * An explicit pacing_max_bitrate_kbps always wins, raised to 110% of the encoder bitrate
   * when it is lower (the 1.2.2 rule). Otherwise `legacy` returns 800 Mbps, and `auto`
   * returns the encoder bitrate times the multiplier, clamped to 300 to 800 Mbps and never
   * below 110% of the wire rate (encoder bitrate plus FEC).
   */
  constexpr pacing_decision_t resolve_pacing_bps(const pacing_input_t &input) noexcept {
    if (input.configured_kbps > 0) {
      const std::uint64_t configured_bps = static_cast<std::uint64_t>(input.configured_kbps) * 1000u;
      const std::uint64_t floor_bps = encoder_floor_bps(input.encoder_bitrate_kbps);
      if (configured_bps < floor_bps) {
        return {floor_bps, pacing_source_e::configured, true};
      }
      return {configured_bps, pacing_source_e::configured, false};
    }

    if (input.mode == pacing_mode_e::legacy) {
      return {legacy_pacing_bps, pacing_source_e::legacy, false};
    }

    const std::uint64_t encoder_bps = static_cast<std::uint64_t>(std::max(input.encoder_bitrate_kbps, 0)) * 1000u;
    const std::uint64_t target_bps = encoder_bps * static_cast<std::uint64_t>(clamp_auto_multiplier_percent(input.multiplier_percent)) / 100u;
    const std::uint64_t clamped_bps = std::clamp(target_bps, auto_floor_bps, auto_ceiling_bps);
    return {std::max(clamped_bps, wire_floor_bps(input.encoder_bitrate_kbps, input.fec_percentage)), pacing_source_e::auto_rate, false};
  }

  /// Generic Segmentation Offload on Linux can't send more than 64 packets in one batch.
  inline constexpr std::size_t max_send_batch_packets = 64;
  /// Smallest batch of `auto`, so tiny batches do not cost one sleep per packet.
  inline constexpr std::size_t min_paced_batch_packets = 4;

  /**
   * @brief How the packets of one frame are cut into send_batch() calls and paced.
   */
  struct send_schedule_t {
    std::size_t packets_per_ms;  ///< Packets the pacing rate allows per millisecond (at least 1).
    std::size_t batch_size;  ///< Most packets in one send_batch() call.
    bool pace_every_batch;  ///< Wait for every batch's due time, not only once per millisecond of packets.

    constexpr bool operator==(const send_schedule_t &) const = default;
  };

  /**
   * @brief Plans the per-frame send schedule.
   *
   * Without paced batches this is the 1.2.2 schedule: batches of up to 64 KB (and 64
   * packets), and a wait only before the first batch of a frame and after each millisecond's
   * worth of packets. A frame smaller than one batch then leaves at line rate. With paced
   * batches (`auto`), a batch holds about 0.5 ms of packets at the pacing rate (at least 4)
   * and every batch waits for its due time, so even small frames are spread out.
   *
   * @param pacing_bps The pacing rate from resolve_pacing_bps().
   * @param blocksize Bytes per packet (shard).
   * @param max_batch_size_bytes Most bytes in one send_batch() call (64 KB or video_max_batch_size_kb).
   * @param paced_batches Whether to use paced batches (`auto` only).
   */
  constexpr send_schedule_t plan_send_schedule(
    const std::uint64_t pacing_bps,
    const std::size_t blocksize,
    const std::size_t max_batch_size_bytes,
    const bool paced_batches
  ) noexcept {
    const std::size_t block = std::max<std::size_t>(blocksize, 1);
    std::size_t packets_per_ms = static_cast<std::size_t>(pacing_bps / 1000 / block / 8);
    if (packets_per_ms == 0) {
      // Floor at one packet/ms so the pacing never divides by zero and we still send
      // something on absurdly low bitrate caps.
      packets_per_ms = 1;
    }

    std::size_t batch_size = std::max<std::size_t>(1, max_batch_size_bytes / block);
    batch_size = std::min<std::size_t>(max_send_batch_packets, batch_size);
    if (paced_batches) {
      batch_size = std::min(batch_size, std::max(min_paced_batch_packets, packets_per_ms / 2));
    }
    return {packets_per_ms, batch_size, paced_batches};
  }

  /**
   * @brief Whether the next batch waits for its due time.
   * @param group_packets_sent Packets sent since the last wait.
   * @param frame_packets_sent Packets of this frame sent so far.
   */
  constexpr bool pace_before_batch(const send_schedule_t &schedule, const std::size_t group_packets_sent, const std::size_t frame_packets_sent) noexcept {
    // The first batch of a frame always waits, to account for the end of the previous frame.
    return schedule.pace_every_batch ||
           group_packets_sent >= schedule.packets_per_ms ||
           frame_packets_sent == 0;
  }

  /**
   * @brief Due time of a batch (or the end of a frame), relative to the frame's start.
   * @param frame_packets_sent Packets of this frame sent before the batch.
   */
  constexpr std::chrono::nanoseconds due_offset(const send_schedule_t &schedule, const std::size_t frame_packets_sent) noexcept {
    return std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(
      std::uint64_t {1'000'000} * frame_packets_sent / schedule.packets_per_ms
    ));
  }

  /**
   * @brief One send_batch() call of a frame.
   */
  struct scheduled_batch_t {
    std::size_t fec_block;  ///< FEC block the batch belongs to.
    std::size_t packets;  ///< Packets in the batch.
    bool paced;  ///< The batch waits until `due`.
    std::chrono::nanoseconds due;  ///< Due time relative to the frame start (only when paced).

    constexpr bool operator==(const scheduled_batch_t &) const = default;
  };

  /**
   * @brief Walks the batches of one frame in send order, as the video broadcast thread sends them.
   *
   * Batches never span FEC blocks. The packet counters run across the whole frame.
   *
   * @param shards_per_fec_block Shards (data plus FEC) of each FEC block of the frame.
   * @param visit Called with each scheduled_batch_t.
   * @return Packets of the frame; the next frame starts at due_offset() of this count.
   */
  template<typename Visitor>
  constexpr std::size_t for_each_scheduled_batch(const send_schedule_t &schedule, const std::span<const std::size_t> shards_per_fec_block, Visitor &&visit) {
    std::size_t group_packets_sent = 0;
    std::size_t frame_packets_sent = 0;
    for (std::size_t block = 0; block < shards_per_fec_block.size(); ++block) {
      const std::size_t shards = shards_per_fec_block[block];
      std::size_t next_shard_to_send = 0;
      for (std::size_t x = 0; x < shards; ++x) {
        if (x - next_shard_to_send + 1 >= schedule.batch_size || x + 1 == shards) {
          scheduled_batch_t batch {block, x - next_shard_to_send + 1, false, std::chrono::nanoseconds::zero()};
          if (pace_before_batch(schedule, group_packets_sent, frame_packets_sent)) {
            batch.paced = true;
            batch.due = due_offset(schedule, frame_packets_sent);
            group_packets_sent = 0;
          }
          visit(batch);
          group_packets_sent += batch.packets;
          frame_packets_sent += batch.packets;
          next_shard_to_send = x + 1;
        }
      }
    }
    return frame_packets_sent;
  }
}  // namespace stream_pacing
