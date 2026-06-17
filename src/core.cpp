#include "core.hpp"

#include "recorder/clip_muxer.hpp"
#include "recorder/gstreamer_recorder.hpp"
#include "recorder/recorder_backend.hpp"
#include "recorder/recorder_setup.hpp"
#include "recorder/segment_file.hpp"
#include "feedback_notifier.hpp"
#include "save_request_controller.hpp"
#include "utils/logger.hpp"
#include "utils/process.hpp"
#include "utils/runtime_paths.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace {

constexpr std::string_view kCoreContext = "core";
volatile std::sig_atomic_t shutdown_requested = 0;
volatile std::sig_atomic_t save_requested = 0;
volatile std::sig_atomic_t choose_capture_requested = 0;

void HandleShutdownSignal(int) {
  shutdown_requested = 1;
}

void HandleSaveSignal(int) {
  save_requested = 1;
}

void HandleChooseCaptureSignal(int) {
  choose_capture_requested = 1;
}

std::string BoolString(bool value) { return value ? "true" : "false"; }

std::string RecorderStatusLine(const clipdeck::RecorderStatus &status) {
  return "backend=" + status.backend + "\n" +
         "running=" + (status.running ? std::string("true") : "false") + "\n" +
         "healthy=" + (status.healthy ? std::string("true") : "false") + "\n" +
         "message=" + status.message + "\n" +
         "capture_source_type=" + status.capture_source_type + "\n" +
         "buffered_milliseconds=" +
         std::to_string(status.buffered_duration.count()) + "\n" +
         "buffered_bytes=" + std::to_string(status.buffered_bytes) + "\n" +
         "memory_budget_bytes=" +
         std::to_string(status.memory_budget_bytes) + "\n" +
         "finalized_segment_count=" +
         std::to_string(status.finalized_segment_count) + "\n" +
         "finalized_segment_duration_ms=" +
         std::to_string(status.finalized_segment_duration.count()) + "\n" +
         "can_save_any_clip=" + BoolString(status.can_save_any_clip) + "\n" +
         "can_save_full_clip_without_padding=" +
         BoolString(status.can_save_full_clip_without_padding) + "\n" +
         "last_capture_anomaly=" + status.last_capture_anomaly + "\n" +
         "last_save_failure=" + status.last_save_failure + "\n" +
         "last_saved_clip=" + status.last_saved_clip.string() + "\n" +
         "audio_sample_rate=" +
         (status.audio_sample_rate.has_value()
              ? std::to_string(status.audio_sample_rate.value())
              : std::string("unknown")) +
         "\n" +
         "audio_channels=" +
         (status.audio_channels.has_value()
              ? std::to_string(status.audio_channels.value())
              : std::string("unknown")) +
         "\n" +
         "audio_enabled=" + BoolString(status.audio_enabled) + "\n";
}

std::string TimePointLine(
    const std::optional<std::chrono::system_clock::time_point> &time_point) {
  if (!time_point.has_value()) {
    return "never";
  }

  return std::to_string(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          time_point.value().time_since_epoch())
          .count());
}

std::string ListenerStatusLine(
    const clipdeck::ListenerStatus &listener_status,
    const clipdeck::SaveControllerStatus &save_status) {
  const std::string last_source =
      save_status.last_request_source.has_value()
          ? std::string(clipdeck::SaveSourceName(
                save_status.last_request_source.value()))
          : std::string("none");

  return "running=" + BoolString(listener_status.running) + "\n" +
         "save_keybind=" + listener_status.save_keybind + "\n" +
         "stop_keybind=" + listener_status.stop_keybind + "\n" +
         "input_directory=" + listener_status.input_directory.string() + "\n" +
         "scanned_devices=" +
         std::to_string(listener_status.scanned_devices) + "\n" +
         "readable_devices=" +
         std::to_string(listener_status.readable_devices) + "\n" +
         "accepted_keyboard_devices=" +
         std::to_string(listener_status.accepted_keyboard_devices) + "\n" +
         "last_error=" + listener_status.last_error + "\n" +
         "last_keybind_detected_ms=" +
         TimePointLine(listener_status.last_keybind_detected) + "\n" +
         "last_save_request_source=" + last_source + "\n" +
         "save_currently_running=" + BoolString(save_status.saving) + "\n" +
         "pending_save=" + BoolString(save_status.pending) + "\n";
}

void WriteRecorderStatusFile(const clipdeck::RecorderStatus &status) {
  std::error_code error;
  std::filesystem::create_directories(
      clipdeck::RecorderStatusPath().parent_path(), error);

  if (error) {
    return;
  }

  std::ofstream output(clipdeck::RecorderStatusPath(), std::ios::trunc);
  if (output.is_open()) {
    output << RecorderStatusLine(status);
  }
}

void WriteListenerStatusFile(
    const clipdeck::ListenerStatus &listener_status,
    const clipdeck::SaveControllerStatus &save_status) {
  std::error_code error;
  std::filesystem::create_directories(
      clipdeck::ListenerStatusPath().parent_path(), error);

  if (error) {
    return;
  }

  std::ofstream output(clipdeck::ListenerStatusPath(), std::ios::trunc);
  if (output.is_open()) {
    output << ListenerStatusLine(listener_status, save_status);
  }
}

void PrintRecorderStatusFile() {
  std::ifstream input(clipdeck::RecorderStatusPath());

  if (!input.is_open()) {
    Log(LogLevel::Warning, kCoreContext,
        "Recorder runtime status is not available yet.");
    return;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      Log(LogLevel::Info, kCoreContext, "Recorder " + line + ".");
    }
  }
}

void PrintListenerStatusFile() {
  std::ifstream input(clipdeck::ListenerStatusPath());

  if (!input.is_open()) {
    Log(LogLevel::Warning, kCoreContext,
        "Global listener runtime status is not available yet.");
    return;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      Log(LogLevel::Info, kCoreContext, "Global listener " + line + ".");
    }
  }
}

void PrintRetainedSegmentStatus(int clip_length_seconds) {
  const auto segments = clipdeck::SelectRecentFinalizedSegmentsByCount(
      clipdeck::SegmentDirectory(),
      static_cast<std::size_t>(std::max(clip_length_seconds + 2, 1)));
  if (segments.empty()) {
    return;
  }

  std::uintmax_t bytes = 0;
  for (const auto &segment : segments) {
    std::error_code error;
    bytes += std::filesystem::file_size(segment, error);
  }

  Log(LogLevel::Info, kCoreContext,
      "Recorder retained_segments=" + std::to_string(segments.size()) + ".");
  Log(LogLevel::Info, kCoreContext,
      "Recorder retained_segment_bytes=" + std::to_string(bytes) + ".");
}

std::optional<std::filesystem::path> NewestClipModifiedAfter(
    const std::filesystem::path &clip_directory,
    std::filesystem::file_time_type threshold) {
  std::optional<std::filesystem::path> newest_clip;
  auto newest_time = std::filesystem::file_time_type::min();
  std::error_code error;

  if (!std::filesystem::exists(clip_directory, error)) {
    return std::nullopt;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(clip_directory, error)) {
    if (error || !entry.is_regular_file() || entry.path().extension() != ".mp4") {
      continue;
    }

    std::error_code size_error;
    if (entry.file_size(size_error) == 0 || size_error) {
      continue;
    }

    std::error_code time_error;
    const auto modified_time = entry.last_write_time(time_error);
    if (time_error || modified_time < threshold || modified_time < newest_time) {
      continue;
    }

    newest_clip = entry.path();
    newest_time = modified_time;
  }

  return newest_clip;
}

std::unordered_map<std::string, std::string>
ReadKeyValueFile(const std::filesystem::path &path) {
  std::unordered_map<std::string, std::string> values;
  std::ifstream input(path);
  if (!input.is_open()) {
    return values;
  }

  std::string line;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }

    values.emplace(line.substr(0, separator), line.substr(separator + 1));
  }

  return values;
}

std::optional<std::string> CurrentRecorderUnhealthyReason() {
  const auto status = ReadKeyValueFile(clipdeck::RecorderStatusPath());
  if (status.empty()) {
    return std::nullopt;
  }

  if (const auto healthy = status.find("healthy");
      healthy != status.end() && healthy->second == "false") {
    if (const auto message = status.find("message");
        message != status.end() && !message->second.empty()) {
      return message->second;
    }

    return std::string("Recorder is not healthy.");
  }

  return std::nullopt;
}

std::optional<std::string> CurrentSaveFailure() {
  const auto status = ReadKeyValueFile(clipdeck::RecorderStatusPath());
  if (status.empty()) {
    return std::nullopt;
  }

  if (auto unhealthy = CurrentRecorderUnhealthyReason(); unhealthy.has_value()) {
    return unhealthy;
  }

  if (const auto failure = status.find("last_save_failure");
      failure != status.end() && !failure->second.empty()) {
    return failure->second;
  }

  return std::nullopt;
}

struct SaveWaitResult {
  std::optional<std::filesystem::path> clip;
  std::optional<std::string> failure;
};

SaveWaitResult WaitForPublishedClipOrFailure(
    const std::filesystem::path &clip_directory,
    std::filesystem::file_time_type threshold,
    std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (auto clip = NewestClipModifiedAfter(clip_directory, threshold);
        clip.has_value()) {
      return SaveWaitResult{.clip = clip, .failure = std::nullopt};
    }

    std::error_code error;
    const auto status_modified_time =
        std::filesystem::last_write_time(clipdeck::RecorderStatusPath(), error);
    if (!error && status_modified_time >= threshold) {
      if (auto failure = CurrentSaveFailure(); failure.has_value()) {
        return SaveWaitResult{.clip = std::nullopt, .failure = failure};
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  return SaveWaitResult{.clip = std::nullopt, .failure = std::nullopt};
}

bool WaitForProcessExit(pid_t pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    if (!clipdeck::IsProcessRunning(pid)) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  return !clipdeck::IsProcessRunning(pid);
}

bool StopClipDeckProcess(pid_t pid, std::string_view source) {
  if (!clipdeck::IsProcessRunning(pid)) {
    return true;
  }

  if (!clipdeck::RequestProcessGroupStop(pid)) {
    Log(LogLevel::Error, kCoreContext,
        "Failed to stop ClipDeck listener " + std::string(source) +
            " pid " + std::to_string(pid) + ".");
    return false;
  }

  if (WaitForProcessExit(pid, std::chrono::seconds(5))) {
    Log(LogLevel::Info, kCoreContext,
        "Stopped ClipDeck listener " + std::string(source) + " pid " +
            std::to_string(pid) + ".");
    return true;
  }

  Log(LogLevel::Warning, kCoreContext,
      "ClipDeck listener " + std::string(source) + " pid " +
          std::to_string(pid) +
          " did not exit after SIGTERM; sending SIGKILL.");

  if (!clipdeck::ForceProcessGroupStop(pid)) {
    return false;
  }

  return WaitForProcessExit(pid, std::chrono::seconds(2));
}

} // namespace

namespace clipdeck {

ClipDeckCore::ClipDeckCore() : settings_(settings_store_.Load()) {}

void ClipDeckCore::Start() {
  const auto pid_file = PidFilePath();

  if (const auto existing_pid = ReadPidFile(pid_file);
      existing_pid.has_value() && IsProcessRunning(existing_pid.value())) {
    Log(LogLevel::Warning, kCoreContext,
        "ClipDeck listener is already running with pid " +
            std::to_string(existing_pid.value()) + ".");
    return;
  }

  std::error_code error;
  std::filesystem::create_directories(settings_.clip_directory, error);

  if (error) {
    Log(LogLevel::Error, kCoreContext,
        "Failed to create clip directory: " + error.message());
    return;
  }

  const pid_t pid = fork();

  if (pid < 0) {
    Log(LogLevel::Error, kCoreContext, "Failed to start background listener.");
    return;
  }

  if (pid > 0) {
    if (WritePidFile(pid_file, pid)) {
      Log(LogLevel::Info, kCoreContext,
          "Started ClipDeck listener in the background with pid " +
              std::to_string(pid) + ".");
    }
    return;
  }

  setsid();
  RedirectStandardStreams(DaemonLogPath());
  Logger::Instance().SetLogFile(DaemonLogPath());

  ClipDeckCore daemon_core;
  const int exit_code = daemon_core.RunListener();
  RemovePidFile(pid_file);
  std::_Exit(exit_code);
}

int ClipDeckCore::RunListener() {
  shutdown_requested = 0;
  save_requested = 0;
  choose_capture_requested = 0;
  std::signal(SIGINT, HandleShutdownSignal);
  std::signal(SIGTERM, HandleShutdownSignal);
  std::signal(SIGUSR1, HandleSaveSignal);
  std::signal(SIGUSR2, HandleChooseCaptureSignal);

  DaemonListener listener(BuildListenerConfig());

  Log(LogLevel::Info, kCoreContext, "ClipDeck listener runtime started.");

  auto recorder =
      std::make_unique<GStreamerRecorder>(BuildRecorderConfig(settings_));
  WriteRecorderStatusFile(RecorderStatus{.running = false,
                                         .healthy = false,
                                         .backend = "native",
                                         .message = "starting",
                                         .capture_source_type = "",
                                         .buffered_duration =
                                             std::chrono::milliseconds(0),
                                         .buffered_bytes = 0,
                                         .memory_budget_bytes = 0,
                                         .finalized_segment_count = 0,
                                         .finalized_segment_duration =
                                             std::chrono::milliseconds(0),
                                         .can_save_any_clip = false,
                                         .can_save_full_clip_without_padding =
                                             false,
                                         .last_capture_anomaly = "",
                                         .last_save_failure = "",
                                         .last_saved_clip = {},
                                         .audio_sample_rate = std::nullopt,
                                         .audio_channels = std::nullopt,
                                         .audio_enabled =
                                             settings_.capture_audio_enabled});
  if (!recorder->Start()) {
    Log(LogLevel::Warning, kCoreContext,
        "Native recorder did not start. Keybinds will still listen, but saves will fail until the recorder is healthy.");
  }
  WriteRecorderStatusFile(recorder->Status());

  SaveRequestController save_controller(
      [&](SaveSource source) {
        Log(LogLevel::Info, kCoreContext,
            "Processing clip save request from " +
                std::string(SaveSourceName(source)) + ".");
        const bool saved = recorder->SaveClip();
        WriteRecorderStatusFile(recorder->Status());
        Log(saved ? LogLevel::Info : LogLevel::Error, kCoreContext,
            "Finished clip save request from " +
                std::string(SaveSourceName(source)) +
                (saved ? " successfully." : " with failure."));
      },
      SaveRequestControllerConfig{.keybind_debounce = std::chrono::seconds(1)});
  FeedbackNotifier feedback_notifier(settings_);

  listener.SetKeybindCallback([&](std::string_view action) {
    if (action == "stop") {
      Log(LogLevel::Info, kCoreContext,
          "Accepted keybind stop request.");
      shutdown_requested = 1;
      return;
    }

    if (action != "save") {
      return;
    }

    if (save_controller.RequestSave(SaveSource::Keybind)) {
      Log(LogLevel::Info, kCoreContext,
          "Accepted keybind clip save request.");
      feedback_notifier.NotifyKeybindAccepted();
    } else {
      Log(LogLevel::Debug, kCoreContext,
          "Ignored keybind clip save request.");
    }
  });

  listener.Start();
  WriteListenerStatusFile(listener.Status(), save_controller.Status());

  auto next_status_write = std::chrono::steady_clock::now();
  bool choose_capture_wait_logged = false;

  while (listener.IsRunning() && shutdown_requested == 0) {
    if (save_requested != 0) {
      save_requested = 0;

      if (save_controller.RequestSave(SaveSource::Manual)) {
        Log(LogLevel::Info, kCoreContext,
            "Accepted manual clip save request.");
      } else {
        Log(LogLevel::Warning, kCoreContext,
            "Ignored manual clip save request.");
      }
    }

    if (choose_capture_requested != 0) {
      const auto save_status = save_controller.Status();
      if (save_status.saving) {
        if (!choose_capture_wait_logged) {
          Log(LogLevel::Info, kCoreContext,
              "Capture reselection requested; waiting for the active save to finish.");
          choose_capture_wait_logged = true;
        }
      } else {
        choose_capture_requested = 0;
        choose_capture_wait_logged = false;
        Log(LogLevel::Info, kCoreContext,
            "Restarting recorder so the desktop portal can choose a new capture source.");
        recorder->Stop();
        recorder =
            std::make_unique<GStreamerRecorder>(BuildRecorderConfig(settings_));
        if (!recorder->Start()) {
          Log(LogLevel::Warning, kCoreContext,
              "Native recorder did not restart after capture reselection request.");
        }
        WriteRecorderStatusFile(recorder->Status());
      }
    }

    if (std::chrono::steady_clock::now() >= next_status_write) {
      const auto recorder_status = recorder->Status();
      WriteRecorderStatusFile(recorder_status);
      WriteListenerStatusFile(listener.Status(), save_controller.Status());
      if (recorder_status.capture_source_type == "window" &&
          !recorder_status.running) {
        Log(LogLevel::Info, kCoreContext,
            "Selected window capture stream ended; stopping ClipDeck listener. Start ClipDeck again to choose a new window, or choose a display capture to keep recording independent of an app window.");
        shutdown_requested = 1;
      }
      next_status_write = std::chrono::steady_clock::now() +
                          std::chrono::seconds(1);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  listener.Stop();
  save_controller.Stop();
  recorder->Stop();
  WriteRecorderStatusFile(recorder->Status());
  WriteListenerStatusFile(listener.Status(), save_controller.Status());

  Log(LogLevel::Info, kCoreContext, "ClipDeck listener runtime stopped.");
  return 0;
}

void ClipDeckCore::Restart() {
  Stop();
  Start();
}

void ClipDeckCore::Stop() {
  const auto pid_file = PidFilePath();
  const auto pid = ReadPidFile(pid_file);
  bool stopped_any = false;

  if (pid.has_value()) {
    if (IsProcessRunning(pid.value())) {
      stopped_any = StopClipDeckProcess(pid.value(), "pid-file") || stopped_any;
    } else {
      Log(LogLevel::Warning, kCoreContext,
          "Removed stale ClipDeck pid file for pid " +
              std::to_string(pid.value()) + ".");
    }
  }

  for (const pid_t orphan_pid : FindSiblingProcessesByExecutable()) {
    if (pid.has_value() && orphan_pid == pid.value()) {
      continue;
    }

    stopped_any =
        StopClipDeckProcess(orphan_pid, "orphan") || stopped_any;
  }

  RemovePidFile(pid_file);
  std::error_code error;
  std::filesystem::remove(RecorderStatusPath(), error);
  std::filesystem::remove(ListenerStatusPath(), error);

  if (!stopped_any) {
    Log(LogLevel::Info, kCoreContext, "ClipDeck listener is not running.");
  }
}

bool ClipDeckCore::Save() {
  Log(LogLevel::Info, kCoreContext, "Clip save requested.");

  const auto pid = ReadPidFile(PidFilePath());
  if (!pid.has_value() || !IsProcessRunning(pid.value())) {
    if (pid.has_value()) {
      RemovePidFile(PidFilePath());
      std::error_code status_error;
      std::filesystem::remove(RecorderStatusPath(), status_error);
      std::filesystem::remove(ListenerStatusPath(), status_error);
    }

    const auto segments = SelectRecentFinalizedSegmentsByCount(
        SegmentDirectory(),
        static_cast<std::size_t>(
            std::max(settings_.clip_length_seconds +
                         std::clamp(settings_.buffer_safety_seconds, 1, 3),
                     1)));
    if (segments.empty()) {
      Log(LogLevel::Error, kCoreContext,
          "ClipDeck daemon is not running and no retained recorder segments are available.");
      return false;
    }

    ClipMuxer muxer(settings_.clip_directory);
    const auto recorder_config = BuildRecorderConfig(settings_);
    const auto clip_path = muxer.WriteClipFromSegments(
        segments, ClipMuxerOptions{.target_duration =
                                       std::chrono::seconds(
                                           settings_.clip_length_seconds),
                                   .width = recorder_config.width,
                                   .height = recorder_config.height,
                                   .fps = recorder_config.fps,
                                   .video_bitrate_kbps =
                                       recorder_config.video_bitrate_kbps,
                                   .audio_bitrate_kbps =
                                       recorder_config.audio_bitrate_kbps,
                                   .audio_enabled =
                                       recorder_config.audio_enabled,
                                   .trust_recorder_segments = true,
                                   .available_duration_seconds =
                                       static_cast<double>(segments.size()),
                                   .validate_black_frames = false});
    if (!clip_path.has_value()) {
      Log(LogLevel::Error, kCoreContext,
          "Failed to save a clip from retained recorder segments.");
      return false;
    }

    Log(LogLevel::Info, kCoreContext,
        "Stored clip from retained recorder segments: " +
            clip_path.value().string() + ".");
    return true;
  }

  const auto save_requested_at =
      std::filesystem::file_time_type::clock::now() - std::chrono::seconds(1);

  if (auto failure = CurrentRecorderUnhealthyReason(); failure.has_value()) {
    Log(LogLevel::Error, kCoreContext,
        "Daemon recorder is not ready to save: " + failure.value());
    return false;
  }

  if (kill(pid.value(), SIGUSR1) != 0) {
    Log(LogLevel::Error, kCoreContext,
        "Failed to request native clip save from daemon pid " +
            std::to_string(pid.value()) + ".");
    return false;
  }

  Log(LogLevel::Info, kCoreContext,
      "Requested native clip save from daemon pid " +
          std::to_string(pid.value()) + ".");

  const auto timeout =
      std::chrono::seconds(std::max(30, settings_.clip_length_seconds * 3));
  const auto result = WaitForPublishedClipOrFailure(
      settings_.clip_directory, save_requested_at, timeout);
  if (result.clip.has_value()) {
    Log(LogLevel::Info, kCoreContext,
        "Saved clip: " + result.clip.value().string() + ".");
    return true;
  }

  if (result.failure.has_value()) {
    Log(LogLevel::Error, kCoreContext,
        "Daemon failed to save clip: " + result.failure.value());
    return false;
  }

  Log(LogLevel::Error, kCoreContext,
      "Timed out waiting for the daemon to publish a clip. Check " +
          DaemonLogPath().string() + " for the save failure.");
  return false;
}

bool ClipDeckCore::ChooseCapture() {
  Log(LogLevel::Info, kCoreContext, "Capture source reselection requested.");

  const auto pid = ReadPidFile(PidFilePath());
  if (!pid.has_value() || !IsProcessRunning(pid.value())) {
    if (pid.has_value()) {
      RemovePidFile(PidFilePath());
      std::error_code status_error;
      std::filesystem::remove(RecorderStatusPath(), status_error);
      std::filesystem::remove(ListenerStatusPath(), status_error);
    }

    Log(LogLevel::Info, kCoreContext,
        "ClipDeck listener is not running; starting it so the portal picker can open.");
    Start();
    return true;
  }

  if (kill(pid.value(), SIGUSR2) != 0) {
    Log(LogLevel::Error, kCoreContext,
        "Failed to request capture reselection from daemon pid " +
            std::to_string(pid.value()) + ".");
    return false;
  }

  Log(LogLevel::Info, kCoreContext,
      "Requested capture reselection from daemon pid " +
          std::to_string(pid.value()) + ".");
  return true;
}

void ClipDeckCore::Status() {
  const auto pid = ReadPidFile(PidFilePath());
  const bool running = pid.has_value() && IsProcessRunning(pid.value());

  if (pid.has_value() && !running) {
    RemovePidFile(PidFilePath());
    std::error_code status_error;
    std::filesystem::remove(RecorderStatusPath(), status_error);
    std::filesystem::remove(ListenerStatusPath(), status_error);
    Log(LogLevel::Warning, kCoreContext,
        "Removed stale ClipDeck pid file for pid " + std::to_string(pid.value()) +
            ".");
  }

  const std::string status = running ? "running" : "stopped";

  Log(LogLevel::Info, kCoreContext, "ClipDeck listener status: " + status + ".");
  ShowSettings();

  if (running) {
    PrintRecorderStatusFile();
    PrintListenerStatusFile();
  } else {
    auto recorder =
        GStreamerRecorder(BuildRecorderConfig(settings_));
    const auto recorder_status = recorder.Status();
    Log(LogLevel::Info, kCoreContext,
        "Recorder backend: " + recorder_status.backend + ".");
    Log(LogLevel::Info, kCoreContext,
        "Recorder status: stopped.");
    PrintRetainedSegmentStatus(settings_.clip_length_seconds);
  }
}

bool ClipDeckCore::Setup() {
  if (!SetupNativeRecorder(settings_)) {
    return false;
  }

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext, "Saved native recorder settings.");
    return true;
  }

  return false;
}

bool ClipDeckCore::Diagnose() {
  const bool ok = DiagnoseNativeRecorder(settings_);

  if (ok) {
    Log(LogLevel::Info, kCoreContext, "Diagnostics completed without errors.");
  }

  return ok;
}

void ClipDeckCore::ShowSettings() const {
  Log(LogLevel::Info, kCoreContext,
      "Clip length: " + std::to_string(settings_.clip_length_seconds) +
          " seconds.");
  Log(LogLevel::Info, kCoreContext,
      "Buffer safety: " + std::to_string(settings_.buffer_safety_seconds) +
          " seconds.");
  Log(LogLevel::Info, kCoreContext, "Save keybind: " + settings_.save_keybind + ".");
  Log(LogLevel::Info, kCoreContext, "Stop keybind: " + settings_.stop_keybind + ".");
  Log(LogLevel::Info, kCoreContext,
      "Clip directory: " + settings_.clip_directory.string() + ".");
  Log(LogLevel::Info, kCoreContext,
      "Capture video source: " + settings_.capture_video_source + ".");
  Log(LogLevel::Info, kCoreContext,
      "Capture audio: " +
          std::string(settings_.capture_audio_enabled ? "enabled" : "disabled") +
          ".");
  const auto resolved_audio_source = ResolveCaptureAudioSource(settings_);
  Log(LogLevel::Info, kCoreContext,
      "Capture audio source: " + settings_.capture_audio_source + ".");
  Log(LogLevel::Info, kCoreContext,
      "Resolved audio source: " +
          (resolved_audio_source.has_value() ? resolved_audio_source.value()
                                             : std::string("<none>")) +
          ".");
  Log(LogLevel::Info, kCoreContext,
      "Capture size: " + std::to_string(settings_.capture_width) + "x" +
          std::to_string(settings_.capture_height) + ".");
  Log(LogLevel::Info, kCoreContext,
      "Capture FPS: " + std::to_string(settings_.capture_fps) + ".");
  Log(LogLevel::Info, kCoreContext,
      "Video bitrate: " + std::to_string(settings_.video_bitrate_kbps) +
          " kbps.");
  Log(LogLevel::Info, kCoreContext,
      "Audio bitrate: " + std::to_string(settings_.audio_bitrate_kbps) +
          " kbps.");
  Log(LogLevel::Info, kCoreContext, "Encoder: " + settings_.encoder + ".");
}

void ClipDeckCore::SetClipLength(int seconds) {
  settings_.clip_length_seconds = seconds;

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Clip length set to " + std::to_string(seconds) + " seconds.");
  }
}

void ClipDeckCore::SetClipDirectory(
    const std::filesystem::path &clip_directory) {
  settings_.clip_directory = clip_directory.is_absolute()
                                 ? clip_directory
                                 : ProjectRootDirectory() / clip_directory;

  std::error_code error;
  std::filesystem::create_directories(settings_.clip_directory, error);

  if (error) {
    Log(LogLevel::Error, kCoreContext,
        "Failed to create clip directory: " + error.message());
    return;
  }

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Clip directory set to " + settings_.clip_directory.string() + ".");
  }
}

void ClipDeckCore::SetSaveKeybind(std::string save_keybind) {
  settings_.save_keybind = std::move(save_keybind);

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Save keybind set to " + settings_.save_keybind + ".");
  }
}

void ClipDeckCore::SetStopKeybind(std::string stop_keybind) {
  settings_.stop_keybind = std::move(stop_keybind);

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Stop keybind set to " + settings_.stop_keybind + ".");
  }
}

void ClipDeckCore::SetBufferSafety(int seconds) {
  settings_.buffer_safety_seconds = seconds;

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Buffer safety set to " + std::to_string(seconds) + " seconds.");
  }
}

void ClipDeckCore::SetCaptureVideoSource(std::string source) {
  if (source != "portal") {
    Log(LogLevel::Error, kCoreContext,
        "Native video capture uses the XDG portal picker. Use: portal.");
    return;
  }

  settings_.capture_video_source = std::move(source);

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Capture video source set to " + settings_.capture_video_source + ".");
  }
}

void ClipDeckCore::SetCaptureAudioEnabled(bool enabled) {
  settings_.capture_audio_enabled = enabled;
  if (settings_.capture_audio_source.empty()) {
    settings_.capture_audio_source = std::string(kAutomaticAudioSource);
  }

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Capture audio " + std::string(enabled ? "enabled" : "disabled") +
            ".");
  }
}

void ClipDeckCore::SetCaptureAudioAuto() {
  settings_.capture_audio_enabled = true;
  settings_.capture_audio_source = std::string(kAutomaticAudioSource);

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Capture audio set to auto. The default output monitor will be resolved when recording starts.");
  }
}

void ClipDeckCore::SetCaptureAudioSource(std::string source) {
  if (source.empty()) {
    source = std::string(kAutomaticAudioSource);
  }

  settings_.capture_audio_enabled = true;
  settings_.capture_audio_source = std::move(source);

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Capture audio source set to " +
            (settings_.capture_audio_source.empty()
                 ? std::string("<none>")
                 : settings_.capture_audio_source) +
            ".");
  }
}

void ClipDeckCore::SetCaptureSize(int width, int height) {
  settings_.capture_width = width;
  settings_.capture_height = height;

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Capture size set to " + std::to_string(width) + "x" +
            std::to_string(height) + ".");
  }
}

void ClipDeckCore::SetCaptureFps(int fps) {
  settings_.capture_fps = fps;

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Capture FPS set to " + std::to_string(fps) + ".");
  }
}

void ClipDeckCore::SetVideoBitrate(int bitrate_kbps) {
  settings_.video_bitrate_kbps = bitrate_kbps;

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Video bitrate set to " + std::to_string(bitrate_kbps) + " kbps.");
  }
}

void ClipDeckCore::SetAudioBitrate(int bitrate_kbps) {
  settings_.audio_bitrate_kbps = bitrate_kbps;

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Audio bitrate set to " + std::to_string(bitrate_kbps) + " kbps.");
  }
}

void ClipDeckCore::SetEncoder(std::string encoder) {
  if (encoder != "openh264" && encoder != "x264") {
    Log(LogLevel::Error, kCoreContext,
        "Encoder must be openh264 or x264.");
    return;
  }

  settings_.encoder = std::move(encoder);

  if (SaveSettings()) {
    Log(LogLevel::Info, kCoreContext,
        "Encoder set to " + settings_.encoder + ".");
  }
}

bool ClipDeckCore::IsRunning() const {
  const auto pid = ReadPidFile(PidFilePath());
  return pid.has_value() && IsProcessRunning(pid.value());
}

ListenerConfig ClipDeckCore::BuildListenerConfig() const {
  return ListenerConfig{.save_keybind = settings_.save_keybind,
                        .stop_keybind = settings_.stop_keybind};
}

bool ClipDeckCore::SaveSettings() const {
  if (!settings_store_.Save(settings_)) {
    Log(LogLevel::Error, kCoreContext, "Failed to save settings.");
    return false;
  }

  return true;
}

} // namespace clipdeck
