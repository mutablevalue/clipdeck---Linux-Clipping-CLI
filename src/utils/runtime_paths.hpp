#pragma once

#include <filesystem>

namespace clipdeck {

struct ClipDeckSettings;

[[nodiscard]] std::filesystem::path ProjectRootDirectory();
[[nodiscard]] std::filesystem::path OutputDirectory();
[[nodiscard]] std::filesystem::path SettingsPath();
[[nodiscard]] std::filesystem::path RuntimeDirectory();
[[nodiscard]] std::filesystem::path SegmentDirectory();
[[nodiscard]] std::filesystem::path PidFilePath();
[[nodiscard]] std::filesystem::path DaemonLogPath();
[[nodiscard]] std::filesystem::path RecorderStatusPath();
[[nodiscard]] std::filesystem::path ListenerStatusPath();
[[nodiscard]] std::filesystem::path DefaultFeedbackSoundPath();
[[nodiscard]] std::filesystem::path ResolveFeedbackSoundPath(
    const ClipDeckSettings &settings);

} // namespace clipdeck
