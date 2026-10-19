/**
 * @file src/stream_pacing_policy.h
 * @brief Platform-neutral choice of the video send pacing rate.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace stream_pacing {
  /**
   * @brief Value of the `pacing_mode` config option.
   */
  enum class pacing_mode_e {
    auto_rate,  ///< `auto`: a multiple of the stream bitrate, clamped to a Wi-Fi friendly range.
    legacy,  ///< `legacy`: 80% of 1 Gbps, as in host 1.2.2.
  };

  inline constexpr pacing_mode_e default_pacing_mode = pacing_mode_e::auto_rate;

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

  struct pacing_decision_t {
    std::uint64_t bps;  ///< Send pacing rate in bits per second.
    pacing_source_e source;
    bool configured_below_stream;  ///< pacing_max_bitrate_kbps was raised to 110% of the stream bitrate.

    constexpr bool operator==(const pacing_decision_t &) const = default;
  };

  constexpr int clamp_auto_multiplier_percent(const int multiplier_percent) noexcept {
    return std::clamp(multiplier_percent, min_auto_multiplier_percent, max_auto_multiplier_percent);
  }

  /**
   * @brief 110% of the stream bitrate: the pacer must never be slower than the encoder.
   */
  constexpr std::uint64_t session_floor_bps(const int session_bitrate_kbps) noexcept {
    return static_cast<std::uint64_t>(std::max(session_bitrate_kbps, 0)) * 1000u * 110u / 100u;
  }

  /**
   * @brief Picks the send pacing rate for one frame.
   *
   * An explicit pacing_max_bitrate_kbps always wins, raised to 110% of the stream bitrate
   * when it is lower (the 1.2.2 rule). Otherwise `legacy` returns 800 Mbps, and `auto`
   * returns the stream bitrate times the multiplier, clamped to 300 to 800 Mbps and never
   * below 110% of the stream bitrate.
   *
   * @param configured_kbps pacing_max_bitrate_kbps; 0 means not set.
   * @param mode pacing_mode.
   * @param session_bitrate_kbps The negotiated stream bitrate, which can change mid-session.
   * @param multiplier_percent pacing_auto_multiplier_percent; clamped to 150 to 1000.
   */
  constexpr pacing_decision_t resolve_pacing_bps(
    const int configured_kbps,
    const pacing_mode_e mode,
    const int session_bitrate_kbps,
    const int multiplier_percent
  ) noexcept {
    const std::uint64_t floor_bps = session_floor_bps(session_bitrate_kbps);

    if (configured_kbps > 0) {
      const std::uint64_t configured_bps = static_cast<std::uint64_t>(configured_kbps) * 1000u;
      if (configured_bps < floor_bps) {
        return {floor_bps, pacing_source_e::configured, true};
      }
      return {configured_bps, pacing_source_e::configured, false};
    }

    if (mode == pacing_mode_e::legacy) {
      return {legacy_pacing_bps, pacing_source_e::legacy, false};
    }

    const std::uint64_t stream_bps = static_cast<std::uint64_t>(std::max(session_bitrate_kbps, 0)) * 1000u;
    const std::uint64_t target_bps = stream_bps * static_cast<std::uint64_t>(clamp_auto_multiplier_percent(multiplier_percent)) / 100u;
    const std::uint64_t clamped_bps = std::clamp(target_bps, auto_floor_bps, auto_ceiling_bps);
    return {std::max(clamped_bps, floor_bps), pacing_source_e::auto_rate, false};
  }
}  // namespace stream_pacing
