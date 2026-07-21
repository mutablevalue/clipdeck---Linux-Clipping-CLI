#pragma once

#include "../save_request_controller.hpp"
#include "clip_muxer.hpp"
#include "recorder_backend.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace clipdeck {

class GStreamerRecorder final : public RecorderBackend {
public:
  explicit GStreamerRecorder(RecorderConfig config);
  ~GStreamerRecorder() override;

  GStreamerRecorder(const GStreamerRecorder &) = delete;
  GStreamerRecorder &operator=(const GStreamerRecorder &) = delete;

  bool Start() override;
  void Stop() override;
  bool SaveClip() override;
  bool SaveClip(const SaveRequest &request);
  void MarkClipBoundary(const SaveRequest &request);
  [[nodiscard]] RecorderStatus Status() const override;

#if defined(CLIPDECK_ENABLE_RECORDER_TEST_HOOKS)
  void SetPortalTestTargetObject(int fd, std::uint32_t node_id,
                                 std::string target_object);
  void SetPortalTestPath(int fd, std::uint32_t node_id);
  void SetPortalTestCaptureSourceType(std::string source_type);
  [[nodiscard]] std::string BuildPipelineDescriptionForTest() const;
#endif

private:
  void MonitorBus(std::stop_token stop_token);
  void CleanSegmentDirectory() const;
  void SplitCurrentSegment() const;
  void SplitAtRunningTime(std::uint64_t running_time_ns) const;
  [[nodiscard]] bool
  WaitForAnyFinalizedSegment(std::chrono::milliseconds timeout) const;
  [[nodiscard]] std::vector<std::filesystem::path>
  SelectSegmentsForClip() const;
  [[nodiscard]] std::size_t SegmentBytes() const;
  [[nodiscard]] std::size_t SegmentCount() const;
  [[nodiscard]] std::string BuildPipelineDescription() const;
  void SetMessage(std::string message, bool healthy);

  struct FragmentBoundary {
    std::filesystem::path path;
    std::uint64_t start_running_time_ns = 0;
    std::uint64_t end_running_time_ns = 0;
  };
  void RecordFragmentMessage(void *message);
  void PruneFragmentsLocked(std::uint64_t latest_running_time_ns);
  [[nodiscard]] std::optional<std::uint64_t> PipelineRunningTimeNs() const;

  RecorderConfig config_;
  ClipMuxer muxer_;
  std::filesystem::path segment_directory_;
  mutable std::mutex state_mutex_;
  std::mutex save_mutex_;
  mutable std::mutex fragment_mutex_;
  std::condition_variable fragment_condition_;
  std::vector<FragmentBoundary> fragments_;
  std::map<std::filesystem::path, std::uint64_t> open_fragments_;
  std::map<std::uint64_t, std::uint64_t> request_cutoffs_;
  std::atomic_bool running_{false};
  bool healthy_ = false;
  std::string message_ = "not started";
  std::string capture_source_type_;
  std::string last_capture_anomaly_;
  std::string last_save_failure_;
  std::filesystem::path last_saved_clip_;
  void *pipeline_ = nullptr;
  void *splitmux_sink_ = nullptr;
  void *bus_ = nullptr;
  void *portal_connection_ = nullptr;
  int portal_fd_ = -1;
  std::uint32_t portal_node_id_ = 0;
  std::optional<std::string> portal_target_object_;
  std::string portal_session_handle_;
  std::jthread bus_thread_;
};

} // namespace clipdeck
