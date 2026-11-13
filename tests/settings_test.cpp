#include "harness.hpp"
#include "settings.hpp"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

std::string TempPath() {
  static int n = 0;
  std::string path = "build/settings-" + std::to_string(++n) + ".json";
  std::remove(path.c_str());
  return path;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

void MissingSettingsUseCtrlAltSpace() {
  const std::string path = TempPath();
  const Settings s = LoadSettings(path);
  CHECK(s.hotkey.modifiers == (1 | 2));
  CHECK(s.hotkey.vk == 0x20);
  FILE* file = std::fopen(path.c_str(), "rb");
  CHECK(file == nullptr);
  if (file != nullptr) std::fclose(file);
}

void SaveSettingsWritesTheDefaultJson() {
  const std::string path = TempPath();
  SaveSettings(path, Settings{DefaultHotkey()});
  CHECK(ReadFile(path) == "{\"modifiers\":3,\"vk\":32}");
  const Settings s = LoadSettings(path);
  CHECK(s.hotkey.modifiers == 3);
  CHECK(s.hotkey.vk == 32);
}

void ReservedOrEmptyHotkeyIsRejected() {
  CHECK(!HotkeyAccepted(Hotkey{0, 0}));
  CHECK(!HotkeyAccepted(Hotkey{1 | 2, 0}));
  CHECK(!HotkeyAccepted(Hotkey{0, 0x5B}));
  CHECK(!HotkeyAccepted(Hotkey{0, 0x0D}));
  CHECK(!HotkeyAccepted(Hotkey{0, 0x1B}));
  CHECK(!HotkeyAccepted(Hotkey{2, 0x09}));
  CHECK(!HotkeyAccepted(Hotkey{2, 0x73}));
  CHECK(!HotkeyAccepted(Hotkey{1 | 2, 0x2E}));
  CHECK(HotkeyAccepted(DefaultHotkey()));
  CHECK(HotkeyAccepted(Hotkey{0, 0x77}));
}
