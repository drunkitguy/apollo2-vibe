/**
 * @file src/qos_policy.h
 * @brief Platform-neutral choice of the QoS class used for stream audio packets.
 */
#pragma once

#include <optional>
#include <string_view>

namespace stream_qos {
  /**
   * @brief Value of the `audio_qos` config option.
   */
  enum class audio_qos_e {
    voice,  ///< Voice class, as in host 1.2.2. Many Wi-Fi access points map it to the WMM voice category.
    video,  ///< The same class as the video stream.
    off,  ///< No DSCP tag on audio packets.
  };

  inline constexpr audio_qos_e default_audio_qos = audio_qos_e::voice;  ///< The 1.2.2 behaviour.

  /**
   * @brief Parses an `audio_qos` value.
   * @return The value, or std::nullopt when it is not one of `voice`, `video` or `off`.
   */
  constexpr std::optional<audio_qos_e> audio_qos_from_view(const std::string_view value) noexcept {
    if (value == "voice") {
      return audio_qos_e::voice;
    }
    if (value == "video") {
      return audio_qos_e::video;
    }
    if (value == "off") {
      return audio_qos_e::off;
    }
    return std::nullopt;
  }

  constexpr std::string_view to_string(const audio_qos_e value) noexcept {
    switch (value) {
      case audio_qos_e::voice:
        return "voice";
      case audio_qos_e::video:
        return "video";
      case audio_qos_e::off:
        return "off";
    }
    return "video";
  }

  /**
   * @brief Traffic type handed to platf::enable_socket_qos() for the audio socket.
   */
  enum class traffic_type_e {
    audio,  ///< platf::qos_data_type_e::audio (qWAVE Voice, DSCP 56; DSCP 48 on Linux).
    video,  ///< platf::qos_data_type_e::video (qWAVE AudioVideo, DSCP 40; DSCP 40 on Linux).
  };

  constexpr std::string_view to_string(const traffic_type_e value) noexcept {
    return value == traffic_type_e::video ? "video" : "audio";
  }

  struct audio_qos_plan_t {
    bool dscp_tagging;  ///< Passed as the dscp_tagging argument.
    traffic_type_e traffic_type;  ///< Passed as the data_type argument.

    constexpr bool operator==(const audio_qos_plan_t &) const = default;
  };

  /**
   * @brief Decides how the audio socket is tagged.
   *
   * The client still decides whether any QoS applies: without its request, and with
   * `audio_qos = off`, the socket is set up exactly like a 1.2.2 session whose client
   * asked for no QoS.
   *
   * @param mode The configured `audio_qos` value.
   * @param client_requested Whether the client asked for audio QoS (audioQosType != 0).
   */
  constexpr audio_qos_plan_t plan_audio_qos(const audio_qos_e mode, const bool client_requested) noexcept {
    if (!client_requested || mode == audio_qos_e::off) {
      return {false, traffic_type_e::audio};
    }
    if (mode == audio_qos_e::video) {
      return {true, traffic_type_e::video};
    }
    return {true, traffic_type_e::audio};
  }
}  // namespace stream_qos
