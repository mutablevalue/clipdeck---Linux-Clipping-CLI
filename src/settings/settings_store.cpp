#include "settings_store.hpp"

#include "../utils/app_error.hpp"
#include "../utils/logger.hpp"
#include "../utils/number_parser.hpp"
#include "../utils/runtime_paths.hpp"

#include <fstream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kSettingsContext = "settings";

std::pair<std::string, std::string> SplitSettingLine(std::string_view line) {
  const std::size_t separator = line.find('=');

  if (separator == std::string_view::npos) {
    return {};
  }

  return {std::string(line.substr(0, separator)),
          std::string(line.substr(separator + 1))};
}

std::filesystem::path ResolveStoredPath(std::string_view value) {
  std::filesystem::path path(value);
  if (path.is_absolute()) {
    return path;
  }

  return clipdeck::ProjectRootDirectory() / path;
}

bool ParseBoolean(std::string_view value, bool &parsed) {
  if (value == "true" || value == "1" || value == "on") {
    parsed = true;
    return true;
  }

  if (value == "false" || value == "0" || value == "off") {
    parsed = false;
    return true;
  }

  return false;
}

} // namespace

namespace clipdeck {

SettingsStore::SettingsStore() : SettingsStore(SettingsPath()) {}

SettingsStore::SettingsStore(std::filesystem::path settings_path)
    : settings_path_(std::move(settings_path)) {}

ClipDeckSettings SettingsStore::Load() const {
  ClipDeckSettings settings;
  settings.clip_directory = settings_path_.parent_path() / "clips";
  std::ifstream input(settings_path_);

  if (!input.is_open()) {
    return settings;
  }

  std::string line;
  while (std::getline(input, line)) {
    const auto [key, value] = SplitSettingLine(line);

    if (key == "clip_length_seconds") {
      int seconds = 0;
      if (ParsePositiveInteger(value, seconds)) {
        settings.clip_length_seconds = seconds;
      }
      continue;
    }

    if (key == "buffer_safety_seconds") {
      int seconds = 0;
      if (ParsePositiveInteger(value, seconds)) {
        settings.buffer_safety_seconds = seconds;
      }
      continue;
    }

    if (key == "save_keybind" && !value.empty()) {
      settings.save_keybind = value;
      continue;
    }

    if (key == "clip_directory" && !value.empty()) {
      settings.clip_directory = ResolveStoredPath(value);
      continue;
    }

    if (key == "capture_video_source" && !value.empty()) {
      if (value == "portal") {
        settings.capture_video_source = value;
      } else {
        settings.capture_video_source = "portal";
        Log(LogLevel::Warning, kSettingsContext,
            "Ignored non-portal video source from settings; native capture is screen-only.");
      }
      continue;
    }

    if (key == "capture_audio_source") {
      settings.capture_audio_source =
          value.empty() ? std::string(kAutomaticAudioSource) : value;
      continue;
    }

    if (key == "capture_audio_enabled") {
      bool enabled = true;
      if (ParseBoolean(value, enabled)) {
        settings.capture_audio_enabled = enabled;
      }
      continue;
    }

    if (key == "capture_width") {
      int width = 0;
      if (ParsePositiveInteger(value, width)) {
        settings.capture_width = width;
      }
      continue;
    }

    if (key == "capture_height") {
      int height = 0;
      if (ParsePositiveInteger(value, height)) {
        settings.capture_height = height;
      }
      continue;
    }

    if (key == "capture_fps") {
      int fps = 0;
      if (ParsePositiveInteger(value, fps)) {
        settings.capture_fps = fps;
      }
      continue;
    }

    if (key == "video_bitrate_kbps") {
      int bitrate = 0;
      if (ParsePositiveInteger(value, bitrate)) {
        settings.video_bitrate_kbps = bitrate;
      }
      continue;
    }

    if (key == "audio_bitrate_kbps") {
      int bitrate = 0;
      if (ParsePositiveInteger(value, bitrate)) {
        settings.audio_bitrate_kbps = bitrate;
      }
      continue;
    }

    if (key == "encoder" && !value.empty()) {
      settings.encoder = value;
      continue;
    }

    if (key == "feedback_sound_enabled") {
      bool enabled = true;
      if (ParseBoolean(value, enabled)) {
        settings.feedback_sound_enabled = enabled;
      }
      continue;
    }

    if (key == "feedback_sound_path") {
      settings.feedback_sound_path =
          value.empty() ? std::filesystem::path{} : ResolveStoredPath(value);
      continue;
    }

    if (key == "feedback_sound_volume") {
      try {
        const double volume = std::stod(value);
        if (volume >= 0.0 && volume <= 1.0) {
          settings.feedback_sound_volume = volume;
        }
      } catch (...) {
      }
      continue;
    }

  }

  return settings;
}

bool SettingsStore::Save(const ClipDeckSettings &settings) const {
  std::error_code error;
  std::filesystem::create_directories(settings_path_.parent_path(), error);

  if (error) {
    HandleError(MakeError("settings_directory", kSettingsContext,
                          "Failed to create settings directory: " +
                              error.message()));
    return false;
  }

  std::ofstream output(settings_path_, std::ios::trunc);

  if (!output.is_open()) {
    HandleError(MakeError("settings_open", kSettingsContext,
                          "Failed to open settings file."));
    return false;
  }

  output << "clip_length_seconds=" << settings.clip_length_seconds << '\n';
  output << "buffer_safety_seconds=" << settings.buffer_safety_seconds << '\n';
  output << "save_keybind=" << settings.save_keybind << '\n';
  output << "clip_directory=" << settings.clip_directory.string() << '\n';
  output << "capture_video_source=" << settings.capture_video_source << '\n';
  output << "capture_audio_enabled="
         << (settings.capture_audio_enabled ? "true" : "false") << '\n';
  output << "capture_audio_source=" << settings.capture_audio_source << '\n';
  output << "capture_width=" << settings.capture_width << '\n';
  output << "capture_height=" << settings.capture_height << '\n';
  output << "capture_fps=" << settings.capture_fps << '\n';
  output << "video_bitrate_kbps=" << settings.video_bitrate_kbps << '\n';
  output << "audio_bitrate_kbps=" << settings.audio_bitrate_kbps << '\n';
  output << "encoder=" << settings.encoder << '\n';
  output << "feedback_sound_enabled="
         << (settings.feedback_sound_enabled ? "true" : "false") << '\n';
  output << "feedback_sound_path=" << settings.feedback_sound_path.string()
         << '\n';
  output << "feedback_sound_volume=" << settings.feedback_sound_volume << '\n';

  return output.good();
}

const std::filesystem::path &SettingsStore::Path() const {
  return settings_path_;
}

} // namespace clipdeck
