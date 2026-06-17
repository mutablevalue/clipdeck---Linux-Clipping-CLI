#pragma once

#include "../settings/settings_store.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

namespace clipdeck {

struct RecorderStatus {
  bool running = false;
  bool healthy = false;
  std::string backend = "native";
  std::string message;
  std::chrono::milliseconds buffered_duration{0};
  std::size_t buffered_bytes = 0;
  std::size_t memory_budget_bytes = 0;
  std::size_t finalized_segment_count = 0;
  std::chrono::milliseconds finalized_segment_duration{0};
  bool can_save_any_clip = false;
  bool can_save_full_clip_without_padding = false;
  std::string last_capture_anomaly;
  std::string last_save_failure;
  std::filesystem::path last_saved_clip;
  std::optional<int> audio_sample_rate;
  std::optional<int> audio_channels;
  bool audio_enabled = false;
};

struct RecorderConfig {
  std::string video_source;
  bool audio_enabled = true;
  std::string audio_source;
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int clip_length_seconds = 30;
  int buffer_safety_seconds = 5;
  int video_bitrate_kbps = 12000;
  int audio_bitrate_kbps = 192;
  std::string encoder = "openh264";
  std::filesystem::path clip_directory = "output/clips";
};

class RecorderBackend {
public:
  virtual ~RecorderBackend() = default;

  virtual bool Start() = 0;
  virtual void Stop() = 0;
  virtual bool SaveClip() = 0;
  [[nodiscard]] virtual RecorderStatus Status() const = 0;
};

[[nodiscard]] RecorderConfig BuildRecorderConfig(
    const ClipDeckSettings &settings);
[[nodiscard]] std::size_t EstimateRecorderMemoryBudgetBytes(
    const RecorderConfig &config);

} // namespace clipdeck
