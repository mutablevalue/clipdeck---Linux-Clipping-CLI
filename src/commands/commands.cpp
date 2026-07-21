#include "commands.hpp"

#include "../core.hpp"
#include "../listener/keybind.hpp"
#include "../listener/keybind_capture.hpp"
#include "../recorder/recorder_setup.hpp"
#include "../utils/logger.hpp"
#include "../utils/number_parser.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kCommandContext = "commands";

bool HasExtraArguments(int argc, int expected_count) {
  return argc != expected_count;
}

void PrintAudioDevices() {
  const auto sources = clipdeck::AvailableAudioCaptureSources();
  bool found_monitor = false;

  for (const auto &source : sources) {
    if (!source.monitor) {
      continue;
    }

    if (!found_monitor) {
      Log(LogLevel::Info, kCommandContext,
          "Available output audio monitor sources:");
      found_monitor = true;
    }

    Log(LogLevel::Info, kCommandContext,
        "  " + source.name +
            (source.default_output_monitor ? " (default output)" : ""));
  }

  if (!found_monitor) {
    Log(LogLevel::Warning, kCommandContext,
        "No output monitor sources were found from pactl. Make sure "
        "PipeWire/PulseAudio is running.");
  }
}

} // namespace

namespace clipdeck {

int CommandHandler::Run(int argc, char *argv[]) {
  if (argc < 2) {
    Log(LogLevel::Error, kCommandContext, "Missing command.");
    PrintHelp();
    return 1;
  }

  const std::string command = argv[1];

  if (command == "help" || command == "--help" || command == "-h") {
    PrintHelp();
    return 0;
  }

  if (command == "start") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The start command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    clipdeck.Start();
    return 0;
  }

  if (command == "setup") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The setup command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    return clipdeck.Setup() ? 0 : 1;
  }

  if (command == "diagnose") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The diagnose command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    return clipdeck.Diagnose() ? 0 : 1;
  }

  if (command == "stop") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The stop command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    clipdeck.Stop();
    return 0;
  }

  if (command == "restart") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The restart command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    clipdeck.Restart();
    return 0;
  }

  if (command == "save") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The save command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    return clipdeck.Save() ? 0 : 1;
  }

  if (command == "choose-capture") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The choose-capture command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    return clipdeck.ChooseCapture() ? 0 : 1;
  }

  if (command == "status") {
    if (HasExtraArguments(argc, 2)) {
      Log(LogLevel::Error, kCommandContext,
          "The status command does not accept extra arguments.");
      PrintHelp();
      return 1;
    }

    ClipDeckCore clipdeck;
    clipdeck.Status();
    return 0;
  }

  if (command == "settings") {
    return RunSettingsCommand(argc, argv);
  }

  if (command == "time") {
    Log(LogLevel::Error, kCommandContext,
        "The time command moved to settings. Use: clipdeck settings length "
        "<seconds>.");
    return 1;
  }

  Log(LogLevel::Error, kCommandContext, "Unknown command: " + command);
  PrintHelp();
  return 1;
}

int CommandHandler::RunSettingsCommand(int argc, char *argv[]) const {
  ClipDeckCore clipdeck;

  if (argc == 2) {
    clipdeck.ShowSettings();
    return 0;
  }

  const std::string setting = argv[2];

  if (setting == "show") {
    if (HasExtraArguments(argc, 3)) {
      Log(LogLevel::Error, kCommandContext,
          "The settings show command does not accept extra arguments.");
      return 1;
    }

    clipdeck.ShowSettings();
    return 0;
  }

  if (setting == "keybind") {
    if (argc == 3) {
      return SetKeybindFromCapture("save");
    }

    if (argc == 4) {
      const std::string normalized_keybind = NormalizeKeybind(argv[3]);
      if (!ParseKeybindRequirements(normalized_keybind).has_value()) {
        Log(LogLevel::Error, kCommandContext,
            "Unsupported keybind. Use letters with Ctrl, Alt, or Shift.");
        return 1;
      }

      clipdeck.SetSaveKeybind(normalized_keybind);
      return 0;
    }

    Log(LogLevel::Error, kCommandContext, "Usage: clipdeck settings keybind");
    return 1;
  }

  if (setting == "stop-keybind") {
    if (argc == 3) {
      return SetKeybindFromCapture("stop");
    }

    if (argc == 4) {
      const std::string normalized_keybind = NormalizeKeybind(argv[3]);
      if (!ParseKeybindRequirements(normalized_keybind).has_value()) {
        Log(LogLevel::Error, kCommandContext,
            "Unsupported keybind. Use letters with Ctrl, Alt, or Shift.");
        return 1;
      }

      clipdeck.SetStopKeybind(normalized_keybind);
      return 0;
    }

    Log(LogLevel::Error, kCommandContext,
        "Usage: clipdeck settings stop-keybind [combo]");
    return 1;
  }

  if (setting == "length") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings length <seconds>");
      return 1;
    }

    int seconds = 0;
    if (!ParsePositiveInteger(argv[3], seconds)) {
      Log(LogLevel::Error, kCommandContext,
          "Clip length must be a positive whole number of seconds.");
      return 1;
    }

    clipdeck.SetClipLength(seconds);
    return 0;
  }

  if (setting == "buffer-safety") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings buffer-safety <seconds>");
      return 1;
    }

    int seconds = 0;
    if (!ParsePositiveInteger(argv[3], seconds)) {
      Log(LogLevel::Error, kCommandContext,
          "Buffer safety must be a positive whole number of seconds.");
      return 1;
    }

    clipdeck.SetBufferSafety(seconds);
    return 0;
  }

  if (setting == "output") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings output <directory>");
      return 1;
    }

    clipdeck.SetClipDirectory(std::filesystem::path(argv[3]));
    return 0;
  }

  if (setting == "video-source") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings video-source portal");
      return 1;
    }

    const std::string_view source = argv[3];
    if (source != "portal") {
      Log(LogLevel::Error, kCommandContext,
          "Native video capture uses the XDG portal picker. Use: clipdeck "
          "settings video-source portal");
      return 1;
    }

    clipdeck.SetCaptureVideoSource(argv[3]);
    return 0;
  }

  if (setting == "audio-source") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings audio-source <monitor-source>");
      return 1;
    }

    clipdeck.SetCaptureAudioSource(argv[3]);
    return 0;
  }

  if (setting == "audio") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings audio <on|off|auto|devices>");
      return 1;
    }

    const std::string mode = argv[3];
    if (mode == "devices" || mode == "list") {
      PrintAudioDevices();
      return 0;
    }

    if (mode == "on") {
      clipdeck.SetCaptureAudioEnabled(true);
      return 0;
    }

    if (mode == "off") {
      clipdeck.SetCaptureAudioEnabled(false);
      return 0;
    }

    if (mode == "auto") {
      clipdeck.SetCaptureAudioAuto();
      return 0;
    }

    Log(LogLevel::Error, kCommandContext,
        "Usage: clipdeck settings audio <on|off|auto|devices>");
    return 1;
  }

  if (setting == "resolution") {
    if (argc != 5) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings resolution <width> <height>");
      return 1;
    }

    int width = 0;
    int height = 0;
    if (!ParsePositiveInteger(argv[3], width) ||
        !ParsePositiveInteger(argv[4], height)) {
      Log(LogLevel::Error, kCommandContext,
          "Resolution width and height must be positive whole numbers.");
      return 1;
    }

    clipdeck.SetCaptureSize(width, height);
    return 0;
  }

  if (setting == "fps") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings fps <frames-per-second>");
      return 1;
    }

    int fps = 0;
    if (!ParsePositiveInteger(argv[3], fps)) {
      Log(LogLevel::Error, kCommandContext,
          "FPS must be a positive whole number.");
      return 1;
    }

    clipdeck.SetCaptureFps(fps);
    return 0;
  }

  if (setting == "video-bitrate") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings video-bitrate <kbps>");
      return 1;
    }

    int bitrate = 0;
    if (!ParsePositiveInteger(argv[3], bitrate)) {
      Log(LogLevel::Error, kCommandContext,
          "Video bitrate must be a positive whole number of kbps.");
      return 1;
    }

    clipdeck.SetVideoBitrate(bitrate);
    return 0;
  }

  if (setting == "audio-bitrate") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings audio-bitrate <kbps>");
      return 1;
    }

    int bitrate = 0;
    if (!ParsePositiveInteger(argv[3], bitrate)) {
      Log(LogLevel::Error, kCommandContext,
          "Audio bitrate must be a positive whole number of kbps.");
      return 1;
    }

    clipdeck.SetAudioBitrate(bitrate);
    return 0;
  }

  if (setting == "max-size") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings max-size <mb>");
      return 1;
    }

    int size_mb = 0;
    if (!ParsePositiveInteger(argv[3], size_mb)) {
      Log(LogLevel::Error, kCommandContext,
          "Maximum clip size must be a positive whole number of MB.");
      return 1;
    }

    clipdeck.SetMaxClipSize(size_mb);
    return 0;
  }

  if (setting == "encoder") {
    if (argc != 4) {
      Log(LogLevel::Error, kCommandContext,
          "Usage: clipdeck settings encoder <auto|openh264|x264>");
      return 1;
    }

    const std::string encoder = argv[3];
    if (encoder != "auto" && encoder != "openh264" && encoder != "x264") {
      Log(LogLevel::Error, kCommandContext,
          "Encoder must be auto, openh264, or x264.");
      return 1;
    }

    clipdeck.SetEncoder(encoder);
    return 0;
  }

  Log(LogLevel::Error, kCommandContext, "Unknown setting: " + setting);
  PrintHelp();
  return 1;
}

int CommandHandler::SetKeybindFromCapture(std::string_view action) const {
  Log(LogLevel::Info, kCommandContext,
      "Press the " + std::string(action) +
          " keybind for terminal-only setup capture, for example Ctrl + Z + "
          "P.");
  Log(LogLevel::Info, kCommandContext,
      "For terminal capture, hold Ctrl and press Z, then press P, then press "
      "Enter.");
  Log(LogLevel::Info, kCommandContext,
      "This setup capture does not verify global /dev/input event permissions "
      "for the daemon.");

  const auto captured_keybind =
      CaptureKeybindFromTerminal(std::chrono::seconds(15));

  if (!captured_keybind.has_value()) {
    Log(LogLevel::Error, kCommandContext, "No keybind was captured.");
    return 1;
  }

  if (!ParseKeybindRequirements(captured_keybind.value()).has_value()) {
    Log(LogLevel::Error, kCommandContext,
        "Captured keybind is not supported by the Linux input listener.");
    return 1;
  }

  ClipDeckCore clipdeck;
  if (action == "stop") {
    clipdeck.SetStopKeybind(captured_keybind.value());
  } else {
    clipdeck.SetSaveKeybind(captured_keybind.value());
  }
  return 0;
}

void CommandHandler::PrintHelp() const {
  Log(LogLevel::Info, kCommandContext, "Usage: clipdeck <command>");
  Log(LogLevel::Info, kCommandContext, "Commands:");
  Log(LogLevel::Info, kCommandContext,
      "  start                         Start the background listener");
  Log(LogLevel::Info, kCommandContext,
      "  stop                          Stop the background listener");
  Log(LogLevel::Info, kCommandContext,
      "  restart                       Restart the background listener");
  Log(LogLevel::Info, kCommandContext,
      "  save                          Request a clip save");
  Log(LogLevel::Info, kCommandContext,
      "  choose-capture                Reopen the desktop portal capture "
      "picker");
  Log(LogLevel::Info, kCommandContext,
      "  status                        Show runtime status and settings");
  Log(LogLevel::Info, kCommandContext,
      "  setup                         Validate and save native capture "
      "sources");
  Log(LogLevel::Info, kCommandContext,
      "  diagnose                      Check recorder configuration");
  Log(LogLevel::Info, kCommandContext,
      "  settings                      Show saved settings");
  Log(LogLevel::Info, kCommandContext,
      "  settings keybind              Capture and save the save keybind");
  Log(LogLevel::Info, kCommandContext,
      "  settings keybind <combo>      Save a keybind directly, like Ctrl+Z+P");
  Log(LogLevel::Info, kCommandContext,
      "  settings stop-keybind         Capture and save the stop keybind");
  Log(LogLevel::Info, kCommandContext,
      "  settings stop-keybind <combo> Save the stop keybind directly");
  Log(LogLevel::Info, kCommandContext,
      "  settings length <seconds>     Set the clip length");
  Log(LogLevel::Info, kCommandContext,
      "  settings buffer-safety <sec>  Set extra recorder buffer duration");
  Log(LogLevel::Info, kCommandContext,
      "  settings output <directory>   Set the clip save directory");
  Log(LogLevel::Info, kCommandContext,
      "  settings video-source portal  Use the desktop portal picker");
  Log(LogLevel::Info, kCommandContext,
      "  settings audio <on|off|auto>  Enable, disable, or auto-select output "
      "audio");
  Log(LogLevel::Info, kCommandContext,
      "  settings audio devices        List output audio monitor sources");
  Log(LogLevel::Info, kCommandContext,
      "  settings audio-source <src>   Set desktop audio monitor source");
  Log(LogLevel::Info, kCommandContext,
      "  settings resolution <w> <h>   Set capture resolution");
  Log(LogLevel::Info, kCommandContext,
      "  settings fps <fps>            Set capture frame rate");
  Log(LogLevel::Info, kCommandContext,
      "  settings video-bitrate <kbps> Set video bitrate");
  Log(LogLevel::Info, kCommandContext,
      "  settings audio-bitrate <kbps> Set audio bitrate");
  Log(LogLevel::Info, kCommandContext,
      "  settings max-size <mb>        Set maximum final clip size");
  Log(LogLevel::Info, kCommandContext,
      "  settings encoder <enc>        Set encoder auto, openh264, or x264");
  Log(LogLevel::Info, kCommandContext,
      "  help                          Show this help");
}

} // namespace clipdeck
