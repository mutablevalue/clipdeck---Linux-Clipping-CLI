#pragma once

#include "settings/settings_store.hpp"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace clipdeck {

class FeedbackNotifier {
public:
  explicit FeedbackNotifier(const ClipDeckSettings &settings);

  FeedbackNotifier(const FeedbackNotifier &) = delete;
  FeedbackNotifier &operator=(const FeedbackNotifier &) = delete;

  void NotifyKeybindAccepted();

#ifdef CLIPDECK_ENABLE_RECORDER_TEST_HOOKS
  void SetPlayerForTest(std::string player);
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  LastPlayedAtForTest() const;
#endif

private:
  [[nodiscard]] bool RateLimitPassed(
      std::chrono::steady_clock::time_point now) const;

  bool enabled_ = false;
  std::filesystem::path sound_path_;
  std::optional<std::string> player_;
  mutable std::mutex mutex_;
  std::optional<std::chrono::steady_clock::time_point> last_played_at_;
};

} // namespace clipdeck
