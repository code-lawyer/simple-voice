#include "settings.hpp"

#include "utf8.hpp"

#include <cstdio>
#include <string>

namespace {

int NumberAfter(const std::string& json, const char* key, bool* ok) {
  const auto at = json.find(std::string("\"") + key + "\"");
  if (at == std::string::npos) {
    *ok = false;
    return 0;
  }
  auto cursor = json.find(':', at);
  if (cursor == std::string::npos) {
    *ok = false;
    return 0;
  }
  ++cursor;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
  if (cursor >= json.size() || json[cursor] < '0' || json[cursor] > '9') {
    *ok = false;
    return 0;
  }
  int value = 0;
  while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
    value = value * 10 + (json[cursor] - '0');
    ++cursor;
  }
  *ok = true;
  return value;
}

}  // namespace

bool HotkeyAccepted(Hotkey hotkey) {
  if (hotkey.vk <= 0 || hotkey.vk > 255) return false;
  if (hotkey.modifiers < 0 || (hotkey.modifiers & ~(1 | 2 | 4)) != 0) return false;
  switch (hotkey.vk) {
    case 0x10:
    case 0x11:
    case 0x12:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3:
    case 0xA4:
    case 0xA5:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x0D:
    case 0x1B:
      return false;
    default:
      break;
  }
  const bool alt = (hotkey.modifiers & 2) != 0;
  const bool ctrl = (hotkey.modifiers & 1) != 0;
  if (alt && hotkey.vk == 0x09) return false;
  if (alt && hotkey.vk == 0x73) return false;
  if (ctrl && alt && hotkey.vk == 0x2E) return false;
  const bool function_key = hotkey.vk >= 0x70 && hotkey.vk <= 0x87;
  if (hotkey.modifiers == 0 && !function_key) return false;
  return true;
}

Settings LoadSettings(const std::string& path) {
  FILE* file = _wfopen(Utf8ToWide(path).c_str(), L"rb");
  if (file == nullptr) return Settings{DefaultHotkey()};
  std::string json;
  char buffer[4096];
  std::size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) json.append(buffer, got);
  std::fclose(file);
  bool mod_ok = false;
  bool vk_ok = false;
  const int modifiers = NumberAfter(json, "modifiers", &mod_ok);
  const int vk = NumberAfter(json, "vk", &vk_ok);
  if (!mod_ok || !vk_ok || vk <= 0 || vk > 255 || modifiers < 0 || (modifiers & ~(1 | 2 | 4)) != 0) {
    return Settings{DefaultHotkey()};
  }
  return Settings{Hotkey{modifiers, vk}};
}

void SaveSettings(const std::string& path, Settings settings) {
  FILE* file = _wfopen(Utf8ToWide(path).c_str(), L"wb");
  if (file == nullptr) return;
  const std::string json = "{\"modifiers\":" + std::to_string(settings.hotkey.modifiers) +
                            ",\"vk\":" + std::to_string(settings.hotkey.vk) + "}";
  std::fwrite(json.data(), 1, json.size(), file);
  std::fclose(file);
}
