/**
 * @file tests/unit/test_qos_policy.cpp
 * @brief Tests for the audio QoS class policy.
 */
#include "../tests_common.h"
#include "src/qos_policy.h"

namespace {
  using stream_qos::audio_qos_e;
  using stream_qos::audio_qos_from_view;
  using stream_qos::audio_qos_plan_t;
  using stream_qos::plan_audio_qos;
  using stream_qos::traffic_type_e;
}  // namespace

TEST(AudioQosPolicy, ParsesTheThreeValues) {
  EXPECT_EQ(audio_qos_from_view("voice"), audio_qos_e::voice);
  EXPECT_EQ(audio_qos_from_view("video"), audio_qos_e::video);
  EXPECT_EQ(audio_qos_from_view("off"), audio_qos_e::off);
}

TEST(AudioQosPolicy, UnknownValuesAreRejectedAndTheDefaultIsThe122Voice) {
  EXPECT_FALSE(audio_qos_from_view("").has_value());
  EXPECT_FALSE(audio_qos_from_view("Voice").has_value());
  EXPECT_FALSE(audio_qos_from_view("audio").has_value());
  EXPECT_EQ(stream_qos::default_audio_qos, audio_qos_e::voice);
  // The default makes the 1.2.2 socket call whether or not the client asks for QoS.
  EXPECT_EQ(plan_audio_qos(stream_qos::default_audio_qos, true), (audio_qos_plan_t {true, traffic_type_e::audio}));
  EXPECT_EQ(plan_audio_qos(stream_qos::default_audio_qos, false), (audio_qos_plan_t {false, traffic_type_e::audio}));
}

TEST(AudioQosPolicy, NamesRoundTrip) {
  for (const auto value : {audio_qos_e::voice, audio_qos_e::video, audio_qos_e::off}) {
    EXPECT_EQ(audio_qos_from_view(stream_qos::to_string(value)), value);
  }
}

TEST(AudioQosPolicy, VoiceKeepsThe122AudioClass) {
  // 1.2.2 called enable_socket_qos(..., qos_data_type_e::audio, audioQosType != 0).
  EXPECT_EQ(plan_audio_qos(audio_qos_e::voice, true), (audio_qos_plan_t {true, traffic_type_e::audio}));
  EXPECT_EQ(plan_audio_qos(audio_qos_e::voice, false), (audio_qos_plan_t {false, traffic_type_e::audio}));
}

TEST(AudioQosPolicy, VideoTagsAudioLikeVideo) {
  EXPECT_EQ(plan_audio_qos(audio_qos_e::video, true), (audio_qos_plan_t {true, traffic_type_e::video}));
}

TEST(AudioQosPolicy, OffSetsNoTag) {
  EXPECT_FALSE(plan_audio_qos(audio_qos_e::off, true).dscp_tagging);
  EXPECT_FALSE(plan_audio_qos(audio_qos_e::off, false).dscp_tagging);
}

TEST(AudioQosPolicy, WithoutAClientRequestNothingIsTaggedAndThe122TypeIsKept) {
  for (const auto value : {audio_qos_e::voice, audio_qos_e::video, audio_qos_e::off}) {
    EXPECT_EQ(plan_audio_qos(value, false), (audio_qos_plan_t {false, traffic_type_e::audio}));
  }
}

TEST(AudioQosPolicy, TrafficTypeNames) {
  EXPECT_EQ(stream_qos::to_string(traffic_type_e::audio), "audio");
  EXPECT_EQ(stream_qos::to_string(traffic_type_e::video), "video");
}
