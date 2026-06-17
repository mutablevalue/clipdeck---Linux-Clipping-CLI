#include "../listener/daemon_listener.hpp"

#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <linux/input.h>
#include <memory>

namespace {

std::unique_ptr<clipdeck::DaemonListener>
MakeTestListener(std::atomic_int &save_count,
                 std::atomic_int &stop_count,
                 std::chrono::milliseconds debounce =
                     std::chrono::milliseconds(0)) {
  auto listener = std::make_unique<clipdeck::DaemonListener>(
      clipdeck::ListenerConfig{.save_keybind = "Ctrl+Alt+S",
                               .stop_keybind = "Ctrl+Alt+X",
                               .input_directory = "/dev/input",
                               .keybind_debounce = debounce});
  listener->SetKeybindCallback([&save_count,
                                &stop_count](std::string_view action) {
    if (action == "save") {
      ++save_count;
    } else if (action == "stop") {
      ++stop_count;
    }
  });
  return listener;
}

void PressSaveCombo(clipdeck::DaemonListener &listener) {
  listener.InjectInputEventForTest(EV_KEY, KEY_LEFTCTRL, 1);
  listener.InjectInputEventForTest(EV_KEY, KEY_LEFTALT, 1);
  listener.InjectInputEventForTest(EV_KEY, KEY_S, 1);
}

void PressStopCombo(clipdeck::DaemonListener &listener) {
  listener.InjectInputEventForTest(EV_KEY, KEY_LEFTCTRL, 1);
  listener.InjectInputEventForTest(EV_KEY, KEY_LEFTALT, 1);
  listener.InjectInputEventForTest(EV_KEY, KEY_X, 1);
}

} // namespace

TEST(DaemonListenerTest, KeybindTriggersOnceOnPress) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener = MakeTestListener(save_count, stop_count);

  PressSaveCombo(*listener);

  EXPECT_EQ(save_count.load(), 1);
}

TEST(DaemonListenerTest, HoldingKeybindDoesNotRetrigger) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener = MakeTestListener(save_count, stop_count);

  PressSaveCombo(*listener);
  listener->InjectInputEventForTest(EV_KEY, KEY_LEFTCTRL, 1);
  listener->InjectInputEventForTest(EV_KEY, KEY_LEFTALT, 1);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 1);

  EXPECT_EQ(save_count.load(), 1);
}

TEST(DaemonListenerTest, KeyRepeatDoesNotTrigger) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener = MakeTestListener(save_count, stop_count);

  listener->InjectInputEventForTest(EV_KEY, KEY_LEFTCTRL, 1);
  listener->InjectInputEventForTest(EV_KEY, KEY_LEFTALT, 1);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 2);

  EXPECT_EQ(save_count.load(), 0);
}

TEST(DaemonListenerTest, ReleasingUnrelatedKeyDoesNotRearm) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener = MakeTestListener(save_count, stop_count);

  PressSaveCombo(*listener);
  listener->InjectInputEventForTest(EV_KEY, KEY_A, 0);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 1);

  EXPECT_EQ(save_count.load(), 1);
}

TEST(DaemonListenerTest, ReleasingRequiredKeyRearmsAfterComboIsUp) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener = MakeTestListener(save_count, stop_count);

  PressSaveCombo(*listener);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 0);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 1);

  EXPECT_EQ(save_count.load(), 2);
}

TEST(DaemonListenerTest, DuplicatePressInsideDebounceDoesNotTrigger) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener =
      MakeTestListener(save_count, stop_count, std::chrono::hours(1));

  PressSaveCombo(*listener);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 0);
  listener->InjectInputEventForTest(EV_KEY, KEY_S, 1);

  EXPECT_EQ(save_count.load(), 1);
}

TEST(DaemonListenerTest, StopKeybindTriggersSeparateAction) {
  std::atomic_int save_count = 0;
  std::atomic_int stop_count = 0;
  auto listener = MakeTestListener(save_count, stop_count);

  PressStopCombo(*listener);

  EXPECT_EQ(save_count.load(), 0);
  EXPECT_EQ(stop_count.load(), 1);
}
