#include "register_status.hpp"
#include "product.hpp"
#include "tsf_service.hpp"

#include <msctf.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return "";
  const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(count > 0 ? count : 0, '\0');
  if (count > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), count, nullptr, nullptr);
  }
  return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  return wide;
}

std::wstring GuidText(const GUID& guid) {
  wchar_t text[64];
  const int count = StringFromGUID2(guid, text, 64);
  return count > 0 ? std::wstring(text) : L"";
}

std::wstring DllPath() {
  wchar_t exe[MAX_PATH];
  const DWORD count = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe, count);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L"voiceime_tsf.dll";
  path.resize(slash + 1);
  path += L"voiceime_tsf.dll";
  return path;
}

bool WriteString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LONG result = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool WriteDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LONG result = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool WriteEmpty(HKEY root, const std::wstring& path) {
  HKEY key = nullptr;
  const LONG result = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (key != nullptr) RegCloseKey(key);
  return result == ERROR_SUCCESS;
}

bool WriteTree(HKEY root, const std::wstring& dll) {
  const std::wstring clsid = GuidText(CLSID_VoiceIme);
  const std::wstring profile = GuidText(GUID_VoiceImeProfile);
  const std::wstring category = GuidText(kTipKeyboardCategory);
  const std::wstring display = GuidText(GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER);
  const std::wstring name = kProductDisplayName;
  const std::wstring classes = L"Software\\Classes\\CLSID\\" + clsid;
  const std::wstring tip = L"Software\\Microsoft\\CTF\\TIP\\" + clsid;
  const std::wstring language = tip + L"\\LanguageProfile\\0x00000804\\" + profile;
  bool ok = WriteString(root, classes, nullptr, name);
  ok = WriteString(root, classes + L"\\InProcServer32", nullptr, dll) && ok;
  ok = WriteString(root, classes + L"\\InProcServer32", L"ThreadingModel", L"Apartment") && ok;
  ok = WriteString(root, tip, L"Enable", L"1") && ok;
  ok = WriteString(root, language, nullptr, name) && ok;
  ok = WriteString(root, language, L"Description", name) && ok;
  ok = WriteString(root, language, L"IconFile", dll) && ok;
  ok = WriteDword(root, language, L"IconIndex", 0) && ok;
  ok = WriteDword(root, language, L"Enable", 1) && ok;
  ok = WriteEmpty(root, tip + L"\\Category\\Category\\" + category + L"\\" + clsid) && ok;
  ok = WriteEmpty(root, tip + L"\\Category\\Item\\" + clsid + L"\\" + category) && ok;
  ok = WriteEmpty(root, tip + L"\\Category\\Category\\" + display + L"\\" + clsid) && ok;
  ok = WriteEmpty(root, tip + L"\\Category\\Item\\" + clsid + L"\\" + display) && ok;
  return ok;
}

bool DeleteTree(HKEY root) {
  const std::wstring clsid = GuidText(CLSID_VoiceIme);
  const LSTATUS classes = RegDeleteTreeW(root, (L"Software\\Classes\\CLSID\\" + clsid).c_str());
  // Both category keys live under this TIP key, so one delete removes the display-attribute registration too.
  const LSTATUS tip = RegDeleteTreeW(root, (L"Software\\Microsoft\\CTF\\TIP\\" + clsid).c_str());
  auto gone = [](LSTATUS status) { return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND; };
  return gone(classes) && gone(tip);
}

void InstallLayout(bool uninstall) {
  const std::wstring layout = L"0804:" + GuidText(CLSID_VoiceIme) + GuidText(GUID_VoiceImeProfile);
  const HMODULE input = LoadLibraryW(L"input.dll");
  if (input == nullptr) return;
  using InstallFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
  auto install = reinterpret_cast<InstallFn>(GetProcAddress(input, "InstallLayoutOrTip"));
  if (install != nullptr) install(layout.c_str(), uninstall ? 1u : 0u);
  FreeLibrary(input);
}

}  // namespace

int main(int argc, char** argv) {
  const bool uninstall = argc > 1 && std::strcmp(argv[1], "--unregister") == 0;
  const std::wstring dll = DllPath();
  if (uninstall) {
    InstallLayout(true);
    const bool hkcu = DeleteTree(HKEY_CURRENT_USER);
    const bool hklm = DeleteTree(HKEY_LOCAL_MACHINE);
    std::printf("unregistered\n");
    return UnregisterExitCode(hkcu && hklm);
  }
  const bool hkcu = WriteTree(HKEY_CURRENT_USER, dll);
  if (!hkcu) {
    std::printf("hkcu failed\n");
    return RegisterExitCode(false, false);
  }
  std::printf("hkcu ok\n");
  const bool hklm = WriteTree(HKEY_LOCAL_MACHINE, dll);
  if (hklm) {
    std::printf("hklm ok\n");
    // This call removes a current-user TIP that has no HKLM registration.
    InstallLayout(false);
    WriteTree(HKEY_CURRENT_USER, dll);
  } else {
    std::printf("hklm denied\n");
  }
  std::printf("dll %s\n", WideToUtf8(dll).c_str());
  return RegisterExitCode(hkcu, hklm);
}
