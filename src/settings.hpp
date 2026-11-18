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
unsigned HotkeyChangedMessage();

class HotkeyCache {
 public:
  void Load(std::wstring path);
  void Reload();
  Hotkey value() const { return hotkey_; }
 private:
  std::wstring path_;
  Hotkey hotkey_ = DefaultHotkey();
};

struct InputPreferences {
  bool learning = true;
  int pause_ms = 800; // Legacy backup/settings field; manual recording ignores it.
  std::wstring microphone; // Empty follows the system default.
  bool guide_dismissed = false;
  bool chinese_only = false;
};
InputPreferences LoadInputPreferences(const std::wstring& path);
bool SaveInputPreferences(const std::wstring& path, const InputPreferences& preferences);
