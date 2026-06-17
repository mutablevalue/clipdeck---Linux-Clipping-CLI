#include "runtime_paths.hpp"

#include "../settings/settings_store.hpp"

#include <array>
#include <cstdlib>
#include <unistd.h>

namespace clipdeck {

std::filesystem::path ProjectRootDirectory() {
  std::array<char, 4096> executable_path{};
  const ssize_t length =
      readlink("/proc/self/exe", executable_path.data(),
               executable_path.size() - 1);

  if (length > 0) {
    executable_path[static_cast<std::size_t>(length)] = '\0';
    const auto executable = std::filesystem::path(executable_path.data());
    const auto executable_directory = executable.parent_path();

    if (executable_directory.filename() == "build") {
      return executable_directory.parent_path();
    }
  }

  return std::filesystem::current_path();
}

std::filesystem::path OutputDirectory() {
  if (const char *output_directory = std::getenv("CLIPDECK_OUTPUT_DIR");
      output_directory != nullptr && output_directory[0] != '\0') {
    return std::filesystem::path(output_directory);
  }

  return ProjectRootDirectory() / "output";
}

std::filesystem::path SettingsPath() { return OutputDirectory() / "settings.conf"; }

std::filesystem::path RuntimeDirectory() { return OutputDirectory() / "runtime"; }

std::filesystem::path SegmentDirectory() {
  return RuntimeDirectory() / "segments";
}

std::filesystem::path PidFilePath() { return RuntimeDirectory() / "clipdeck.pid"; }

std::filesystem::path DaemonLogPath() {
  return RuntimeDirectory() / "clipdeck-daemon.log";
}

std::filesystem::path RecorderStatusPath() {
  return RuntimeDirectory() / "recorder-status.conf";
}

std::filesystem::path ListenerStatusPath() {
  return RuntimeDirectory() / "listener-status.conf";
}

std::filesystem::path DefaultFeedbackSoundPath() {
  return RuntimeDirectory() / "assets" / "sounds" / "clip-accepted.wav";
}

std::filesystem::path ResolveFeedbackSoundPath(
    const ClipDeckSettings &settings) {
  if (settings.feedback_sound_path.empty()) {
    return DefaultFeedbackSoundPath();
  }

  return settings.feedback_sound_path;
}

} // namespace clipdeck
