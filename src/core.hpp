#pragma once

#include "listener/daemon_listener.hpp"
#include "settings/settings_store.hpp"

#include <filesystem>
#include <string>

namespace clipdeck {

class ClipDeckCore {
public:
  ClipDeckCore();

  void Start();
  int RunListener();
  void Stop();
  void Restart();
  bool Save();
  bool ChooseCapture();
  void Status();
  bool Setup();
  bool Diagnose();
  void ShowSettings() const;
  void SetClipLength(int seconds);
  void SetClipDirectory(const std::filesystem::path &clip_directory);
  void SetSaveKeybind(std::string save_keybind);
  void SetStopKeybind(std::string stop_keybind);
  void SetBufferSafety(int seconds);
  void SetCaptureVideoSource(std::string source);
  void SetCaptureAudioEnabled(bool enabled);
  void SetCaptureAudioAuto();
  void SetCaptureAudioSource(std::string source);
  void SetCaptureSize(int width, int height);
  void SetCaptureFps(int fps);
  void SetVideoBitrate(int bitrate_kbps);
  void SetAudioBitrate(int bitrate_kbps);
  void SetMaxClipSize(int size_mb);
  void SetEncoder(std::string encoder);
  [[nodiscard]] bool IsRunning() const;

private:
  [[nodiscard]] ListenerConfig BuildListenerConfig() const;
  bool SaveSettings() const;

  SettingsStore settings_store_;
  ClipDeckSettings settings_;
};

} // namespace clipdeck
