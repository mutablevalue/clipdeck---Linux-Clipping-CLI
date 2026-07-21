#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace clipdeck {

enum class SaveSource { Keybind, Manual };

enum class SaveRequestResult { Accepted, Debounced, QueueFull, Stopped };

struct SaveRequest {
  std::uint64_t id = 0;
  SaveSource source = SaveSource::Manual;
  std::chrono::steady_clock::time_point requested_at;
};

struct SaveRequestControllerConfig {
  std::chrono::milliseconds keybind_debounce{1000};
  std::size_t queue_capacity = 10;
};

struct SaveControllerStatus {
  bool saving = false;
  bool pending = false;
  std::size_t queued_count = 0;
  std::size_t queue_capacity = 10;
  std::optional<SaveSource> last_request_source;
};

[[nodiscard]] std::string_view SaveSourceName(SaveSource source);

class SaveRequestController {
public:
  using SaveCallback = std::function<void(const SaveRequest &request)>;
  using AcceptCallback = std::function<void(const SaveRequest &request)>;

  explicit SaveRequestController(
      SaveCallback save_callback,
      SaveRequestControllerConfig config = SaveRequestControllerConfig{},
      AcceptCallback accept_callback = {});
  ~SaveRequestController();

  SaveRequestController(const SaveRequestController &) = delete;
  SaveRequestController &operator=(const SaveRequestController &) = delete;

  SaveRequestResult RequestSave(SaveSource source);
  void Stop();
  [[nodiscard]] SaveControllerStatus Status() const;

private:
  void WorkerLoop(std::stop_token stop_token);
  [[nodiscard]] bool
  KeybindDebouncePassed(std::chrono::steady_clock::time_point now) const;

  SaveCallback save_callback_;
  AcceptCallback accept_callback_;
  SaveRequestControllerConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable_any condition_;
  std::deque<SaveRequest> requests_;
  bool saving_ = false;
  std::uint64_t next_request_id_ = 1;
  std::optional<SaveSource> last_request_source_;
  std::optional<std::chrono::steady_clock::time_point> last_keybind_request_at_;
  bool stopped_ = false;
  std::jthread worker_thread_;
};

} // namespace clipdeck
