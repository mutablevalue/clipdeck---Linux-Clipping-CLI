#include "../recorder/recorder_backend.hpp"

#include <gtest/gtest.h>

TEST(RecorderBackendTest, DisablesAudioWhenSettingsDisableAudio) {
  clipdeck::ClipDeckSettings settings;
  settings.capture_audio_enabled = false;
  settings.capture_audio_source = std::string(clipdeck::kAutomaticAudioSource);

  const clipdeck::RecorderConfig config =
      clipdeck::BuildRecorderConfig(settings);

  EXPECT_FALSE(config.audio_enabled);
  EXPECT_TRUE(config.audio_source.empty());
}

TEST(RecorderBackendTest, ExcludesDisabledAudioFromMemoryBudget) {
  clipdeck::RecorderConfig config;
  config.clip_length_seconds = 10;
  config.buffer_safety_seconds = 0;
  config.video_bitrate_kbps = 8000;
  config.audio_bitrate_kbps = 2000;
  config.audio_enabled = false;

  EXPECT_EQ(clipdeck::EstimateRecorderMemoryBudgetBytes(config), 12500000);
}

TEST(RecorderBackendTest, LimitsBitrateToConfiguredClipSize) {
  clipdeck::RecorderConfig config;
  config.clip_length_seconds = 30;
  config.video_bitrate_kbps = 12000;
  config.audio_bitrate_kbps = 128;
  config.max_clip_size_mb = 11;

  EXPECT_LT(clipdeck::EffectiveVideoBitrateKbps(config), 2700);
  EXPECT_GT(clipdeck::EffectiveVideoBitrateKbps(config), 2400);
}
