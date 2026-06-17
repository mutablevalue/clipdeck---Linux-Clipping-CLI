#include "feedback_notifier.hpp"

#include "utils/logger.hpp"
#include "utils/runtime_paths.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ranges>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::string_view kFeedbackContext = "feedback";
constexpr auto kFeedbackRateLimit = std::chrono::milliseconds(750);
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kBitsPerSample = 16;
constexpr double kDurationSeconds = 0.18;
constexpr double kPi = 3.14159265358979323846;

void WriteLittleEndian16(std::ostream &output, std::uint16_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
}

void WriteLittleEndian32(std::ostream &output, std::uint32_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
  output.put(static_cast<char>((value >> 16) & 0xff));
  output.put(static_cast<char>((value >> 24) & 0xff));
}

bool GenerateDefaultFeedbackSound(const std::filesystem::path &path,
                                  double volume) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }

  const int sample_count = static_cast<int>(kSampleRate * kDurationSeconds);
  const auto data_bytes = static_cast<std::uint32_t>(
      sample_count * kChannels * (kBitsPerSample / 8));
  const auto riff_size = static_cast<std::uint32_t>(36 + data_bytes);
  const auto byte_rate = static_cast<std::uint32_t>(
      kSampleRate * kChannels * (kBitsPerSample / 8));
  const auto block_align =
      static_cast<std::uint16_t>(kChannels * (kBitsPerSample / 8));

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    return false;
  }

  output.write("RIFF", 4);
  WriteLittleEndian32(output, riff_size);
  output.write("WAVE", 4);
  output.write("fmt ", 4);
  WriteLittleEndian32(output, 16);
  WriteLittleEndian16(output, 1);
  WriteLittleEndian16(output, kChannels);
  WriteLittleEndian32(output, kSampleRate);
  WriteLittleEndian32(output, byte_rate);
  WriteLittleEndian16(output, block_align);
  WriteLittleEndian16(output, kBitsPerSample);
  output.write("data", 4);
  WriteLittleEndian32(output, data_bytes);

  const double amplitude =
      std::clamp(volume, 0.0, 1.0) * 0.22 * static_cast<double>(INT16_MAX);
  for (int sample = 0; sample < sample_count; ++sample) {
    const double t = static_cast<double>(sample) / kSampleRate;
    const double envelope = t < 0.012 ? t / 0.012
                                      : std::exp(-(t - 0.012) * 22.0);
    const double first = std::sin(2.0 * kPi * 880.0 * t);
    const double second =
        t > 0.045 ? std::sin(2.0 * kPi * 1320.0 * (t - 0.045)) : 0.0;
    const double mixed = (first * 0.72 + second * 0.45) * envelope;
    const auto pcm = static_cast<std::int16_t>(
        std::clamp(mixed * amplitude,
                   -static_cast<double>(INT16_MAX),
                   static_cast<double>(INT16_MAX)));
    WriteLittleEndian16(output, static_cast<std::uint16_t>(pcm));
    WriteLittleEndian16(output, static_cast<std::uint16_t>(pcm));
  }

  return output.good();
}

std::vector<std::filesystem::path> PathEntries() {
  std::vector<std::filesystem::path> entries;
  const char *path = std::getenv("PATH");
  if (path == nullptr) {
    return entries;
  }

  std::stringstream stream(path);
  std::string entry;
  while (std::getline(stream, entry, ':')) {
    if (!entry.empty()) {
      entries.emplace_back(entry);
    }
  }

  return entries;
}

std::optional<std::string> FindExecutable(std::string_view name) {
  for (const auto &entry : PathEntries()) {
    const auto candidate = entry / name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }

  return std::nullopt;
}

std::optional<std::string> FindAudioPlayer() {
  for (std::string_view name : {"paplay", "pw-play", "aplay"}) {
    if (auto executable = FindExecutable(name); executable.has_value()) {
      return executable;
    }
  }

  return std::nullopt;
}

void PlaySoundAsync(std::string player, std::filesystem::path sound_path) {
  std::thread([player = std::move(player), sound_path = std::move(sound_path)] {
    const pid_t pid = fork();
    if (pid < 0) {
      return;
    }

    if (pid == 0) {
      const int null_descriptor = open("/dev/null", O_RDWR);
      if (null_descriptor >= 0) {
        dup2(null_descriptor, STDOUT_FILENO);
        dup2(null_descriptor, STDERR_FILENO);
        close(null_descriptor);
      }

      std::array<std::string, 2> arguments{player, sound_path.string()};
      std::array<char *, 3> argv{};
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        argv[index] = arguments[index].data();
      }

      execv(player.c_str(), argv.data());
      std::_Exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
  }).detach();
}

} // namespace

namespace clipdeck {

FeedbackNotifier::FeedbackNotifier(const ClipDeckSettings &settings) {
  if (!settings.feedback_sound_enabled) {
    return;
  }

  sound_path_ = ResolveFeedbackSoundPath(settings);
  const bool using_default = settings.feedback_sound_path.empty();
  if (using_default &&
      !GenerateDefaultFeedbackSound(sound_path_, settings.feedback_sound_volume) &&
      !std::filesystem::exists(sound_path_)) {
    Log(LogLevel::Warning, kFeedbackContext,
        "Default feedback sound is missing and could not be generated: " +
            sound_path_.string());
    return;
  }

  if (!std::filesystem::exists(sound_path_)) {
    Log(LogLevel::Warning, kFeedbackContext,
        "Feedback sound is missing: " + sound_path_.string());
    return;
  }

  player_ = FindAudioPlayer();
  if (!player_.has_value()) {
    Log(LogLevel::Warning, kFeedbackContext,
        "No feedback audio player found. Install paplay, pw-play, or aplay to enable keybind feedback.");
    return;
  }

  enabled_ = true;
}

void FeedbackNotifier::NotifyKeybindAccepted() {
  std::scoped_lock lock(mutex_);
  if (!enabled_) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (!RateLimitPassed(now)) {
    return;
  }

  last_played_at_ = now;
  PlaySoundAsync(player_.value(), sound_path_);
}

#ifdef CLIPDECK_ENABLE_RECORDER_TEST_HOOKS
void FeedbackNotifier::SetPlayerForTest(std::string player) {
  std::scoped_lock lock(mutex_);
  player_ = std::move(player);
  enabled_ = true;
}

std::optional<std::chrono::steady_clock::time_point>
FeedbackNotifier::LastPlayedAtForTest() const {
  std::scoped_lock lock(mutex_);
  return last_played_at_;
}
#endif

bool FeedbackNotifier::RateLimitPassed(
    std::chrono::steady_clock::time_point now) const {
  return !last_played_at_.has_value() ||
         now - last_played_at_.value() >= kFeedbackRateLimit;
}

} // namespace clipdeck
