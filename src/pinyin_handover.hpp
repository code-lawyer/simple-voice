#pragma once

#include <windows.h>

inline constexpr wchar_t kPinyinHandoverKey[] = L"Software\\SimpleVoicePinyinHandover";

// TSF registration is machine-wide. Keep the barrier across sign-outs so
// another signed-in account cannot open its dictionary in two versions.
inline LSTATUS MarkPinyinHandover(HKEY hive, const wchar_t* path) {
  HKEY key = nullptr;
  const auto status = RegCreateKeyExW(hive, path, 0, nullptr, REG_OPTION_VOLATILE,
                                     KEY_QUERY_VALUE, nullptr, &key, nullptr);
  if (key) RegCloseKey(key);
  return status;
}

inline bool PinyinHandoverPending(HKEY hive, const wchar_t* path) {
  HKEY key = nullptr;
  const auto status = RegOpenKeyExW(hive, path, 0, KEY_QUERY_VALUE, &key);
  if (key) RegCloseKey(key);
  return status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND;
}
