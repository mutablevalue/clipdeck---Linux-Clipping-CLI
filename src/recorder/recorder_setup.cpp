#include "recorder_setup.hpp"

#include "../utils/logger.hpp"
#include "../utils/string_utils.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(CLIPDECK_HAS_GSTREAMER)
#include <gst/gst.h>
#endif

namespace {

constexpr std::string_view kSetupContext = "setup";

std::optional<std::string> CaptureCommandOutput(const std::string &command) {
  std::array<char, 256> buffer{};
  std::string output;

  struct PipeCloser {
    void operator()(FILE *file) const {
      if (file != nullptr) {
        pclose(file);
      }
    }
  };

  std::unique_ptr<FILE, PipeCloser> pipe(popen(command.c_str(), "r"));
  if (pipe == nullptr) {
    return std::nullopt;
  }

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) !=
         nullptr) {
    output += buffer.data();
  }

  while (!output.empty() &&
         (output.back() == '\n' || output.back() == '\r' ||
          output.back() == ' ' || output.back() == '\t')) {
    output.pop_back();
  }

  if (output.empty()) {
    return std::nullopt;
  }

  return output;
}

std::optional<std::string> DefaultSinkMonitor() {
  const auto default_sink = CaptureCommandOutput("pactl get-default-sink 2>/dev/null");
  if (!default_sink.has_value()) {
    return std::nullopt;
  }

  return default_sink.value() + ".monitor";
}

bool UsesAutomaticAudioSource(std::string_view source) {
  return source.empty() || source == clipdeck::kAutomaticAudioSource;
}

#if defined(CLIPDECK_HAS_GSTREAMER)
std::vector<std::string>
RequiredElements(const clipdeck::ClipDeckSettings &settings) {
  std::vector<std::string> elements{"pipewiresrc", "videoconvert", "videoscale",
                                    "h264parse", "mp4mux", "splitmuxsink"};

  elements.push_back(settings.encoder == "x264" ? "x264enc" : "openh264enc");

  if (settings.capture_audio_enabled) {
    elements.emplace_back("pulsesrc");
    elements.emplace_back("audioconvert");
    elements.emplace_back("audioresample");
    elements.emplace_back("avenc_aac");
    elements.emplace_back("aacparse");
  }

  return elements;
}
#endif

bool ValidateGStreamerPlugins(const clipdeck::ClipDeckSettings &settings) {
#if !defined(CLIPDECK_HAS_GSTREAMER)
  (void)settings;
  Log(LogLevel::Error, kSetupContext,
      "This build was compiled without GStreamer support.");
  return false;
#else
  GError *error = nullptr;
  if (!gst_init_check(nullptr, nullptr, &error)) {
    const std::string message =
        error == nullptr ? "unknown error" : error->message;
    if (error != nullptr) {
      g_error_free(error);
    }

    Log(LogLevel::Error, kSetupContext,
        "Failed to initialize GStreamer: " + message);
    return false;
  }

  bool ok = true;
  GstRegistry *registry = gst_registry_get();

  for (const auto &element : RequiredElements(settings)) {
    GstPluginFeature *feature =
        gst_registry_lookup_feature(registry, element.c_str());
    if (feature == nullptr) {
      Log(LogLevel::Error, kSetupContext,
          "Missing GStreamer element: " + element + ".");
      ok = false;
      continue;
    }

    gst_object_unref(feature);
    Log(LogLevel::Info, kSetupContext,
        "Found GStreamer element: " + element + ".");
  }

  return ok;
#endif
}

bool ValidateFfmpeg() {
  if (CaptureCommandOutput("command -v ffmpeg").has_value()) {
    Log(LogLevel::Info, kSetupContext, "Found ffmpeg for clip composition.");
    return true;
  }

  Log(LogLevel::Error, kSetupContext,
      "Missing ffmpeg. Install ffmpeg so ClipDeck can compose retained recorder segments into MP4 clips.");
  return false;
}

} // namespace

namespace clipdeck {

std::vector<AudioCaptureSource> ParseAudioCaptureSources(
    std::string_view pactl_sources_output,
    const std::optional<std::string> &default_output_monitor) {
  std::vector<AudioCaptureSource> capture_sources;

  std::istringstream lines{std::string(pactl_sources_output)};
  std::string line;
  while (std::getline(lines, line)) {
    std::istringstream columns(line);
    std::string index;
    std::string source;
    columns >> index >> source;
    if (source.empty()) {
      continue;
    }

    capture_sources.push_back(AudioCaptureSource{
        .name = source,
        .monitor = source.ends_with(".monitor"),
        .default_output_monitor =
            default_output_monitor.has_value() &&
            source == default_output_monitor.value()});
  }

  return capture_sources;
}

std::optional<std::string>
SelectAutomaticAudioMonitor(const std::vector<AudioCaptureSource> &sources) {
  for (const auto &source : sources) {
    if (source.monitor && source.default_output_monitor) {
      return source.name;
    }
  }

  for (const auto &source : sources) {
    if (source.monitor) {
      return source.name;
    }
  }

  return std::nullopt;
}

std::vector<AudioCaptureSource> AvailableAudioCaptureSources() {
  const auto output = CaptureCommandOutput("pactl list short sources 2>/dev/null");
  if (!output.has_value()) {
    return {};
  }

  return ParseAudioCaptureSources(output.value(), DefaultSinkMonitor());
}

std::optional<std::string>
ResolveCaptureAudioSource(const ClipDeckSettings &settings) {
  if (!settings.capture_audio_enabled) {
    return std::nullopt;
  }

  if (UsesAutomaticAudioSource(settings.capture_audio_source)) {
    return SelectAutomaticAudioMonitor(AvailableAudioCaptureSources());
  }

  for (const auto &source : AvailableAudioCaptureSources()) {
    if (source.name == settings.capture_audio_source) {
      return settings.capture_audio_source;
    }
  }

  return std::nullopt;
}

bool SetupNativeRecorder(ClipDeckSettings &settings) {
  if (settings.capture_video_source.empty() ||
      settings.capture_video_source != "portal") {
    settings.capture_video_source = "portal";
  }

  Log(LogLevel::Info, kSetupContext,
      "Video source: portal picker for monitor or window capture.");

  if (settings.capture_audio_source.empty()) {
    settings.capture_audio_source = std::string(kAutomaticAudioSource);
  }

  const auto audio_source = ResolveCaptureAudioSource(settings);
  if (!settings.capture_audio_enabled) {
    Log(LogLevel::Info, kSetupContext, "Audio capture: disabled.");
  } else if (!audio_source.has_value()) {
    Log(LogLevel::Error, kSetupContext,
        "No desktop audio monitor source was detected. Use 'clipdeck settings audio off' for video-only clips, or set a monitor source with 'clipdeck settings audio-source <source>'.");
    return false;
  } else {
    Log(LogLevel::Info, kSetupContext,
        "Audio source: " + audio_source.value() + ".");
  }

  const bool plugins_ok = ValidateGStreamerPlugins(settings) && ValidateFfmpeg();
  if (!plugins_ok) {
    Log(LogLevel::Error, kSetupContext,
        "Install the missing GStreamer/PipeWire runtime plugins and rerun clipdeck setup.");
    return false;
  }

  Log(LogLevel::Info, kSetupContext,
      "Native recorder setup completed. Portal permission may still be requested when capture starts.");
  return true;
}

bool DiagnoseNativeRecorder(const ClipDeckSettings &settings) {
  bool ok = true;

  Log(LogLevel::Info, kSetupContext,
      "Capture: " + std::to_string(settings.capture_width) + "x" +
          std::to_string(settings.capture_height) + "@" +
          std::to_string(settings.capture_fps) + "fps.");
  Log(LogLevel::Info, kSetupContext,
      "Video source: " + settings.capture_video_source + ".");
  Log(LogLevel::Info, kSetupContext,
      "Audio capture: " +
          std::string(settings.capture_audio_enabled ? "enabled" : "disabled") +
          ".");
  const auto audio_source = ResolveCaptureAudioSource(settings);
  Log(LogLevel::Info, kSetupContext,
      "Audio source: " +
          (audio_source.has_value() ? audio_source.value()
                                    : std::string("<none>")) +
          ".");

  if (settings.capture_audio_enabled && !audio_source.has_value()) {
    Log(LogLevel::Error, kSetupContext,
        "Audio capture is enabled but no PulseAudio/PipeWire monitor source is available.");
    ok = false;
  }

  ok = ValidateGStreamerPlugins(settings) && ok;
  ok = ValidateFfmpeg() && ok;

  return ok;
}

} // namespace clipdeck
