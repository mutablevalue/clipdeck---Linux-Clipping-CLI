#include "daemon_listener.hpp"

#include "keybind.hpp"
#include "../utils/file_descriptor.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <linux/input.h>
#include <poll.h>
#include <ranges>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view kListenerContext = "daemon-listener";
constexpr auto kDeviceRetryInterval = std::chrono::seconds(5);
constexpr auto kPollTimeout = std::chrono::milliseconds(250);

constexpr int kBitsPerUnsignedLong = static_cast<int>(
    sizeof(unsigned long) * std::numeric_limits<unsigned char>::digits);
constexpr std::size_t kKeyBitsetSize =
    static_cast<std::size_t>((KEY_MAX + kBitsPerUnsignedLong) /
                             kBitsPerUnsignedLong);

bool TestBit(const std::array<unsigned long, kKeyBitsetSize> &bits, int bit) {
  if (bit < 0 || bit > KEY_MAX) {
    return false;
  }

  const auto index = static_cast<std::size_t>(bit / kBitsPerUnsignedLong);
  const auto offset = static_cast<unsigned int>(bit % kBitsPerUnsignedLong);
  return (bits[index] & (1UL << offset)) != 0;
}

bool DeviceExposesKeybindRequirements(
    int descriptor, const std::vector<clipdeck::KeyRequirement> &requirements) {
  std::array<unsigned long, kKeyBitsetSize> key_bits{};
  if (ioctl(descriptor, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits.data()) <
      0) {
    return false;
  }

  return std::ranges::all_of(requirements, [&key_bits](const auto &requirement) {
    return std::ranges::any_of(requirement.alternatives, [&key_bits](int key_code) {
      return TestBit(key_bits, key_code);
    });
  });
}

std::optional<std::vector<clipdeck::KeyRequirement>>
ConfiguredKeybindRequirements(const clipdeck::ListenerConfig &config,
                              std::string &last_error) {
  std::vector<clipdeck::KeyRequirement> requirements;

  const auto save_requirements =
      clipdeck::ParseKeybindRequirements(config.save_keybind);
  if (!save_requirements.has_value()) {
    last_error =
        "Configured save keybind is not supported by the Linux input listener.";
    return std::nullopt;
  }
  requirements.insert(requirements.end(), save_requirements->begin(),
                      save_requirements->end());

  const auto stop_requirements =
      clipdeck::ParseKeybindRequirements(config.stop_keybind);
  if (!stop_requirements.has_value()) {
    last_error =
        "Configured stop keybind is not supported by the Linux input listener.";
    return std::nullopt;
  }
  requirements.insert(requirements.end(), stop_requirements->begin(),
                      stop_requirements->end());

  return requirements;
}

} // namespace

namespace clipdeck {

DaemonListener::DaemonListener(ListenerConfig config)
    : config_(std::move(config)),
      keybinds_({KeybindActionState{.action = "save",
                                    .keybind = config_.save_keybind,
                                    .was_down = false,
                                    .last_detected_at = std::nullopt},
                 KeybindActionState{.action = "stop",
                                    .keybind = config_.stop_keybind,
                                    .was_down = false,
                                    .last_detected_at = std::nullopt}}),
      status_(ListenerStatus{.running = false,
                             .save_keybind = config_.save_keybind,
                             .stop_keybind = config_.stop_keybind,
                             .input_directory = config_.input_directory,
                             .scanned_devices = 0,
                             .readable_devices = 0,
                             .accepted_keyboard_devices = 0,
                             .last_error = "",
                             .last_keybind_detected = std::nullopt}) {}

DaemonListener::~DaemonListener() { Stop(); }

void DaemonListener::Start() {
  if (running_) {
    Log(LogLevel::Warning, kListenerContext,
        "Daemon listener is already running.");
    return;
  }

  running_ = true;
  listener_thread_ =
      std::jthread([this](std::stop_token stop_token) { ListenLoop(stop_token); });

  Log(LogLevel::Info, kListenerContext,
      "Started global Linux input keybind listener. Save keybind: " +
          config_.save_keybind + ". Stop keybind: " + config_.stop_keybind +
          ".");
}

void DaemonListener::Stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  if (listener_thread_.joinable()) {
    listener_thread_.request_stop();
    listener_thread_.join();
  }

  Log(LogLevel::Info, kListenerContext, "Stopped keybind listener.");
}

bool DaemonListener::IsRunning() const { return running_; }

void DaemonListener::SetKeybindCallback(KeybindCallback callback) {
  keybind_callback_ = std::move(callback);
}

ListenerStatus DaemonListener::Status() const {
  std::scoped_lock lock(status_mutex_);
  ListenerStatus status = status_;
  status.running = running_;
  return status;
}

#ifdef CLIPDECK_ENABLE_RECORDER_TEST_HOOKS
void DaemonListener::InjectInputEventForTest(int event_type, int event_code,
                                             int event_value) {
  HandleInputEvent(event_type, event_code, event_value);
}
#endif

void DaemonListener::ListenLoop(std::stop_token stop_token) {
  auto next_device_retry = std::chrono::steady_clock::now();

  while (running_ && !stop_token.stop_requested()) {
    if (std::chrono::steady_clock::now() >= next_device_retry) {
      RescanInputDevices();
      next_device_retry =
          std::chrono::steady_clock::now() + kDeviceRetryInterval;
    }

    if (input_devices_.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    std::vector<pollfd> poll_descriptors;
    poll_descriptors.reserve(input_devices_.size());

    for (const auto &device : input_devices_) {
      poll_descriptors.push_back({device.descriptor.Get(), POLLIN, 0});
    }

    const int ready = poll(poll_descriptors.data(), poll_descriptors.size(),
                           static_cast<int>(kPollTimeout.count()));
    if (ready <= 0) {
      continue;
    }

    std::vector<std::size_t> broken_indices;

    for (std::size_t index = 0; index < poll_descriptors.size(); ++index) {
      const auto &descriptor = poll_descriptors[index];
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        broken_indices.push_back(index);
        continue;
      }

      if ((descriptor.revents & POLLIN) == 0) {
        continue;
      }

      input_event event{};
      const ssize_t bytes_read = read(descriptor.fd, &event, sizeof(event));

      if (bytes_read == sizeof(event)) {
        HandleInputEvent(event.type, event.code, event.value);
      } else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR) {
        broken_indices.push_back(index);
      }
    }

    RemoveBrokenDevices(broken_indices);
  }

  running_ = false;
}

void DaemonListener::RescanInputDevices() {
  int scanned_devices = 0;
  int readable_devices = static_cast<int>(input_devices_.size());
  int accepted_keyboard_devices = static_cast<int>(input_devices_.size());
  std::string last_error;

  std::string keybind_error;
  const auto requirements = ConfiguredKeybindRequirements(config_, keybind_error);
  if (!requirements.has_value()) {
    UpdateStatusCounts(0, readable_devices, accepted_keyboard_devices,
                       keybind_error);
    return;
  }

  std::error_code error;
  if (!std::filesystem::exists(config_.input_directory, error)) {
    const std::string message =
        "Input directory does not exist: " + config_.input_directory.string();
    Log(LogLevel::Warning, kListenerContext,
        message);
    UpdateStatusCounts(0, readable_devices, accepted_keyboard_devices, message);
    return;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(config_.input_directory, error)) {
    if (error) {
      last_error = "Failed to scan input directory " +
                   config_.input_directory.string() + ": " + error.message();
      break;
    }

    const std::string filename = entry.path().filename().string();
    if (!filename.starts_with("event")) {
      continue;
    }
    ++scanned_devices;

    const bool already_open =
        std::ranges::any_of(input_devices_, [&entry](const auto &device) {
          return device.path == entry.path();
        });
    if (already_open) {
      continue;
    }

    const int descriptor = open(entry.path().c_str(), O_RDONLY | O_NONBLOCK);
    if (descriptor >= 0) {
      ++readable_devices;
      FileDescriptor device_descriptor(descriptor);
      if (!DeviceExposesKeybindRequirements(
              device_descriptor.Get(), requirements.value())) {
        last_error = entry.path().string() +
                     ": readable but does not expose the configured keybinds";
        continue;
      }

      ++accepted_keyboard_devices;
      input_devices_.push_back(InputDevice{.path = entry.path(),
                                           .descriptor =
                                               std::move(device_descriptor)});
      Log(LogLevel::Debug, kListenerContext,
          "Accepted global input device: " + entry.path().string() + ".");
      continue;
    }

    last_error = entry.path().string() + ": " + std::strerror(errno);
  }

  if (error && last_error.empty()) {
    last_error = "Failed to scan input directory " +
                 config_.input_directory.string() + ": " + error.message();
  }

  UpdateStatusCounts(scanned_devices, readable_devices, accepted_keyboard_devices,
                     last_error);

  if (input_devices_.empty()) {
    Log(LogLevel::Warning, kListenerContext,
        "No usable global input event devices found under /dev/input/event*. "
        "Add the user to the input group, install a udev rule, or use a polkit helper for input access. "
        "clipdeck setup terminal keybind capture working does not prove runtime global daemon listening works. Last error: " +
            (last_error.empty() ? std::string("none") : last_error));
    return;
  }

  Log(LogLevel::Info, kListenerContext,
      "Scanned " + std::to_string(scanned_devices) + " input event devices, " +
          std::to_string(readable_devices) + " readable, " +
          std::to_string(accepted_keyboard_devices) +
          " accepted for the configured keybinds.");
}

void DaemonListener::RemoveBrokenDevices(
    const std::vector<std::size_t> &broken_indices) {
  if (broken_indices.empty()) {
    return;
  }

  std::vector<std::size_t> unique_indices = broken_indices;
  std::ranges::sort(unique_indices);
  const auto [first_duplicate, end] = std::ranges::unique(unique_indices);
  unique_indices.erase(first_duplicate, end);

  for (const auto index : unique_indices | std::views::reverse) {
    if (index >= input_devices_.size()) {
      continue;
    }

    Log(LogLevel::Warning, kListenerContext,
        "Removed unavailable input device: " +
            input_devices_[index].path.string() + ".");
    input_devices_.erase(input_devices_.begin() +
                         static_cast<std::ptrdiff_t>(index));
  }

  ResetKeyState();
  UpdateStatusCounts(Status().scanned_devices,
                     static_cast<int>(input_devices_.size()),
                     static_cast<int>(input_devices_.size()),
                     "One or more input devices became unavailable.");
}

void DaemonListener::HandleInputEvent(int event_type, int event_code,
                                      int event_value) {
  if (event_type != EV_KEY) {
    return;
  }

  if (event_value == 2) {
    return;
  }

  if (event_value == 0) {
    std::erase(pressed_key_codes_, event_code);
  } else if (event_value == 1 &&
      std::ranges::find(pressed_key_codes_, event_code) ==
          pressed_key_codes_.end()) {
    pressed_key_codes_.push_back(event_code);
  } else if (event_value != 1) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  for (auto &keybind : keybinds_) {
    const bool is_down = KeybindIsPressed(keybind.keybind);
    if (is_down && !keybind.was_down && KeybindDebouncePassed(keybind, now)) {
      keybind.last_detected_at = now;
      MarkKeybindDetected();
      Log(LogLevel::Info, kListenerContext,
          "Global " + keybind.action + " keybind detected.");

      if (keybind_callback_) {
        keybind_callback_(keybind.action);
      }
    }

    keybind.was_down = is_down;
  }
}

void DaemonListener::ResetKeyState() {
  pressed_key_codes_.clear();
  for (auto &keybind : keybinds_) {
    keybind.was_down = false;
  }
}

bool DaemonListener::KeybindIsPressed(std::string_view keybind) const {
  const auto requirements = ParseKeybindRequirements(keybind);

  if (!requirements.has_value()) {
    return false;
  }

  return std::ranges::all_of(requirements.value(), [this](const auto &requirement) {
    return std::ranges::any_of(requirement.alternatives, [this](int key_code) {
      return std::ranges::find(pressed_key_codes_, key_code) !=
             pressed_key_codes_.end();
    });
  });
}

bool DaemonListener::KeybindDebouncePassed(
    const KeybindActionState &keybind,
    std::chrono::steady_clock::time_point now) const {
  return !keybind.last_detected_at.has_value() ||
         now - keybind.last_detected_at.value() >= config_.keybind_debounce;
}

void DaemonListener::UpdateStatusCounts(int scanned_devices, int readable_devices,
                                        int accepted_keyboard_devices,
                                        std::string last_error) {
  std::scoped_lock lock(status_mutex_);
  status_.running = running_;
  status_.save_keybind = config_.save_keybind;
  status_.stop_keybind = config_.stop_keybind;
  status_.input_directory = config_.input_directory;
  status_.scanned_devices = scanned_devices;
  status_.readable_devices = readable_devices;
  status_.accepted_keyboard_devices = accepted_keyboard_devices;
  status_.last_error = std::move(last_error);
}

void DaemonListener::MarkKeybindDetected() {
  std::scoped_lock lock(status_mutex_);
  status_.last_keybind_detected = std::chrono::system_clock::now();
}

} // namespace clipdeck
