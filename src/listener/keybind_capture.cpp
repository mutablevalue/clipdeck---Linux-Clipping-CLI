#include "keybind_capture.hpp"

#include "keybind.hpp"
#include "terminal.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <ranges>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kInputFileDescriptor = STDIN_FILENO;
constexpr std::string_view kCaptureContext = "settings";

} // namespace

namespace clipdeck {

std::optional<std::string>
CaptureKeybindFromTerminal(std::chrono::seconds timeout) {
  TerminalRawMode terminal_mode(kInputFileDescriptor);
  std::vector<std::string> tokens;
  const auto expires_at = std::chrono::steady_clock::now() + timeout;

  if (!terminal_mode.IsEnabled()) {
    Log(LogLevel::Warning, kCaptureContext,
        "Terminal raw mode is unavailable; setup keybind capture may be limited.");
  }

  while (std::chrono::steady_clock::now() < expires_at) {
    if (!InputAvailable(kInputFileDescriptor, std::chrono::milliseconds(100))) {
      continue;
    }

    char character = '\0';
    const ssize_t bytes_read = read(kInputFileDescriptor, &character, 1);

    if (bytes_read <= 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    if (character == '\n' || character == '\r') {
      break;
    }

    auto token = RawCharacterToKeyToken(character);
    if (!token.has_value()) {
      continue;
    }

    for (const auto part : std::views::split(token.value(), '+')) {
      std::string text;
      for (const char part_character : part) {
        text.push_back(part_character);
      }

      if (std::ranges::find(tokens, text) == tokens.end()) {
        tokens.push_back(text);
      }
    }
  }

  if (tokens.empty()) {
    return std::nullopt;
  }

  std::string keybind;
  for (const auto &token : tokens) {
    if (!keybind.empty()) {
      keybind += '+';
    }

    keybind += token;
  }

  return NormalizeKeybind(keybind);
}

} // namespace clipdeck
