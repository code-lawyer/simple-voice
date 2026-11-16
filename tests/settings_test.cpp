#include "harness.hpp"
#include "hotkey.hpp"
#include "register_status.hpp"
#include "settings.hpp"
#include "utf8.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <windows.h>
#include <filesystem>

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

void FailedSettingsReplacePreservesPreviousFile() {
  const auto path = TempPath();
  CHECK(SaveSettings(path, Settings{DefaultHotkey()}));
  HANDLE held = CreateFileW(Utf8ToWide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  CHECK(held != INVALID_HANDLE_VALUE);
  const bool saved = SaveSettings(path, Settings{Hotkey{0, VK_F8}});
  if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
  CHECK(!saved);
  CHECK(LoadSettings(path).hotkey.vk == DefaultHotkey().vk);
  CHECK(!std::filesystem::exists(path + ".tmp"));
  std::remove(path.c_str());
}

void SettingsSaveReportsMissingDirectory() {
  const auto path = TempPath() + "/unavailable/config.json";
  CHECK(!SaveSettings(path, Settings{DefaultHotkey()}));
  CHECK(!std::filesystem::exists(path));
}

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
  CHECK(!HotkeyAccepted(Hotkey{0, 0x20}));
  CHECK(!HotkeyAccepted(Hotkey{0, 0x41}));
  CHECK(!HotkeyAccepted(Hotkey{0, 0x08}));
  CHECK(!HotkeyAccepted(Hotkey{0, 0x09}));
  CHECK(HotkeyAccepted(DefaultHotkey()));
  CHECK(HotkeyAccepted(Hotkey{0, 0x77}));
}

void DeadEngineIsStartedAgain() {
  CHECK(ShouldLaunchEngine(false));
  CHECK(!ShouldLaunchEngine(true));
}

void RepeatedHotkeyIsNotANewPress() {
  CHECK(IsKeyRepeat(0x40000000u));
  CHECK(!IsKeyRepeat(0));
}

void RegisterRequiresTheMachineKey() {
  CHECK(RegisterExitCode(true, true) == 0);
  CHECK(RegisterExitCode(true, false) == 1);
  CHECK(RegisterExitCode(false, false) == 1);
  CHECK(UnregisterExitCode(true) == 0);
  CHECK(UnregisterExitCode(false) == 1);
}

void SettingsRoundTripOnANonAsciiPath() {
  const std::string path = "build/设置-hotkey.json";
  std::remove(path.c_str());
  SaveSettings(path, Settings{Hotkey{1, 0x70}});
  const Settings s = LoadSettings(path);
  CHECK(s.hotkey.modifiers == 1);
  CHECK(s.hotkey.vk == 0x70);
  CHECK(FileExistsUtf8(path));
  std::remove(path.c_str());
}

void SkinsPersistWithoutChangingLegacyHotkeys() {
  const auto hotkey_path=TempPath();
  const auto skin_path=Utf8ToWide(TempPath());
  CHECK(SaveSettings(hotkey_path,Settings{Hotkey{1,VK_F8}}));
  const auto original=ReadFile(hotkey_path);
  CHECK(LoadSkin(skin_path)==UiSkin::Titanium);
  for (auto skin : {UiSkin::Titanium,UiSkin::Daylight,UiSkin::DeepSpace}) {
    CHECK(SaveSkin(skin_path,skin));
    CHECK(LoadSkin(skin_path)==skin);
    CHECK(ReadFile(hotkey_path)==original);
  }
  CHECK(SaveSettings(hotkey_path,Settings{DefaultHotkey()}));
  CHECK(LoadSkin(skin_path)==UiSkin::DeepSpace);
  std::remove(hotkey_path.c_str()); DeleteFileW(skin_path.c_str());
}

void InvalidAppearanceFallsBackAndFailedSavePreservesSkin() {
  for (const auto* bad : {"", "{", "{\"skin\":\"unknown\"}", "{\"skin\":2}",
       "{\"skin\":\"daylight\"}junk", "{\"skin\":\"daylight\",\"skin\":\"deep-space\"}"})
    CHECK(ParseSkin(bad)==UiSkin::Titanium);
  CHECK(ParseSkin("\xEF\xBB\xBF{ \"skin\" : \"deep-space\" }\n")==UiSkin::DeepSpace);
  const auto path=Utf8ToWide(TempPath());
  CHECK(SaveSkin(path,UiSkin::Daylight));
  HANDLE held=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  CHECK(held!=INVALID_HANDLE_VALUE);
  CHECK(!SaveSkin(path,UiSkin::DeepSpace));
  CloseHandle(held);
  CHECK(LoadSkin(path)==UiSkin::Daylight);
  CHECK(!SaveSkin(path,static_cast<UiSkin>(99)));
  CHECK(!std::filesystem::exists(path+L".tmp"));
  DeleteFileW(path.c_str());
}

void HotkeyCacheKeepsTypingIndependentOfFileReads() {
  const auto path=TempPath();
  CHECK(SaveSettings(path,Settings{Hotkey{0,VK_F8}}));
  HotkeyCache cache; cache.Load(Utf8ToWide(path));
  CHECK(cache.value().vk==VK_F8);
  CHECK(SaveSettings(path,Settings{Hotkey{0,VK_F9}}));
  CHECK(cache.value().vk==VK_F8); // Changes take effect at explicit refresh, not mid-key.
  cache.Reload(); CHECK(cache.value().vk==VK_F9);
  HANDLE held=CreateFileW(Utf8ToWide(path).c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr);
  CHECK(held!=INVALID_HANDLE_VALUE);
  for(int i=0;i<1000;++i)CHECK(cache.value().vk==VK_F9);
  CHECK(!SaveSettings(path,Settings{Hotkey{0,VK_F10}}));
  CloseHandle(held);
  cache.Reload(); CHECK(cache.value().vk==VK_F9);
  std::remove(path.c_str());
  CHECK(cache.value().vk==VK_F9);
  cache.Reload(); CHECK(cache.value().vk==DefaultHotkey().vk);
}
