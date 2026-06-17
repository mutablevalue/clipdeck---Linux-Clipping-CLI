#include "../utils/runtime_paths.hpp"
#include "../settings/settings_store.hpp"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>

TEST(RuntimePathsTest, UsesConfiguredOutputDirectory) {
  const auto output_directory =
      std::filesystem::temp_directory_path() / "clipdeck-runtime-path-test";
  setenv("CLIPDECK_OUTPUT_DIR", output_directory.c_str(), 1);

  EXPECT_EQ(clipdeck::OutputDirectory(), output_directory);
  EXPECT_EQ(clipdeck::SettingsPath(), output_directory / "settings.conf");
  EXPECT_EQ(clipdeck::RuntimeDirectory(), output_directory / "runtime");
  EXPECT_EQ(clipdeck::SegmentDirectory(),
            output_directory / "runtime" / "segments");
  EXPECT_EQ(clipdeck::ListenerStatusPath(),
            output_directory / "runtime" / "listener-status.conf");
  EXPECT_EQ(clipdeck::DefaultFeedbackSoundPath(),
            output_directory / "runtime" / "assets" / "sounds" /
                "clip-accepted.wav");

  clipdeck::ClipDeckSettings settings;
  EXPECT_EQ(clipdeck::ResolveFeedbackSoundPath(settings),
            clipdeck::DefaultFeedbackSoundPath());

  unsetenv("CLIPDECK_OUTPUT_DIR");
}
