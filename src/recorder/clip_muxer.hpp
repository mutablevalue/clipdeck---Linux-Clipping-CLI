#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace clipdeck {

struct ClipMuxerOptions {
  std::chrono::seconds target_duration{30};
  int width = 1920;
  int height = 1080;
  int fps = 60;
  int video_bitrate_kbps = 8000;
  int audio_bitrate_kbps = 192;
  bool audio_enabled = true;
  bool trust_recorder_segments = false;
  std::optional<double> available_duration_seconds;
  bool validate_black_frames = true;
};

class ClipMuxer {
public:
  explicit ClipMuxer(std::filesystem::path clip_directory);
  ClipMuxer(std::filesystem::path clip_directory,
            std::filesystem::path temp_directory);

  [[nodiscard]] std::optional<std::filesystem::path>
  WriteClipFromSegments(const std::vector<std::filesystem::path> &segments) const;
  [[nodiscard]] std::optional<std::filesystem::path>
  WriteClipFromSegments(const std::vector<std::filesystem::path> &segments,
                        const ClipMuxerOptions &options) const;
  [[nodiscard]] std::string LastFailure() const;

private:
  [[nodiscard]] std::filesystem::path BuildClipPath() const;
  [[nodiscard]] std::filesystem::path BuildStagingDirectory() const;
  void SetLastFailure(std::string failure) const;

  std::filesystem::path clip_directory_;
  std::filesystem::path temp_directory_;
  mutable std::string last_failure_;
};

} // namespace clipdeck
