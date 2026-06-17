#include "../recorder/clip_muxer.hpp"
#include "../recorder/media_probe.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

std::filesystem::path TestDirectory(std::string_view name) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("clipdeck-" + std::string(name) + "-" + std::to_string(getpid()) +
          "-" + std::to_string(suffix));
}

int RunFfmpeg(std::vector<std::string> arguments) {
  const pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    execvp("ffmpeg", argv.data());
    std::_Exit(127);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) != pid) {
    return -1;
  }

  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string RunCommandCapture(std::vector<std::string> arguments) {
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

bool EncoderAvailable(std::string_view encoder) {
  const auto output =
      RunCommandCapture({"ffmpeg", "-hide_banner", "-encoders"});
  return output.find(std::string(encoder)) != std::string::npos;
}

std::vector<std::string> TestVideoEncoderArguments() {
  if (EncoderAvailable("libx264")) {
    return {"-c:v", "libx264", "-preset", "veryfast", "-crf", "23"};
  }

  if (EncoderAvailable("libopenh264")) {
    return {"-c:v", "libopenh264", "-b:v", "500k"};
  }

  return {"-c:v", "mpeg4", "-q:v", "4"};
}

std::filesystem::path CreateTestSegment(const std::filesystem::path &directory,
                                        std::string_view name,
                                        double duration_seconds) {
  const auto path = directory / (std::string(name) + ".mp4");
  const std::string duration = std::to_string(duration_seconds);
  std::vector<std::string> arguments{
      "ffmpeg",
      "-hide_banner",
      "-loglevel",
      "error",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "testsrc2=size=320x180:rate=10:duration=" + duration,
      "-f",
      "lavfi",
      "-i",
      "sine=frequency=440:sample_rate=48000:duration=" + duration,
      "-t",
      duration,
      "-map",
      "0:v:0",
      "-map",
      "1:a:0"};
  const auto video_arguments = TestVideoEncoderArguments();
  arguments.insert(arguments.end(), video_arguments.begin(),
                   video_arguments.end());
  arguments.insert(arguments.end(),
                   {"-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k",
                    "-ar", "48000", "-ac", "2", "-movflags", "+faststart",
                    path.string()});
  const int exit_code = RunFfmpeg(std::move(arguments));

  EXPECT_EQ(exit_code, 0);
  return path;
}

std::filesystem::path CreateBlackSegment(const std::filesystem::path &directory,
                                         std::string_view name,
                                         double duration_seconds) {
  const auto path = directory / (std::string(name) + ".mp4");
  const std::string duration = std::to_string(duration_seconds);
  std::vector<std::string> arguments{
      "ffmpeg",
      "-hide_banner",
      "-loglevel",
      "error",
      "-y",
      "-f",
      "lavfi",
      "-i",
      "color=c=black:size=320x180:rate=10:duration=" + duration,
      "-f",
      "lavfi",
      "-i",
      "sine=frequency=440:sample_rate=48000:duration=" + duration,
      "-t",
      duration,
      "-map",
      "0:v:0",
      "-map",
      "1:a:0"};
  const auto video_arguments = TestVideoEncoderArguments();
  arguments.insert(arguments.end(), video_arguments.begin(),
                   video_arguments.end());
  arguments.insert(arguments.end(),
                   {"-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "96k",
                    "-ar", "48000", "-ac", "2", "-movflags", "+faststart",
                    path.string()});
  const int exit_code = RunFfmpeg(std::move(arguments));

  EXPECT_EQ(exit_code, 0);
  return path;
}

std::filesystem::path
CreateAudioOnlySegment(const std::filesystem::path &directory,
                       std::string_view name, double duration_seconds) {
  const auto path = directory / (std::string(name) + ".mp4");
  const std::string duration = std::to_string(duration_seconds);
  std::vector<std::string> arguments{
      "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
      "-f", "lavfi", "-i",
      "sine=frequency=440:sample_rate=48000:duration=" + duration,
      "-t", duration, "-map", "0:a:0", "-c:a", "aac", "-b:a", "96k",
      "-ar", "48000", "-ac", "2", "-movflags", "+faststart",
      path.string()};
  const int exit_code = RunFfmpeg(std::move(arguments));

  EXPECT_EQ(exit_code, 0);
  return path;
}

clipdeck::ClipMuxerOptions TestOptions(std::chrono::seconds target_duration) {
  return clipdeck::ClipMuxerOptions{.target_duration = target_duration,
                                    .width = 320,
                                    .height = 180,
                                    .fps = 10,
                                    .video_bitrate_kbps = 500,
                                    .audio_bitrate_kbps = 96,
                                    .audio_enabled = true};
}

void ExpectValidClip(const std::filesystem::path &path,
                     const clipdeck::ClipMuxerOptions &options,
                     double expected_duration_seconds) {
  const auto probe = clipdeck::ProbeMediaFile(path);
  ASSERT_TRUE(probe.has_value());
  EXPECT_TRUE(clipdeck::IsUsableFinalClip(probe.value(), options,
                                          expected_duration_seconds))
      << probe->error_message;
  EXPECT_TRUE(probe->has_video);
  EXPECT_TRUE(probe->has_audio);
  EXPECT_EQ(probe->audio_sample_rate, 48000);
  EXPECT_EQ(probe->audio_channels, 2);
  EXPECT_NEAR(probe->duration_seconds, expected_duration_seconds, 0.75);
}

} // namespace

TEST(ClipMuxerTest, RefusesBlankClipWhenNoSegmentsAreAvailable) {
  const auto root = TestDirectory("muxer-empty");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);

  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip =
      muxer.WriteClipFromSegments({}, TestOptions(std::chrono::seconds(2)));

  EXPECT_FALSE(clip.has_value());
  EXPECT_TRUE(std::filesystem::is_empty(clips));

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, ExportsShorterRealCaptureWithoutPadding) {
  const auto root = TestDirectory("muxer-padding");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto real_segment = CreateTestSegment(segments, "real", 3.0);
  const auto options = TestOptions(std::chrono::seconds(30));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip = muxer.WriteClipFromSegments({real_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 3.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, PublishesFullCaptureWithoutPadding) {
  const auto root = TestDirectory("muxer-full");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto real_segment = CreateTestSegment(segments, "real", 5.0);
  const auto options = TestOptions(std::chrono::seconds(5));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip = muxer.WriteClipFromSegments({real_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 5.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, TrimsOldMaterialWhenSegmentsExceedTarget) {
  const auto root = TestDirectory("muxer-trim");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto first_segment = CreateTestSegment(segments, "first", 3.0);
  const auto second_segment = CreateTestSegment(segments, "second", 3.0);
  const auto options = TestOptions(std::chrono::seconds(5));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip =
      muxer.WriteClipFromSegments({first_segment, second_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 5.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, SkipsInvalidSegments) {
  const auto root = TestDirectory("muxer-invalid");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto invalid_segment = segments / "invalid.mp4";
  std::ofstream(invalid_segment) << "not media";
  const auto real_segment = CreateTestSegment(segments, "real", 2.0);
  const auto options = TestOptions(std::chrono::seconds(5));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip =
      muxer.WriteClipFromSegments({invalid_segment, real_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 2.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, PublishesTargetDurationWhenEnoughCaptureExists) {
  const auto root = TestDirectory("muxer-target");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto first_segment = CreateTestSegment(segments, "first", 15.0);
  const auto second_segment = CreateTestSegment(segments, "second", 15.0);
  const auto options = TestOptions(std::chrono::seconds(30));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip =
      muxer.WriteClipFromSegments({first_segment, second_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 30.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, TrimsLatestTargetDurationFromLongCapture) {
  const auto root = TestDirectory("muxer-long-trim");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto first_segment = CreateTestSegment(segments, "first", 15.0);
  const auto second_segment = CreateTestSegment(segments, "second", 15.0);
  const auto third_segment = CreateTestSegment(segments, "third", 15.0);
  const auto options = TestOptions(std::chrono::seconds(30));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip = muxer.WriteClipFromSegments(
      {first_segment, second_segment, third_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 30.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, RejectsMostlyBlackOutput) {
  const auto root = TestDirectory("muxer-black");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto black_segment = CreateBlackSegment(segments, "black", 2.0);
  const auto options = TestOptions(std::chrono::seconds(5));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip = muxer.WriteClipFromSegments({black_segment}, options);

  EXPECT_FALSE(clip.has_value());
  EXPECT_TRUE(std::filesystem::is_empty(clips));
  EXPECT_NE(muxer.LastFailure().find("capture appears black/invalid"),
            std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, SkipsSegmentWithoutVideo) {
  const auto root = TestDirectory("muxer-no-video");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto audio_only_segment = CreateAudioOnlySegment(segments, "audio", 2.0);
  const auto real_segment = CreateTestSegment(segments, "real", 2.0);
  const auto options = TestOptions(std::chrono::seconds(5));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip =
      muxer.WriteClipFromSegments({audio_only_segment, real_segment}, options);

  ASSERT_TRUE(clip.has_value());
  ExpectValidClip(clip.value(), options, 2.0);

  std::error_code error;
  std::filesystem::remove_all(root, error);
}

TEST(ClipMuxerTest, CleansStagingDirectoryAfterFailure) {
  const auto root = TestDirectory("muxer-cleanup");
  const auto clips = root / "clips";
  const auto temp = root / "runtime";
  const auto segments = root / "segments";
  std::filesystem::create_directories(clips);
  std::filesystem::create_directories(temp);
  std::filesystem::create_directories(segments);

  const auto black_segment = CreateBlackSegment(segments, "black", 2.0);
  const auto options = TestOptions(std::chrono::seconds(5));
  clipdeck::ClipMuxer muxer(clips, temp);
  const auto clip = muxer.WriteClipFromSegments({black_segment}, options);

  EXPECT_FALSE(clip.has_value());
  EXPECT_TRUE(std::filesystem::is_empty(temp));

  std::error_code error;
  std::filesystem::remove_all(root, error);
}
