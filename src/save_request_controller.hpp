#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace clipdeck {

enum class SaveSource { Keybind, Manual };

enum class SaveState { Idle, Saving, Pending };

struct SaveRequestControllerConfig {
  std::chrono::milliseconds keybind_debounce{1000};
};

struct SaveControllerStatus {
  bool saving = false;
  bool pending = false;
  std::optional<SaveSource> last_request_source;
};

[[nodiscard]] std::string_view SaveSourceName(SaveSource source);

class SaveRequestController {
public:
  using SaveCallback = std::function<void(SaveSource source)>;

  explicit SaveRequestController(
      SaveCallback save_callback,
      SaveRequestControllerConfig config = SaveRequestControllerConfig{});
  ~SaveRequestController();

  SaveRequestController(const SaveRequestController &) = delete;
  SaveRequestController &operator=(const SaveRequestController &) = delete;

  bool RequestSave(SaveSource source);
  void Stop();
  [[nodiscard]] SaveControllerStatus Status() const;

private:
  void WorkerLoop(std::stop_token stop_token);
  [[nodiscard]] bool KeybindDebouncePassed(
      std::chrono::steady_clock::time_point now) const;

  SaveCallback save_callback_;
  SaveRequestControllerConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  SaveState state_ = SaveState::Idle;
  std::optional<SaveSource> active_source_;
  std::optional<SaveSource> pending_source_;
  std::optional<SaveSource> last_request_source_;
  std::optional<std::chrono::steady_clock::time_point> last_keybind_request_at_;
  bool stopped_ = false;
  std::jthread worker_thread_;
};

} // namespace clipdeck
