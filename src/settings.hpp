#pragma once

#include "hotkey.hpp"

#include <string>

struct Settings {
  Hotkey hotkey;
};

enum class UiSkin { Titanium, Daylight, DeepSpace };
const char* SkinId(UiSkin skin);
const wchar_t* SkinLabel(UiSkin skin);
UiSkin ParseSkin(const std::string& json);
UiSkin LoadSkin(const std::wstring& path);
bool SaveSkin(const std::wstring& path, UiSkin skin);

Settings LoadSettings(const std::string& path);
Settings LoadSettings(const std::wstring& path);
Settings ParseSettings(const std::string& json);
bool SaveSettings(const std::string& path, Settings settings);
bool HotkeyAccepted(Hotkey hotkey);
