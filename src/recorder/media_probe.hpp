#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace clipdeck {

struct ClipMuxerOptions;

struct MediaProbeResult {
  std::filesystem::path path;
  bool valid = false;
  bool has_video = false;
  bool has_audio = false;
  double duration_seconds = 0.0;
  std::optional<double> video_duration_seconds;
  std::optional<double> audio_duration_seconds;
  std::optional<int> audio_sample_rate;
  std::optional<int> audio_channels;
  std::string error_message;
};

struct BlackFrameValidationResult {
  bool valid = false;
  int sampled_frames = 0;
  int black_frames = 0;
  std::string message;
};

[[nodiscard]] std::optional<MediaProbeResult>
ProbeMediaFile(const std::filesystem::path &path);
[[nodiscard]] bool IsUsableRecorderSegment(const MediaProbeResult &probe,
                                           bool audio_expected);
[[nodiscard]] bool IsUsableFinalClip(const MediaProbeResult &probe,
                                     const ClipMuxerOptions &options);
[[nodiscard]] bool IsUsableFinalClip(const MediaProbeResult &probe,
                                     const ClipMuxerOptions &options,
                                     double expected_duration_seconds);
[[nodiscard]] BlackFrameValidationResult
ValidateNotMostlyBlack(const std::filesystem::path &path,
                       double duration_seconds);

} // namespace clipdeck
