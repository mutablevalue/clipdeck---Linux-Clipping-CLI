#pragma once

#include "../utils/file_descriptor.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace clipdeck {

struct ListenerConfig {
  std::string save_keybind = "Ctrl+Z+P";
  std::string stop_keybind = "Ctrl+Z+O";
  std::filesystem::path input_directory = "/dev/input";
  std::chrono::milliseconds keybind_debounce{1000};
};

struct ListenerStatus {
  bool running = false;
  std::string save_keybind = "Ctrl+Z+P";
  std::string stop_keybind = "Ctrl+Z+O";
  std::filesystem::path input_directory = "/dev/input";
  int scanned_devices = 0;
  int readable_devices = 0;
  int accepted_keyboard_devices = 0;
  std::string last_error;
  std::optional<std::chrono::system_clock::time_point> last_keybind_detected;
};

class DaemonListener {
public:
  using KeybindCallback = std::function<void(std::string_view action)>;

  explicit DaemonListener(ListenerConfig config = {});
  ~DaemonListener();

  DaemonListener(const DaemonListener &) = delete;
  DaemonListener &operator=(const DaemonListener &) = delete;

  void Start();
  void Stop();
  [[nodiscard]] bool IsRunning() const;
  void SetKeybindCallback(KeybindCallback callback);
  [[nodiscard]] ListenerStatus Status() const;

#ifdef CLIPDECK_ENABLE_RECORDER_TEST_HOOKS
  void InjectInputEventForTest(int event_type, int event_code, int event_value);
#endif

private:
  struct InputDevice {
    std::filesystem::path path;
    FileDescriptor descriptor;
  };

  struct KeybindActionState {
    std::string action;
    std::string keybind;
    bool was_down = false;
    std::optional<std::chrono::steady_clock::time_point> last_detected_at;
  };

  void ListenLoop(std::stop_token stop_token);
  void RescanInputDevices();
  void RemoveBrokenDevices(const std::vector<std::size_t> &broken_indices);
  void HandleInputEvent(int event_type, int event_code, int event_value);
  void ResetKeyState();
  [[nodiscard]] bool KeybindIsPressed(std::string_view keybind) const;
  [[nodiscard]] bool KeybindDebouncePassed(
      const KeybindActionState &keybind,
      std::chrono::steady_clock::time_point now) const;
  void UpdateStatusCounts(int scanned_devices, int readable_devices,
                          int accepted_keyboard_devices,
                          std::string last_error);
  void MarkKeybindDetected();

  ListenerConfig config_;
  KeybindCallback keybind_callback_;
  std::atomic_bool running_{false};
  std::jthread listener_thread_;
  std::vector<InputDevice> input_devices_;
  std::vector<int> pressed_key_codes_;
  std::vector<KeybindActionState> keybinds_;
  mutable std::mutex status_mutex_;
  ListenerStatus status_;
};

} // namespace clipdeck
