#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace clipdeck {

inline constexpr std::string_view kAutomaticAudioSource = "auto";

struct ClipDeckSettings {
  int settings_version = 2;
  int clip_length_seconds = 30;
  int buffer_safety_seconds = 5;
  std::string save_keybind = "Ctrl+Z+P";
  std::string stop_keybind = "Ctrl+Z+O";
  std::filesystem::path clip_directory = "output/clips";
  std::string capture_video_source = "portal";
  bool capture_audio_enabled = true;
  std::string capture_audio_source = std::string(kAutomaticAudioSource);
  int capture_width = 1920;
  int capture_height = 1080;
  int capture_fps = 60;
  int video_bitrate_kbps = 2500;
  int audio_bitrate_kbps = 128;
  int max_clip_size_mb = 11;
  std::string encoder = "auto";
  bool feedback_sound_enabled = true;
  std::filesystem::path feedback_sound_path;
  double feedback_sound_volume = 0.5;
};

class SettingsStore {
public:
  SettingsStore();
  explicit SettingsStore(std::filesystem::path settings_path);

  [[nodiscard]] ClipDeckSettings Load() const;
  bool Save(const ClipDeckSettings &settings) const;
  [[nodiscard]] const std::filesystem::path &Path() const;

private:
  std::filesystem::path settings_path_;
};

} // namespace clipdeck
