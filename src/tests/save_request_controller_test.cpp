#include "../save_request_controller.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>

namespace {

bool Accepted(clipdeck::SaveRequestResult result) {
  return result == clipdeck::SaveRequestResult::Accepted;
}

bool WaitUntil(
    const std::function<bool()> &predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return predicate();
}

} // namespace

TEST(SaveRequestControllerTest, OneRequestRunsOneSave) {
  std::atomic_int save_count = 0;
  clipdeck::SaveRequestController controller(
      [&save_count](const clipdeck::SaveRequest &) { ++save_count; });

  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Manual)));

  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));
  controller.Stop();
  EXPECT_EQ(save_count.load(), 1);
}

TEST(SaveRequestControllerTest, RequestWhileSavingMarksOnePendingSave) {
  std::atomic_int save_count = 0;
  std::mutex mutex;
  std::condition_variable condition;
  bool release_first_save = false;

  clipdeck::SaveRequestController controller(
      [&](const clipdeck::SaveRequest &) {
        const int current_count = ++save_count;
        if (current_count == 1) {
          std::unique_lock lock(mutex);
          condition.notify_all();
          condition.wait(lock,
                         [&release_first_save] { return release_first_save; });
        }
      });

  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Manual)));
  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));

  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Manual)));
  EXPECT_TRUE(controller.Status().pending);

  {
    std::scoped_lock lock(mutex);
    release_first_save = true;
  }
  condition.notify_all();

  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 2; }));
  controller.Stop();
  EXPECT_EQ(save_count.load(), 2);
}

TEST(SaveRequestControllerTest, RapidRequestsFillBoundedFifoInOrder) {
  std::atomic_int save_count = 0;
  std::vector<std::uint64_t> request_ids;
  std::mutex mutex;
  std::condition_variable condition;
  bool release_first_save = false;

  clipdeck::SaveRequestController controller(
      [&](const clipdeck::SaveRequest &request) {
        {
          std::scoped_lock lock(mutex);
          request_ids.push_back(request.id);
        }
        const int current_count = ++save_count;
        if (current_count == 1) {
          std::unique_lock lock(mutex);
          condition.notify_all();
          condition.wait(lock,
                         [&release_first_save] { return release_first_save; });
        }
      });

  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Manual)));
  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));

  for (int request = 0; request < 9; ++request) {
    EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Manual)));
  }
  EXPECT_EQ(controller.RequestSave(clipdeck::SaveSource::Manual),
            clipdeck::SaveRequestResult::QueueFull);

  {
    std::scoped_lock lock(mutex);
    release_first_save = true;
  }
  condition.notify_all();

  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 10; }));
  controller.Stop();
  EXPECT_EQ(save_count.load(), 10);
  ASSERT_EQ(request_ids.size(), 10);
  EXPECT_TRUE(std::is_sorted(request_ids.begin(), request_ids.end()));
}

TEST(SaveRequestControllerTest, KeybindDebounceSuppressesDuplicateRequests) {
  std::atomic_int save_count = 0;
  clipdeck::SaveRequestController controller(
      [&save_count](const clipdeck::SaveRequest &) { ++save_count; },
      clipdeck::SaveRequestControllerConfig{.keybind_debounce =
                                                std::chrono::hours(1)});

  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Keybind)));
  EXPECT_EQ(controller.RequestSave(clipdeck::SaveSource::Keybind),
            clipdeck::SaveRequestResult::Debounced);

  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));
  controller.Stop();
  EXPECT_EQ(save_count.load(), 1);
}

TEST(SaveRequestControllerTest, ManualRequestWhileSavingIsSerialized) {
  std::atomic_int save_count = 0;
  std::atomic_int concurrent_saves = 0;
  std::atomic_int max_concurrent_saves = 0;
  std::mutex mutex;
  std::condition_variable condition;
  bool release_first_save = false;

  clipdeck::SaveRequestController controller(
      [&](const clipdeck::SaveRequest &) {
        const int active_saves = ++concurrent_saves;
        max_concurrent_saves.store(
            std::max(max_concurrent_saves.load(), active_saves));
        const int current_count = ++save_count;
        if (current_count == 1) {
          std::unique_lock lock(mutex);
          condition.notify_all();
          condition.wait(lock,
                         [&release_first_save] { return release_first_save; });
        }
        --concurrent_saves;
      });

  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Keybind)));
  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));
  EXPECT_TRUE(Accepted(controller.RequestSave(clipdeck::SaveSource::Manual)));

  {
    std::scoped_lock lock(mutex);
    release_first_save = true;
  }
  condition.notify_all();

  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 2; }));
  controller.Stop();
  EXPECT_EQ(save_count.load(), 2);
  EXPECT_EQ(max_concurrent_saves.load(), 1);
}
