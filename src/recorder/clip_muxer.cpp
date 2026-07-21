#include "clip_muxer.hpp"

#include "../utils/app_error.hpp"
#include "../utils/logger.hpp"
#include "../utils/runtime_paths.hpp"
#include "media_probe.hpp"
#include "segment_file.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kMuxerContext = "clip-muxer";

std::string TimestampForFilename() {
  const auto now = std::chrono::floor<std::chrono::seconds>(
      std::chrono::system_clock::now());
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()) %
      std::chrono::seconds(1);
  return std::format("{:%Y%m%d-%H%M%S}-{:03}", now, milliseconds.count());
}

std::string EscapeConcatPath(const std::filesystem::path &path) {
  std::string escaped;
  for (const char character : path.string()) {
    if (character == '\'') {
      escaped += "'\\''";
      continue;
    }

    escaped.push_back(character);
  }

  return escaped;
}

std::string FormatSeconds(double seconds) {
  std::ostringstream output;
  output << std::fixed << std::setprecision(3) << seconds;
  return output.str();
}

bool TrustedFinalClipLooksUsable(const clipdeck::MediaProbeResult &probe,
                                 const clipdeck::ClipMuxerOptions &options,
                                 double expected_duration_seconds) {
  if (!probe.valid || !probe.has_video || probe.duration_seconds <= 0.0) {
    return false;
  }

  const double tolerance = expected_duration_seconds >= 5.0 ? 1.0 : 0.5;
  if (probe.duration_seconds + tolerance < expected_duration_seconds ||
      probe.duration_seconds - expected_duration_seconds > tolerance) {
    return false;
  }

  if (!options.audio_enabled) {
    return true;
  }

  return probe.has_audio && probe.audio_sample_rate.value_or(0) == 48000 &&
         probe.audio_channels.value_or(0) == 2;
}

bool FinalClipPassesValidation(
    const std::optional<clipdeck::MediaProbeResult> &probe,
    const clipdeck::ClipMuxerOptions &options,
    double expected_duration_seconds) {
  if (!probe.has_value()) {
    return false;
  }

  if (options.max_output_bytes.has_value()) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(probe->path, size_error);
    if (size_error || size > options.max_output_bytes.value()) {
      return false;
    }
  }

  if (options.trust_recorder_segments) {
    return TrustedFinalClipLooksUsable(probe.value(), options,
                                       expected_duration_seconds);
  }

  return clipdeck::IsUsableFinalClip(probe.value(), options,
                                     expected_duration_seconds);
}

std::string FinalClipValidationFailure(
    const std::optional<clipdeck::MediaProbeResult> &probe,
    const clipdeck::ClipMuxerOptions &options,
    double expected_duration_seconds) {
  if (!probe.has_value()) {
    return "ffprobe did not return a result";
  }

  if (options.max_output_bytes.has_value()) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(probe->path, size_error);
    if (size_error) {
      return "output size could not be read";
    }
    if (size > options.max_output_bytes.value()) {
      return "output size " + std::to_string(size) + " bytes exceeds limit " +
             std::to_string(options.max_output_bytes.value()) + " bytes";
    }
  }

  if (!probe->valid || !probe->has_video || probe->duration_seconds <= 0.0) {
    return probe->error_message.empty()
               ? "missing video stream or positive duration"
               : probe->error_message;
  }

  if (std::abs(probe->duration_seconds - expected_duration_seconds) > 0.5) {
    return "duration " + FormatSeconds(probe->duration_seconds) +
           "s is outside tolerance for expected export duration " +
           FormatSeconds(expected_duration_seconds) + "s";
  }

  if (options.audio_enabled && !probe->has_audio) {
    return "audio was expected but no audio stream was found";
  }

  if (options.audio_enabled && probe->audio_sample_rate.value_or(0) != 48000) {
    return "audio sample rate is " +
           (probe->audio_sample_rate.has_value()
                ? std::to_string(probe->audio_sample_rate.value())
                : std::string("unknown")) +
           ", expected 48000";
  }

  if (options.audio_enabled && probe->audio_channels.value_or(0) != 2) {
    return "audio channel count is " +
           (probe->audio_channels.has_value()
                ? std::to_string(probe->audio_channels.value())
                : std::string("unknown")) +
           ", expected 2";
  }

  return "unknown validation failure";
}

class ScopedDirectoryCleanup {
public:
  explicit ScopedDirectoryCleanup(std::filesystem::path path)
      : path_(std::move(path)) {}

  ~ScopedDirectoryCleanup() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  ScopedDirectoryCleanup(const ScopedDirectoryCleanup &) = delete;
  ScopedDirectoryCleanup &operator=(const ScopedDirectoryCleanup &) = delete;

private:
  std::filesystem::path path_;
};

int ExitCodeFromWaitStatus(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }

  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }

  return -1;
}

void SignalProcessGroupOrProcess(pid_t pid, int signal) {
  if (kill(-pid, signal) != 0) {
    kill(pid, signal);
  }
}

int WaitForProcessWithTimeout(pid_t pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;

  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      return ExitCodeFromWaitStatus(status);
    }

    if (result < 0 && errno != EINTR) {
      return -1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  SignalProcessGroupOrProcess(pid, SIGTERM);
  const auto terminate_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < terminate_deadline) {
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      return 124;
    }

    if (result < 0 && errno != EINTR) {
      return 124;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  SignalProcessGroupOrProcess(pid, SIGKILL);
  waitpid(pid, &status, 0);
  return 124;
}

std::chrono::milliseconds ComposeTimeout(double export_duration_seconds,
                                         bool stream_copy) {
  const int seconds =
      stream_copy
          ? std::max(20,
                     static_cast<int>(std::ceil(export_duration_seconds * 2.0)))
          : std::max(
                60, static_cast<int>(std::ceil(export_duration_seconds * 4.0)));
  return std::chrono::seconds(seconds);
}

int RunCommand(std::vector<std::string> arguments,
               std::chrono::milliseconds timeout) {
  if (arguments.empty()) {
    return -1;
  }

  const pid_t pid = fork();

  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    setpgid(0, 0);
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    execvp("ffmpeg", argv.data());
    std::_Exit(127);
  }

  setpgid(pid, pid);
  return WaitForProcessWithTimeout(pid, timeout);
}

std::string RunCommandCapture(std::vector<std::string> arguments) {
  if (arguments.empty()) {
    return {};
  }

  std::array<int, 2> output_pipe{};
  if (pipe(output_pipe.data()) != 0) {
    return {};
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    return {};
  }

  if (pid == 0) {
    close(output_pipe[0]);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(output_pipe[1]);

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    execvp(argv.front(), argv.data());
    std::_Exit(127);
  }

  close(output_pipe[1]);
  std::string output;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t bytes_read =
        read(output_pipe[0], buffer.data(), buffer.size());
    if (bytes_read > 0) {
      output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
      continue;
    }

    if (bytes_read == 0 || errno != EINTR) {
      break;
    }
  }
  close(output_pipe[0]);

  int status = 0;
  waitpid(pid, &status, 0);
  return output;
}

bool FfmpegEncoderAvailableUncached(std::string_view encoder) {
  const auto output =
      RunCommandCapture({"ffmpeg", "-hide_banner", "-encoders"});
  return output.find(std::string(encoder)) != std::string::npos;
}

std::vector<std::string>
VideoEncoderArguments(const clipdeck::ClipMuxerOptions &options) {
  static const bool libx264_available =
      FfmpegEncoderAvailableUncached("libx264");
  static const bool libopenh264_available =
      FfmpegEncoderAvailableUncached("libopenh264");

  if (libx264_available) {
    const auto bitrate = std::to_string(options.video_bitrate_kbps) + "k";
    return {"-c:v",     "libx264",
            "-preset",  "veryfast",
            "-b:v",     bitrate,
            "-maxrate", bitrate,
            "-bufsize", std::to_string(options.video_bitrate_kbps * 2) + "k"};
  }

  if (libopenh264_available) {
    return {"-c:v", "libopenh264", "-b:v",
            std::to_string(options.video_bitrate_kbps) + "k"};
  }

  return {"-c:v", "mpeg4", "-q:v", "4"};
}

void AppendVideoEncoderArguments(std::vector<std::string> &arguments,
                                 const clipdeck::ClipMuxerOptions &options) {
  for (auto argument : VideoEncoderArguments(options)) {
    arguments.push_back(std::move(argument));
  }
  arguments.emplace_back("-pix_fmt");
  arguments.emplace_back("yuv420p");
}

void AppendAudioEncoderArguments(std::vector<std::string> &arguments,
                                 const clipdeck::ClipMuxerOptions &options) {
  arguments.emplace_back("-c:a");
  arguments.emplace_back("aac");
  arguments.emplace_back("-b:a");
  arguments.emplace_back(std::to_string(options.audio_bitrate_kbps) + "k");
}

int RunFfmpegCompose(const std::filesystem::path &manifest_path,
                     const std::filesystem::path &output_path,
                     double trim_start_seconds, double export_duration_seconds,
                     const clipdeck::ClipMuxerOptions &options) {
  std::vector<std::string> arguments{"ffmpeg",
                                     "-hide_banner",
                                     "-loglevel",
                                     "error",
                                     "-y",
                                     "-fflags",
                                     "+genpts",
                                     "-f",
                                     "concat",
                                     "-safe",
                                     "0",
                                     "-i",
                                     manifest_path.string()};

  if (trim_start_seconds > 0.001) {
    arguments.insert(arguments.end(),
                     {"-ss", FormatSeconds(trim_start_seconds)});
  }

  arguments.insert(arguments.end(),
                   {"-t", FormatSeconds(export_duration_seconds), "-map",
                    "0:v:0", "-map", "0:a:0?"});

  AppendVideoEncoderArguments(arguments, options);
  if (options.audio_enabled) {
    AppendAudioEncoderArguments(arguments, options);
    arguments.emplace_back("-ar");
    arguments.emplace_back("48000");
    arguments.emplace_back("-ac");
    arguments.emplace_back("2");
  } else {
    arguments.insert(arguments.end(), {"-an"});
  }

  arguments.insert(arguments.end(),
                   {"-avoid_negative_ts", "make_zero", "-movflags",
                    "+faststart", output_path.string()});

  return RunCommand(std::move(arguments),
                    ComposeTimeout(export_duration_seconds, false));
}

int RunFfmpegComposeCopy(const std::filesystem::path &manifest_path,
                         const std::filesystem::path &output_path,
                         double trim_start_seconds,
                         double export_duration_seconds,
                         const clipdeck::ClipMuxerOptions &options) {
  (void)options;
  std::vector<std::string> arguments{"ffmpeg",
                                     "-hide_banner",
                                     "-loglevel",
                                     "error",
                                     "-y",
                                     "-fflags",
                                     "+genpts",
                                     "-f",
                                     "concat",
                                     "-safe",
                                     "0",
                                     "-i",
                                     manifest_path.string()};

  if (trim_start_seconds > 0.001) {
    arguments.insert(arguments.end(),
                     {"-ss", FormatSeconds(trim_start_seconds)});
  }

  arguments.insert(arguments.end(),
                   {"-t", FormatSeconds(export_duration_seconds), "-map",
                    "0:v:0", "-map", "0:a:0?", "-c", "copy",
                    "-avoid_negative_ts", "make_zero", "-movflags",
                    "+faststart", output_path.string()});

  return RunCommand(std::move(arguments),
                    ComposeTimeout(export_duration_seconds, true));
}

struct StagedSegments {
  std::vector<std::filesystem::path> paths;
  double duration_seconds = 0.0;
};

bool TrustedRecorderSegmentLooksUsable(const std::filesystem::path &segment) {
  std::error_code error;
  return std::filesystem::is_regular_file(segment, error) && !error &&
         segment.extension() == ".mp4" &&
         std::filesystem::file_size(segment, error) > 0 && !error &&
         clipdeck::IsFinalizedMp4Segment(segment);
}

bool IsStaleStagingDirectory(const std::filesystem::directory_entry &entry) {
  std::error_code error;
  if (!entry.is_directory(error) || error) {
    return false;
  }

  return entry.path().filename().string().starts_with("clipdeck-compose-");
}

void CleanStaleStagingDirectories(const std::filesystem::path &temp_directory) {
  std::error_code error;
  if (!std::filesystem::exists(temp_directory, error)) {
    return;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(temp_directory, error)) {
    if (error) {
      break;
    }

    if (!IsStaleStagingDirectory(entry)) {
      continue;
    }

    std::error_code remove_error;
    std::filesystem::remove_all(entry.path(), remove_error);
  }
}

StagedSegments StageSegments(const std::vector<std::filesystem::path> &segments,
                             const std::filesystem::path &staging_directory,
                             const clipdeck::ClipMuxerOptions &options) {
  StagedSegments staged;
  int index = 0;

  for (const auto &segment : segments) {
    if (options.trust_recorder_segments &&
        !TrustedRecorderSegmentLooksUsable(segment)) {
      Log(LogLevel::Debug, kMuxerContext,
          "Skipping non-finalized recorder segment before staging: " +
              segment.string());
      continue;
    }

    if (!options.trust_recorder_segments) {
      const auto source_probe = clipdeck::ProbeMediaFile(segment);
      if (!source_probe.has_value() ||
          !clipdeck::IsUsableRecorderSegment(source_probe.value(),
                                             options.audio_enabled)) {
        Log(LogLevel::Debug, kMuxerContext,
            "Skipping unusable segment before staging: " + segment.string());
        continue;
      }
    }

    const auto staged_path =
        staging_directory / std::format("segment-{:05}.mp4", index++);
    std::error_code copy_error;
    std::filesystem::copy_file(
        segment, staged_path, std::filesystem::copy_options::overwrite_existing,
        copy_error);
    if (copy_error) {
      Log(LogLevel::Warning, kMuxerContext,
          "Failed to stage segment " + segment.string() + ": " +
              copy_error.message());
      continue;
    }

    staged.paths.push_back(staged_path);
    if (!options.trust_recorder_segments) {
      const auto staged_probe = clipdeck::ProbeMediaFile(staged_path);
      if (!staged_probe.has_value() ||
          !clipdeck::IsUsableRecorderSegment(staged_probe.value(),
                                             options.audio_enabled)) {
        Log(LogLevel::Warning, kMuxerContext,
            "Staged segment is not usable: " + staged_path.string());
        staged.paths.pop_back();
        std::filesystem::remove(staged_path, copy_error);
        continue;
      }

      staged.duration_seconds += staged_probe->duration_seconds;
    }
  }

  if (options.trust_recorder_segments) {
    staged.duration_seconds = options.available_duration_seconds.value_or(
        static_cast<double>(staged.paths.size()));
  }

  return staged;
}

} // namespace

namespace clipdeck {

ClipMuxer::ClipMuxer(std::filesystem::path clip_directory)
    : ClipMuxer(std::move(clip_directory),
                RuntimeDirectory() / "clip-compose") {}

ClipMuxer::ClipMuxer(std::filesystem::path clip_directory,
                     std::filesystem::path temp_directory)
    : clip_directory_(std::move(clip_directory)),
      temp_directory_(std::move(temp_directory)) {}

std::optional<std::filesystem::path> ClipMuxer::WriteClipFromSegments(
    const std::vector<std::filesystem::path> &segments) const {
  return WriteClipFromSegments(segments, ClipMuxerOptions{});
}

std::optional<std::filesystem::path> ClipMuxer::WriteClipFromSegments(
    const std::vector<std::filesystem::path> &segments,
    const ClipMuxerOptions &options) const {
  SetLastFailure("");
  const double target_seconds =
      static_cast<double>(std::max(options.target_duration.count(), 1L));

  std::error_code error;
  std::filesystem::create_directories(clip_directory_, error);

  if (error) {
    const std::string message =
        "Failed to create clip directory: " + error.message();
    SetLastFailure(message);
    HandleError(MakeError("clip_directory", kMuxerContext, message));
    return std::nullopt;
  }

  std::filesystem::create_directories(temp_directory_, error);
  if (error) {
    const std::string message =
        "Failed to create clip temp directory: " + error.message();
    SetLastFailure(message);
    HandleError(MakeError("clip_temp_directory", kMuxerContext, message));
    return std::nullopt;
  }
  CleanStaleStagingDirectories(temp_directory_);

  const auto staging_directory = BuildStagingDirectory();
  std::filesystem::create_directories(staging_directory, error);
  if (error) {
    const std::string message =
        "Failed to create clip staging directory: " + error.message();
    SetLastFailure(message);
    HandleError(MakeError("clip_staging_directory", kMuxerContext, message));
    return std::nullopt;
  }
  ScopedDirectoryCleanup cleanup_staging(staging_directory);

  auto staged = StageSegments(segments, staging_directory, options);
  if (staged.paths.empty()) {
    const std::string message = "No finalized recorder segments are available "
                                "yet; refusing to publish a blank clip.";
    SetLastFailure(message);
    Log(LogLevel::Warning, kMuxerContext, message);
    return std::nullopt;
  }

  const double available_duration_seconds =
      std::isfinite(staged.duration_seconds) && staged.duration_seconds > 0.0
          ? staged.duration_seconds
          : static_cast<double>(staged.paths.size());
  const double usable_duration_seconds =
      std::max(0.0, available_duration_seconds -
                        std::max(options.end_trim_seconds, 0.0));
  const double export_duration_seconds =
      std::min(usable_duration_seconds, target_seconds);
  if (export_duration_seconds <= 0.001) {
    const std::string message = "No captured duration remains after trimming.";
    SetLastFailure(message);
    return std::nullopt;
  }
  const double trim_seconds =
      std::max(0.0, usable_duration_seconds - export_duration_seconds);
  const bool shortened = usable_duration_seconds + 0.001 < target_seconds;
  Log(LogLevel::Info, kMuxerContext,
      "Preparing clip: requested_duration=" + FormatSeconds(target_seconds) +
          "s, real_available_duration=" +
          FormatSeconds(available_duration_seconds) +
          "s, final_exported_duration=" +
          FormatSeconds(export_duration_seconds) +
          "s, selected_segment_count=" + std::to_string(staged.paths.size()) +
          ", shortened=" + (shortened ? "true" : "false") +
          ", trim_duration=" + FormatSeconds(trim_seconds) + "s.");

  const auto manifest_path = staging_directory / "concat.txt";
  const auto temp_output_path = staging_directory / "clipdeck-output.mp4";
  const auto clip_path = BuildClipPath();

  std::ofstream manifest(manifest_path, std::ios::trunc);
  if (!manifest.is_open()) {
    const std::string message =
        "Failed to open concat manifest: " + manifest_path.string();
    SetLastFailure(message);
    HandleError(MakeError("clip_manifest", kMuxerContext, message));
    return std::nullopt;
  }

  for (const auto &segment : staged.paths) {
    manifest << "file '" << EscapeConcatPath(std::filesystem::absolute(segment))
             << "'\n";
  }
  manifest.close();

  const double trim_start_seconds =
      std::max(0.0, usable_duration_seconds - export_duration_seconds);
  const bool can_try_stream_copy = true;
  int exit_code = -1;
  if (can_try_stream_copy) {
    Log(LogLevel::Info, kMuxerContext,
        "Composing clip with stream-copy fast path.");
    exit_code = RunFfmpegComposeCopy(manifest_path, temp_output_path,
                                     trim_start_seconds,
                                     export_duration_seconds, options);
    const auto copy_probe = ProbeMediaFile(temp_output_path);
    if (exit_code == 0 && FinalClipPassesValidation(copy_probe, options,
                                                    export_duration_seconds)) {
      if (!options.validate_black_frames) {
        Log(LogLevel::Info, kMuxerContext,
            "Stream-copy compose passed validation.");
      } else {
        const auto black_validation = ValidateNotMostlyBlack(
            temp_output_path, copy_probe->duration_seconds);
        if (black_validation.valid) {
          Log(LogLevel::Info, kMuxerContext,
              "Stream-copy compose passed validation.");
        } else {
          Log(LogLevel::Warning, kMuxerContext,
              "Stream-copy compose produced invalid video; falling back to "
              "normalized re-encode: " +
                  black_validation.message);
          std::error_code remove_error;
          std::filesystem::remove(temp_output_path, remove_error);
          exit_code = RunFfmpegCompose(manifest_path, temp_output_path,
                                       trim_start_seconds,
                                       export_duration_seconds, options);
        }
      }
    } else {
      std::error_code remove_error;
      std::filesystem::remove(temp_output_path, remove_error);
      Log(LogLevel::Warning, kMuxerContext,
          "Stream-copy compose did not validate; falling back to normalized "
          "re-encode.");
      exit_code =
          RunFfmpegCompose(manifest_path, temp_output_path, trim_start_seconds,
                           export_duration_seconds, options);
    }
  } else {
    exit_code =
        RunFfmpegCompose(manifest_path, temp_output_path, trim_start_seconds,
                         export_duration_seconds, options);
  }

  if (exit_code != 0) {
    const std::string message = "ffmpeg failed to compose clip, exit code " +
                                std::to_string(exit_code) + ".";
    SetLastFailure(message);
    HandleError(MakeError("clip_compose", kMuxerContext, message));
    return std::nullopt;
  }

  const auto final_probe = ProbeMediaFile(temp_output_path);
  if (!FinalClipPassesValidation(final_probe, options,
                                 export_duration_seconds)) {
    const std::string reason = FinalClipValidationFailure(
        final_probe, options, export_duration_seconds);
    const std::string message = "Composed clip failed validation: " + reason;
    SetLastFailure(message);
    HandleError(MakeError("clip_empty", kMuxerContext, message));
    std::error_code status_error;
    std::filesystem::remove(temp_output_path, status_error);
    return std::nullopt;
  }

  if (options.validate_black_frames) {
    const auto black_validation =
        ValidateNotMostlyBlack(temp_output_path, final_probe->duration_seconds);
    if (!black_validation.valid) {
      const std::string message =
          "Composed clip failed validation: " + black_validation.message +
          " (sampled_frames=" +
          std::to_string(black_validation.sampled_frames) +
          ", black_frames=" + std::to_string(black_validation.black_frames) +
          ").";
      SetLastFailure(message);
      HandleError(MakeError("clip_black", kMuxerContext, message));
      std::error_code status_error;
      std::filesystem::remove(temp_output_path, status_error);
      return std::nullopt;
    }
  }

  std::error_code status_error;
  std::filesystem::rename(temp_output_path, clip_path, error);
  if (error) {
    error.clear();
    std::filesystem::copy_file(
        temp_output_path, clip_path,
        std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      const std::string message =
          "Failed to publish final clip: " + error.message();
      SetLastFailure(message);
      HandleError(MakeError("clip_publish", kMuxerContext, message));
      return std::nullopt;
    }
    std::filesystem::remove(temp_output_path, status_error);
  }

  Log(LogLevel::Info, kMuxerContext,
      "Published validated clip: " + clip_path.string() + ".");
  return clip_path;
}

std::string ClipMuxer::LastFailure() const { return last_failure_; }

std::filesystem::path ClipMuxer::BuildClipPath() const {
  return clip_directory_ / ("clipdeck-" + TimestampForFilename() + ".mp4");
}

std::filesystem::path ClipMuxer::BuildStagingDirectory() const {
  return temp_directory_ / ("clipdeck-compose-" + TimestampForFilename() + "-" +
                            std::to_string(getpid()));
}

void ClipMuxer::SetLastFailure(std::string failure) const {
  last_failure_ = std::move(failure);
}

} // namespace clipdeck
