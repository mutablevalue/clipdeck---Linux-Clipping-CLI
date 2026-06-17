#include "../settings/settings_store.hpp"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

std::filesystem::path TestSettingsPath() {
  return std::filesystem::temp_directory_path() /
         "clipdeck-settings-store-test.conf";
}

} // namespace

TEST(SettingsStoreTest, LoadsDefaultsWhenFileDoesNotExist) {
  const auto settings_path = TestSettingsPath();
  std::filesystem::remove(settings_path);

  const clipdeck::SettingsStore store(settings_path);
  const clipdeck::ClipDeckSettings settings = store.Load();

  EXPECT_EQ(settings.clip_length_seconds, 30);
  EXPECT_EQ(settings.buffer_safety_seconds, 5);
  EXPECT_EQ(settings.save_keybind, "Ctrl+Z+P");
  EXPECT_EQ(settings.clip_directory, settings_path.parent_path() / "clips");
  EXPECT_EQ(settings.capture_video_source, "portal");
  EXPECT_TRUE(settings.capture_audio_enabled);
  EXPECT_EQ(settings.capture_audio_source, clipdeck::kAutomaticAudioSource);
  EXPECT_EQ(settings.capture_width, 1920);
  EXPECT_EQ(settings.capture_height, 1080);
  EXPECT_EQ(settings.capture_fps, 60);
  EXPECT_EQ(settings.video_bitrate_kbps, 12000);
  EXPECT_EQ(settings.audio_bitrate_kbps, 192);
  EXPECT_EQ(settings.encoder, "openh264");
  EXPECT_TRUE(settings.feedback_sound_enabled);
  EXPECT_TRUE(settings.feedback_sound_path.empty());
  EXPECT_DOUBLE_EQ(settings.feedback_sound_volume, 0.5);
}

TEST(SettingsStoreTest, SavesAndLoadsSettings) {
  const auto settings_path = TestSettingsPath();
  std::filesystem::remove(settings_path);

  clipdeck::ClipDeckSettings saved_settings;
  saved_settings.clip_length_seconds = 45;
  saved_settings.buffer_safety_seconds = 8;
  saved_settings.save_keybind = "Ctrl+Alt+P";
  saved_settings.clip_directory = "/tmp/clipdeck-clips";
  saved_settings.capture_video_source = "42";
  saved_settings.capture_audio_enabled = false;
  saved_settings.capture_audio_source = "alsa_output.test.monitor";
  saved_settings.capture_width = 1280;
  saved_settings.capture_height = 720;
  saved_settings.capture_fps = 30;
  saved_settings.video_bitrate_kbps = 8000;
  saved_settings.audio_bitrate_kbps = 160;
  saved_settings.encoder = "x264";
  saved_settings.feedback_sound_enabled = false;
  saved_settings.feedback_sound_path = "/tmp/clipdeck-sound.wav";
  saved_settings.feedback_sound_volume = 0.25;

  const clipdeck::SettingsStore store(settings_path);
  ASSERT_TRUE(store.Save(saved_settings));

  const clipdeck::ClipDeckSettings loaded_settings = store.Load();
  EXPECT_EQ(loaded_settings.clip_length_seconds, 45);
  EXPECT_EQ(loaded_settings.buffer_safety_seconds, 8);
  EXPECT_EQ(loaded_settings.save_keybind, "Ctrl+Alt+P");
  EXPECT_EQ(loaded_settings.clip_directory, "/tmp/clipdeck-clips");
  EXPECT_EQ(loaded_settings.capture_video_source, "portal");
  EXPECT_FALSE(loaded_settings.capture_audio_enabled);
  EXPECT_EQ(loaded_settings.capture_audio_source,
            "alsa_output.test.monitor");
  EXPECT_EQ(loaded_settings.capture_width, 1280);
  EXPECT_EQ(loaded_settings.capture_height, 720);
  EXPECT_EQ(loaded_settings.capture_fps, 30);
  EXPECT_EQ(loaded_settings.video_bitrate_kbps, 8000);
  EXPECT_EQ(loaded_settings.audio_bitrate_kbps, 160);
  EXPECT_EQ(loaded_settings.encoder, "x264");
  EXPECT_FALSE(loaded_settings.feedback_sound_enabled);
  EXPECT_EQ(loaded_settings.feedback_sound_path, "/tmp/clipdeck-sound.wav");
  EXPECT_DOUBLE_EQ(loaded_settings.feedback_sound_volume, 0.25);
}
