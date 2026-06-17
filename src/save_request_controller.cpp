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

SaveRequestController::SaveRequestController(
    SaveCallback save_callback, SaveRequestControllerConfig config)
    : save_callback_(std::move(save_callback)), config_(config),
      worker_thread_([this](std::stop_token stop_token) {
        WorkerLoop(stop_token);
      }) {}

SaveRequestController::~SaveRequestController() {
  Stop();
}

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

bool SaveRequestController::RequestSave(SaveSource source) {
  const auto now = std::chrono::steady_clock::now();
  std::scoped_lock lock(mutex_);

  if (stopped_) {
    return false;
  }

  if (source == SaveSource::Keybind && !KeybindDebouncePassed(now)) {
    return false;
  }

  if (source == SaveSource::Keybind) {
    last_keybind_request_at_ = now;
  }

  last_request_source_ = source;

  if (state_ == SaveState::Idle) {
    state_ = SaveState::Saving;
    active_source_ = source;
    condition_.notify_one();
    return true;
  }

  if (state_ == SaveState::Saving) {
    state_ = SaveState::Pending;
    pending_source_ = source;
    return true;
  }

  return false;
}

SaveControllerStatus SaveRequestController::Status() const {
  std::scoped_lock lock(mutex_);
  return SaveControllerStatus{.saving = state_ == SaveState::Saving ||
                                        state_ == SaveState::Pending,
                              .pending = state_ == SaveState::Pending,
                              .last_request_source = last_request_source_};
}

void SaveRequestController::WorkerLoop(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    std::optional<SaveSource> source;

    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, stop_token, [this] {
        return active_source_.has_value();
      });

      if (stop_token.stop_requested() && !active_source_.has_value()) {
        return;
      }

      source = active_source_;
      active_source_.reset();
    }

    if (source.has_value()) {
      save_callback_(source.value());
    }

    {
      std::scoped_lock lock(mutex_);
      if (stop_token.stop_requested() || stopped_) {
        state_ = SaveState::Idle;
        active_source_.reset();
        pending_source_.reset();
        return;
      }

      if (state_ == SaveState::Pending) {
        state_ = SaveState::Saving;
        active_source_ = pending_source_;
        pending_source_.reset();
        condition_.notify_one();
      } else {
        state_ = SaveState::Idle;
      }
    }
  }
}

bool SaveRequestController::KeybindDebouncePassed(
    std::chrono::steady_clock::time_point now) const {
  return !last_keybind_request_at_.has_value() ||
         now - last_keybind_request_at_.value() >= config_.keybind_debounce;
}

} // namespace clipdeck
