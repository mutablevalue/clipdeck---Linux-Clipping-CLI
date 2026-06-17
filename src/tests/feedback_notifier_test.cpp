#include "../feedback_notifier.hpp"
#include "../utils/runtime_paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

namespace {

std::filesystem::path TestDirectory(std::string_view name) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("clipdeck-" + std::string(name) + "-" + std::to_string(getpid()) +
          "-" + std::to_string(suffix));
}

} // namespace

TEST(FeedbackNotifierTest, DefaultSoundPathResolvesAndGenerates) {
  const auto output_directory = TestDirectory("feedback-default");
  setenv("CLIPDECK_OUTPUT_DIR", output_directory.c_str(), 1);

  clipdeck::ClipDeckSettings settings;
  settings.feedback_sound_enabled = true;
  settings.feedback_sound_path.clear();

  clipdeck::FeedbackNotifier notifier(settings);

  EXPECT_TRUE(std::filesystem::exists(clipdeck::DefaultFeedbackSoundPath()));

  unsetenv("CLIPDECK_OUTPUT_DIR");
  std::error_code error;
  std::filesystem::remove_all(output_directory, error);
}

TEST(FeedbackNotifierTest, MissingCustomSoundDoesNotCrash) {
  clipdeck::ClipDeckSettings settings;
  settings.feedback_sound_enabled = true;
  settings.feedback_sound_path = TestDirectory("feedback-missing") / "missing.wav";

  clipdeck::FeedbackNotifier notifier(settings);

  notifier.NotifyKeybindAccepted();
  EXPECT_FALSE(notifier.LastPlayedAtForTest().has_value());
}

TEST(FeedbackNotifierTest, DisabledSettingSuppressesPlayback) {
  clipdeck::ClipDeckSettings settings;
  settings.feedback_sound_enabled = false;

  clipdeck::FeedbackNotifier notifier(settings);
  notifier.NotifyKeybindAccepted();

  EXPECT_FALSE(notifier.LastPlayedAtForTest().has_value());
}

TEST(FeedbackNotifierTest, PlaybackIsRateLimited) {
  const auto output_directory = TestDirectory("feedback-rate");
  setenv("CLIPDECK_OUTPUT_DIR", output_directory.c_str(), 1);

  clipdeck::ClipDeckSettings settings;
  settings.feedback_sound_enabled = true;
  settings.feedback_sound_path.clear();

  clipdeck::FeedbackNotifier notifier(settings);
  notifier.SetPlayerForTest("/bin/true");
  notifier.NotifyKeybindAccepted();
  const auto first_play = notifier.LastPlayedAtForTest();
  notifier.NotifyKeybindAccepted();
  const auto second_play = notifier.LastPlayedAtForTest();

  ASSERT_TRUE(first_play.has_value());
  ASSERT_TRUE(second_play.has_value());
  EXPECT_EQ(first_play.value(), second_play.value());

  unsetenv("CLIPDECK_OUTPUT_DIR");
  std::error_code error;
  std::filesystem::remove_all(output_directory, error);
}
