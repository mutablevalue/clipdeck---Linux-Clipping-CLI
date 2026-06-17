#include "../save_request_controller.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

namespace {

bool WaitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout =
                   std::chrono::milliseconds(1000)) {
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
      [&save_count](clipdeck::SaveSource) { ++save_count; });

  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Manual));

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
      [&](clipdeck::SaveSource) {
        const int current_count = ++save_count;
        if (current_count == 1) {
          std::unique_lock lock(mutex);
          condition.notify_all();
          condition.wait(lock, [&release_first_save] {
            return release_first_save;
          });
        }
      });

  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Manual));
  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));

  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Manual));
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

TEST(SaveRequestControllerTest, RapidRequestsDoNotCreateUnlimitedQueue) {
  std::atomic_int save_count = 0;
  std::mutex mutex;
  std::condition_variable condition;
  bool release_first_save = false;

  clipdeck::SaveRequestController controller(
      [&](clipdeck::SaveSource) {
        const int current_count = ++save_count;
        if (current_count == 1) {
          std::unique_lock lock(mutex);
          condition.notify_all();
          condition.wait(lock, [&release_first_save] {
            return release_first_save;
          });
        }
      });

  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Manual));
  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));

  for (int request = 0; request < 10; ++request) {
    controller.RequestSave(clipdeck::SaveSource::Manual);
  }

  {
    std::scoped_lock lock(mutex);
    release_first_save = true;
  }
  condition.notify_all();

  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 2; }));
  controller.Stop();
  EXPECT_EQ(save_count.load(), 2);
}

TEST(SaveRequestControllerTest, KeybindDebounceSuppressesDuplicateRequests) {
  std::atomic_int save_count = 0;
  clipdeck::SaveRequestController controller(
      [&save_count](clipdeck::SaveSource) { ++save_count; },
      clipdeck::SaveRequestControllerConfig{.keybind_debounce =
                                                std::chrono::hours(1)});

  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Keybind));
  EXPECT_FALSE(controller.RequestSave(clipdeck::SaveSource::Keybind));

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
      [&](clipdeck::SaveSource) {
        const int active_saves = ++concurrent_saves;
        max_concurrent_saves.store(
            std::max(max_concurrent_saves.load(), active_saves));
        const int current_count = ++save_count;
        if (current_count == 1) {
          std::unique_lock lock(mutex);
          condition.notify_all();
          condition.wait(lock, [&release_first_save] {
            return release_first_save;
          });
        }
        --concurrent_saves;
      });

  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Keybind));
  EXPECT_TRUE(WaitUntil([&save_count] { return save_count.load() == 1; }));
  EXPECT_TRUE(controller.RequestSave(clipdeck::SaveSource::Manual));

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
