#pragma once

#include <windows.h>
#include <algorithm>
#include <string>

inline std::wstring ModuleFilePath(HMODULE module=nullptr) {
  std::wstring path(MAX_PATH,L'\0');
  for(;;) {
    const DWORD length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!length) return {};
    if(length<path.size()) { path.resize(length); return path; }
    if(path.size()>=32768) return {};
    path.resize(std::min<size_t>(path.size()*2,32768));
  }
}

inline std::wstring ExecutableDirectory(HMODULE module=nullptr) {
  const auto path=ModuleFilePath(module);
  const auto separator=path.find_last_of(L"\\/");
  return separator==std::wstring::npos?std::wstring{}:path.substr(0,separator);
}

inline std::wstring VoiceUserDataDirectory() {
  DWORD capacity = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  while (capacity) {
    std::wstring directory(capacity, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory.data(), capacity);
    if (!length) break;
    if (length < capacity) {
      directory.resize(length);
      return directory + L"\\VoiceIme";
    }
    capacity = length;
  }
  return {};
}
