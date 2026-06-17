#include "media_probe.hpp"

#include "clip_muxer.hpp"
#include "../utils/file_descriptor.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace {

constexpr double kFinalDurationToleranceSeconds = 0.5;
constexpr double kAudioDurationToleranceSeconds = 1.0;
constexpr double kMaxRecorderSegmentDurationSeconds = 300.0;
constexpr int kRawFrameWidth = 16;
constexpr int kRawFrameHeight = 16;
constexpr unsigned char kBlackLumaThreshold = 16;
constexpr double kBlackAverageThreshold = 6.0;
constexpr int kMaxBlackSamples = 7;

struct ProbeCommandResult {
  int exit_code = -1;
  std::string output;
};

struct StreamInfo {
  std::string codec_type;
  std::optional<double> duration_seconds;
  std::optional<int> sample_rate;
  std::optional<int> channels;
};

std::string StripFlatValueQuotes(std::string value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value.erase(value.begin());
    value.pop_back();
  }

  return value;
}

std::string FormatSeconds(double seconds) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3) << seconds;
  return output.str();
}

ProbeCommandResult RunFfprobe(const std::filesystem::path &path) {
  std::array<int, 2> output_pipe{};
  if (pipe(output_pipe.data()) != 0) {
    return {};
  }
  clipdeck::FileDescriptor read_fd(output_pipe[0]);
  clipdeck::FileDescriptor write_fd(output_pipe[1]);

  const pid_t pid = fork();
  if (pid < 0) {
    return {};
  }

  if (pid == 0) {
    read_fd.Reset();
    dup2(write_fd.Get(), STDOUT_FILENO);
    dup2(write_fd.Get(), STDERR_FILENO);
    write_fd.Reset();

    std::array<std::string, 8> arguments{
        "ffprobe",
        "-v",
        "error",
        "-show_entries",
        "stream=codec_type,sample_rate,channels,duration:format=duration",
        "-of",
        "flat",
        path.string(),
    };

    std::array<char *, 9> argv{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      argv[index] = arguments[index].data();
    }

    execvp("ffprobe", argv.data());
    std::_Exit(127);
  }

  write_fd.Reset();
  std::string output;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read =
        read(read_fd.Get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
      continue;
    }

    if (bytes_read == 0 || errno != EINTR) {
      break;
    }
  }
  read_fd.Reset();

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    return ProbeCommandResult{.exit_code = -1, .output = std::move(output)};
  }

  if (WIFEXITED(status)) {
    return ProbeCommandResult{.exit_code = WEXITSTATUS(status),
                              .output = std::move(output)};
  }

  if (WIFSIGNALED(status)) {
    return ProbeCommandResult{.exit_code = 128 + WTERMSIG(status),
                              .output = std::move(output)};
  }

  return ProbeCommandResult{.exit_code = -1, .output = std::move(output)};
}

struct RawFrameResult {
  int exit_code = -1;
  std::vector<unsigned char> bytes;
};

RawFrameResult ReadGrayFrame(const std::filesystem::path &path,
                             double timestamp_seconds) {
  std::array<int, 2> output_pipe{};
  if (pipe(output_pipe.data()) != 0) {
    return {};
  }
  clipdeck::FileDescriptor read_fd(output_pipe[0]);
  clipdeck::FileDescriptor write_fd(output_pipe[1]);

  std::vector<std::string> arguments{
      "ffmpeg",
      "-hide_banner",
      "-loglevel",
      "error",
      "-ss",
      FormatSeconds(std::max(0.0, timestamp_seconds)),
      "-i",
      path.string(),
      "-frames:v",
      "1",
      "-vf",
      "scale=16:16,format=gray",
      "-f",
      "rawvideo",
      "-",
  };

  const pid_t pid = fork();
  if (pid < 0) {
    return {};
  }

  if (pid == 0) {
    read_fd.Reset();
    dup2(write_fd.Get(), STDOUT_FILENO);
    write_fd.Reset();

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    execvp(argv.front(), argv.data());
    std::_Exit(127);
  }

  write_fd.Reset();
  std::vector<unsigned char> bytes;
  bytes.reserve(kRawFrameWidth * kRawFrameHeight);
  std::array<unsigned char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read =
        read(read_fd.Get(), buffer.data(), buffer.size());
    if (bytes_read > 0) {
      bytes.insert(bytes.end(), buffer.begin(),
                   buffer.begin() + bytes_read);
      continue;
    }

    if (bytes_read == 0 || errno != EINTR) {
      break;
    }
  }
  read_fd.Reset();

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    return RawFrameResult{.exit_code = -1, .bytes = std::move(bytes)};
  }

  if (WIFEXITED(status)) {
    return RawFrameResult{.exit_code = WEXITSTATUS(status),
                          .bytes = std::move(bytes)};
  }

  if (WIFSIGNALED(status)) {
    return RawFrameResult{.exit_code = 128 + WTERMSIG(status),
                          .bytes = std::move(bytes)};
  }

  return RawFrameResult{.exit_code = -1, .bytes = std::move(bytes)};
}

std::optional<int> ParseInteger(std::string_view value) {
  int parsed = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    return std::nullopt;
  }

  return parsed;
}

std::optional<double> ParseDouble(std::string_view value) {
  std::string text(value);
  const char *begin = text.c_str();
  char *end = nullptr;
  const double parsed = std::strtod(begin, &end);
  if (begin == end || !std::isfinite(parsed)) {
    return std::nullopt;
  }

  return parsed;
}

bool IsBlackFrame(const std::vector<unsigned char> &bytes) {
  if (bytes.empty()) {
    return false;
  }

  unsigned long long luma_sum = 0;
  int near_black_pixels = 0;
  for (const unsigned char byte : bytes) {
    luma_sum += byte;
    if (byte <= kBlackLumaThreshold) {
      ++near_black_pixels;
    }
  }

  const double average =
      static_cast<double>(luma_sum) / static_cast<double>(bytes.size());
  const double near_black_ratio =
      static_cast<double>(near_black_pixels) / static_cast<double>(bytes.size());
  return average <= kBlackAverageThreshold || near_black_ratio >= 0.98;
}

std::vector<double> SampleTimestamps(double duration_seconds) {
  std::vector<double> samples;
  if (duration_seconds <= 0.0) {
    return samples;
  }

  const int sample_count =
      std::min(kMaxBlackSamples, std::max(3, static_cast<int>(
                                             std::ceil(duration_seconds))));
  samples.reserve(static_cast<std::size_t>(sample_count));

  for (int index = 0; index < sample_count; ++index) {
    const double ratio =
        sample_count == 1 ? 0.0
                          : static_cast<double>(index) /
                                static_cast<double>(sample_count - 1);
    const double timestamp =
        std::clamp(duration_seconds * ratio, 0.0,
                   std::max(0.0, duration_seconds - 0.05));
    if (samples.empty() || std::abs(samples.back() - timestamp) > 0.025) {
      samples.push_back(timestamp);
    }
  }

  return samples;
}

} // namespace

namespace clipdeck {

std::optional<MediaProbeResult>
ProbeMediaFile(const std::filesystem::path &path) {
  MediaProbeResult result{.path = path,
                          .valid = false,
                          .has_video = false,
                          .has_audio = false,
                          .duration_seconds = 0.0,
                          .video_duration_seconds = std::nullopt,
                          .audio_duration_seconds = std::nullopt,
                          .audio_sample_rate = std::nullopt,
                          .audio_channels = std::nullopt,
                          .error_message = ""};

  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    result.error_message = "file does not exist";
    return result;
  }

  if (std::filesystem::file_size(path, error) == 0 || error) {
    result.error_message = "file is empty";
    return result;
  }

  const auto command = RunFfprobe(path);
  if (command.exit_code != 0) {
    result.error_message =
        command.output.empty()
            ? "ffprobe failed with exit code " + std::to_string(command.exit_code)
            : command.output;
    return result;
  }

  std::istringstream input(command.output);
  std::string line;
  std::unordered_map<std::string, StreamInfo> streams;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }

    const std::string key = line.substr(0, separator);
    const std::string value = StripFlatValueQuotes(line.substr(separator + 1));

    if (key == "format.duration") {
      if (const auto duration = ParseDouble(value); duration.has_value()) {
        result.duration_seconds = duration.value();
      }
      continue;
    }

    constexpr std::string_view stream_prefix = "streams.stream.";
    if (!key.starts_with(stream_prefix)) {
      continue;
    }

    const std::size_t field_separator = key.rfind('.');
    if (field_separator == std::string::npos ||
        field_separator <= stream_prefix.size()) {
      continue;
    }

    const std::string stream_key = key.substr(0, field_separator);
    const std::string field = key.substr(field_separator + 1);
    auto &stream = streams[stream_key];
    if (field == "codec_type") {
      stream.codec_type = value;
    } else if (field == "duration") {
      if (const auto duration = ParseDouble(value); duration.has_value()) {
        stream.duration_seconds = duration.value();
      }
    } else if (field == "sample_rate") {
      stream.sample_rate = ParseInteger(value);
    } else if (field == "channels") {
      stream.channels = ParseInteger(value);
    }
  }

  for (const auto &[_, stream] : streams) {
    if (stream.codec_type == "video") {
      result.has_video = true;
      if (!result.video_duration_seconds.has_value() &&
          stream.duration_seconds.has_value()) {
        result.video_duration_seconds = stream.duration_seconds;
      }
      continue;
    }

    if (stream.codec_type == "audio") {
      result.has_audio = true;
      if (!result.audio_duration_seconds.has_value() &&
          stream.duration_seconds.has_value()) {
        result.audio_duration_seconds = stream.duration_seconds;
      }
      if (!result.audio_sample_rate.has_value()) {
        result.audio_sample_rate = stream.sample_rate;
      }
      if (!result.audio_channels.has_value()) {
        result.audio_channels = stream.channels;
      }
    }
  }

  result.valid = result.duration_seconds > 0.0 && result.has_video;
  if (!result.valid) {
    result.error_message = "missing video stream or positive duration";
  }

  return result;
}

bool IsUsableRecorderSegment(const MediaProbeResult &probe,
                             bool audio_expected) {
  if (!probe.valid || !probe.has_video || probe.duration_seconds <= 0.0 ||
      probe.duration_seconds > kMaxRecorderSegmentDurationSeconds) {
    return false;
  }

  if (probe.has_audio && !probe.audio_sample_rate.has_value()) {
    return false;
  }

  if (audio_expected && !probe.has_audio) {
    return false;
  }

  if (audio_expected && probe.audio_sample_rate.value_or(0) <= 0) {
    return false;
  }

  return true;
}

bool IsUsableFinalClip(const MediaProbeResult &probe,
                       const ClipMuxerOptions &options) {
  const double target_seconds =
      static_cast<double>(std::max(options.target_duration.count(), 1L));
  return IsUsableFinalClip(probe, options, target_seconds);
}

bool IsUsableFinalClip(const MediaProbeResult &probe,
                       const ClipMuxerOptions &options,
                       double expected_duration_seconds) {
  if (!probe.valid || !probe.has_video || probe.duration_seconds <= 0.0) {
    return false;
  }

  if (expected_duration_seconds <= 0.0 ||
      std::abs(probe.duration_seconds - expected_duration_seconds) >
      kFinalDurationToleranceSeconds) {
    return false;
  }

  if (!options.audio_enabled) {
    return true;
  }

  if (!probe.has_audio || probe.audio_sample_rate.value_or(0) != 48000) {
    return false;
  }

  if (probe.audio_channels.value_or(0) != 2) {
    return false;
  }

  if (probe.audio_duration_seconds.has_value() &&
      std::abs(probe.audio_duration_seconds.value() - probe.duration_seconds) >
          kAudioDurationToleranceSeconds) {
    return false;
  }

  return true;
}

BlackFrameValidationResult
ValidateNotMostlyBlack(const std::filesystem::path &path,
                       double duration_seconds) {
  const auto samples = SampleTimestamps(duration_seconds);
  if (samples.empty()) {
    return BlackFrameValidationResult{
        .valid = false,
        .sampled_frames = 0,
        .black_frames = 0,
        .message = "duration is not valid for black-frame validation"};
  }

  int sampled_frames = 0;
  int black_frames = 0;
  for (const double timestamp : samples) {
    const auto frame = ReadGrayFrame(path, timestamp);
    constexpr std::size_t expected_frame_size =
        static_cast<std::size_t>(kRawFrameWidth * kRawFrameHeight);
    if (frame.exit_code != 0 || frame.bytes.size() < expected_frame_size) {
      continue;
    }

    ++sampled_frames;
    if (IsBlackFrame(frame.bytes)) {
      ++black_frames;
    }
  }

  if (sampled_frames == 0) {
    return BlackFrameValidationResult{
        .valid = false,
        .sampled_frames = 0,
        .black_frames = 0,
        .message = "could not sample video frames for black-frame validation"};
  }

  const bool mostly_black =
      black_frames == sampled_frames ||
      static_cast<double>(black_frames) / static_cast<double>(sampled_frames) >=
          0.70;
  if (mostly_black) {
    return BlackFrameValidationResult{
        .valid = false,
        .sampled_frames = sampled_frames,
        .black_frames = black_frames,
        .message = "capture appears black/invalid"};
  }

  return BlackFrameValidationResult{.valid = true,
                                    .sampled_frames = sampled_frames,
                                    .black_frames = black_frames,
                                    .message = ""};
}

} // namespace clipdeck
