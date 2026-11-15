#include "settings.hpp"

#include "utf8.hpp"

#include <cstdio>
#include <string>
#include <windows.h>
#include <io.h>
#include <charconv>
#include <string_view>

namespace {

void Space(std::string_view& input) {
  while (!input.empty() && (input.front() == ' ' || input.front() == '\t' ||
         input.front() == '\r' || input.front() == '\n')) input.remove_prefix(1);
}
bool Consume(std::string_view& input, char c) {
  Space(input);
  if (input.empty() || input.front() != c) return false;
  input.remove_prefix(1);
  return true;
}

bool WriteConfig(const std::wstring& path, const std::string& json) {
  const auto temporary = path + L".tmp";
  FILE* file = _wfopen(temporary.c_str(), L"wb");
  if (file == nullptr) return false;
  bool ok = std::fwrite(json.data(), 1, json.size(), file) == json.size();
  if (std::fflush(file) != 0 || _commit(_fileno(file)) != 0) ok = false;
  if (std::fclose(file) != 0) ok = false;
  if (ok) ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  if (!ok) DeleteFileW(temporary.c_str());
  return ok;
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
    case 0x7B:  // F12 is reserved for the debugger, even outside debugging.
    case 0x2C:  // PrintScreen is a system capture shortcut.
      return false;
    default:
      break;
  }
  const bool alt = (hotkey.modifiers & 2) != 0;
  const bool ctrl = (hotkey.modifiers & 1) != 0;
  if (alt && hotkey.vk == 0x09) return false;
  if (alt && hotkey.vk == 0x73) return false;
  if (hotkey.modifiers == 2 && hotkey.vk == 0x20) return false;
  if (ctrl && alt && hotkey.vk == 0x2E) return false;
  const bool function_key = hotkey.vk >= 0x70 && hotkey.vk <= 0x87;
  if (hotkey.modifiers == 0 && !function_key) return false;
  return true;
}

Settings LoadSettings(const std::string& path) {
  return LoadSettings(Utf8ToWide(path));
}

Settings LoadSettings(const std::wstring& path) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return Settings{DefaultHotkey()};
  char buffer[4097];
  DWORD got = 0;
  const BOOL read = ReadFile(file, buffer, sizeof(buffer), &got, nullptr);
  CloseHandle(file);
  if (!read || got == 0 || got > 4096) return Settings{DefaultHotkey()};
  return ParseSettings(std::string(buffer, got));
}

Settings ParseSettings(const std::string& json) {
  const Settings fallback{DefaultHotkey()};
  if (json.size() > 4096) return fallback;
  std::string_view input(json);
  if (input.substr(0, 3) == "\xEF\xBB\xBF") input.remove_prefix(3);
  if (!Consume(input, '{')) return fallback;
  Hotkey hotkey{};
  bool have_mod = false, have_vk = false;
  for (int field = 0; field < 2; ++field) {
    if (field != 0 && !Consume(input, ',')) return fallback;
    if (!Consume(input, '"')) return fallback;
    const auto end = input.find('"');
    if (end == std::string_view::npos) return fallback;
    const auto key = input.substr(0, end);
    input.remove_prefix(end + 1);
    if (!Consume(input, ':')) return fallback;
    Space(input);
    if (input.empty() || input.front() < '0' || input.front() > '9') return fallback;
    if (input.size() > 1 && input.front() == '0' && input[1] >= '0' && input[1] <= '9') return fallback;
    int value = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{}) return fallback;
    input.remove_prefix(static_cast<std::size_t>(parsed.ptr - input.data()));
    if (key == "modifiers" && !have_mod) { have_mod = true; hotkey.modifiers = value; }
    else if (key == "vk" && !have_vk) { have_vk = true; hotkey.vk = value; }
    else return fallback;
  }
  if (!Consume(input, '}')) return fallback;
  Space(input);
  return input.empty() && have_mod && have_vk && HotkeyAccepted(hotkey) ? Settings{hotkey} : fallback;
}

bool SaveSettings(const std::string& path, Settings settings) {
  if (!HotkeyAccepted(settings.hotkey)) return false;
  const std::string json = "{\"modifiers\":" + std::to_string(settings.hotkey.modifiers) +
                            ",\"vk\":" + std::to_string(settings.hotkey.vk) + "}";
  return WriteConfig(Utf8ToWide(path), json);
}

const char* SkinId(UiSkin skin) {
  switch (skin) {
    case UiSkin::Titanium: return "titanium";
    case UiSkin::Daylight: return "daylight";
    case UiSkin::DeepSpace: return "deep-space";
  }
  return nullptr;
}

const wchar_t* SkinLabel(UiSkin skin) {
  switch (skin) {
    case UiSkin::Titanium: return L"钛银";
    case UiSkin::Daylight: return L"极昼";
    case UiSkin::DeepSpace: return L"深空";
  }
  return L"钛银";
}

UiSkin ParseSkin(const std::string& json) {
  if (json.size() > 256) return UiSkin::Titanium;
  std::string_view input(json);
  if (input.substr(0, 3) == "\xEF\xBB\xBF") input.remove_prefix(3);
  if (!Consume(input, '{') || !Consume(input, '"')) return UiSkin::Titanium;
  if (input.substr(0, 5) != "skin\"") return UiSkin::Titanium;
  input.remove_prefix(5);
  if (!Consume(input, ':') || !Consume(input, '"')) return UiSkin::Titanium;
  const auto end = input.find('"');
  if (end == std::string_view::npos) return UiSkin::Titanium;
  const auto value = input.substr(0, end);
  input.remove_prefix(end + 1);
  if (!Consume(input, '}')) return UiSkin::Titanium;
  Space(input);
  if (!input.empty()) return UiSkin::Titanium;
  if (value == "daylight") return UiSkin::Daylight;
  if (value == "deep-space") return UiSkin::DeepSpace;
  return UiSkin::Titanium;
}

UiSkin LoadSkin(const std::wstring& path) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return UiSkin::Titanium;
  char buffer[257]; DWORD got=0;
  const BOOL read=ReadFile(file,buffer,sizeof(buffer),&got,nullptr);
  CloseHandle(file);
  return read && got<=256 ? ParseSkin(std::string(buffer,got)) : UiSkin::Titanium;
}

bool SaveSkin(const std::wstring& path, UiSkin skin) {
  const auto id=SkinId(skin);
  return id && WriteConfig(path,std::string("{\"skin\":\"")+id+"\"}");
}
