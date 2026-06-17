#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

namespace clipdeck {

[[nodiscard]] bool IsFinalizedMp4Segment(const std::filesystem::path &path);

[[nodiscard]] std::optional<double>
Mp4DurationSeconds(const std::filesystem::path &path);

[[nodiscard]] std::vector<std::filesystem::path> SelectRecentSegmentsForDuration(
    const std::filesystem::path &segment_directory,
    std::chrono::seconds target_duration);
[[nodiscard]] std::vector<std::filesystem::path> SelectRecentSegmentsForDuration(
    const std::filesystem::path &segment_directory,
    std::chrono::seconds target_duration, bool audio_expected);
[[nodiscard]] std::vector<std::filesystem::path>
SelectRecentFinalizedSegmentsByCount(
    const std::filesystem::path &segment_directory, std::size_t max_count);

[[nodiscard]] std::size_t
FinalizedSegmentCount(const std::filesystem::path &segment_directory);
[[nodiscard]] double FinalizedSegmentDurationSeconds(
    const std::filesystem::path &segment_directory, bool audio_expected);

} // namespace clipdeck
