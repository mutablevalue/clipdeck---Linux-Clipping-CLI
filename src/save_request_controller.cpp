#include "save_request_controller.hpp"

#include <utility>

namespace clipdeck {

std::string_view SaveSourceName(SaveSource source) {
  switch (source) {
  case SaveSource::Keybind:
    return "keybind";
  case SaveSource::Manual:
    return "manual";
  }

  return "unknown";
}

SaveRequestController::SaveRequestController(SaveCallback save_callback,
                                             SaveRequestControllerConfig config,
                                             AcceptCallback accept_callback)
    : save_callback_(std::move(save_callback)),
      accept_callback_(std::move(accept_callback)), config_(config),
      worker_thread_(
          [this](std::stop_token stop_token) { WorkerLoop(stop_token); }) {}

SaveRequestController::~SaveRequestController() { Stop(); }

void SaveRequestController::Stop() {
  {
    std::scoped_lock lock(mutex_);
    stopped_ = true;
  }

  worker_thread_.request_stop();
  condition_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

SaveRequestResult SaveRequestController::RequestSave(SaveSource source) {
  const auto now = std::chrono::steady_clock::now();
  SaveRequest request;
  {
    std::scoped_lock lock(mutex_);

    if (stopped_) {
      return SaveRequestResult::Stopped;
    }

    if (source == SaveSource::Keybind && !KeybindDebouncePassed(now)) {
      return SaveRequestResult::Debounced;
    }

    const std::size_t outstanding = requests_.size() + (saving_ ? 1U : 0U);
    if (outstanding >= config_.queue_capacity) {
      return SaveRequestResult::QueueFull;
    }

    if (source == SaveSource::Keybind) {
      last_keybind_request_at_ = now;
    }

    last_request_source_ = source;
    request = SaveRequest{
        .id = next_request_id_++, .source = source, .requested_at = now};
    requests_.push_back(request);
  }

  if (accept_callback_) {
    accept_callback_(request);
  }
  condition_.notify_one();
  return SaveRequestResult::Accepted;
}

SaveControllerStatus SaveRequestController::Status() const {
  std::scoped_lock lock(mutex_);
  const std::size_t outstanding = requests_.size() + (saving_ ? 1U : 0U);
  return SaveControllerStatus{.saving = outstanding > 0,
                              .pending = requests_.size() > 0,
                              .queued_count = outstanding,
                              .queue_capacity = config_.queue_capacity,
                              .last_request_source = last_request_source_};
}

void SaveRequestController::WorkerLoop(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    std::optional<SaveRequest> request;

    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, stop_token, [this] { return !requests_.empty(); });

      if (stop_token.stop_requested()) {
        requests_.clear();
        return;
      }

      request = requests_.front();
      requests_.pop_front();
      saving_ = true;
    }

    if (request.has_value()) {
      save_callback_(request.value());
    }

    {
      std::scoped_lock lock(mutex_);
      if (stop_token.stop_requested() || stopped_) {
        saving_ = false;
        requests_.clear();
        return;
      }
      saving_ = false;
    }
  }
}

bool SaveRequestController::KeybindDebouncePassed(
    std::chrono::steady_clock::time_point now) const {
  return !last_keybind_request_at_.has_value() ||
         now - last_keybind_request_at_.value() >= config_.keybind_debounce;
}

} // namespace clipdeck
