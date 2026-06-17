#pragma once

#include <string_view>

namespace clipdeck {

class CommandHandler {
public:
  int Run(int argc, char *argv[]);

private:
  int RunSettingsCommand(int argc, char *argv[]) const;
  int SetKeybindFromCapture(std::string_view action) const;
  void PrintHelp() const;
};

} // namespace clipdeck
