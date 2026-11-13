#pragma once

#include "hotkey.hpp"

#include <string>

struct Settings {
  Hotkey hotkey;
};

Settings LoadSettings(const std::string& path);
void SaveSettings(const std::string& path, Settings settings);
bool HotkeyAccepted(Hotkey hotkey);
