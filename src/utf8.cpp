#include "utf8.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) return "";
  const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(count > 0 ? count : 0, '\0');
  if (count > 0) {
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), count, nullptr, nullptr);
  }
  return utf8;
}

bool FileExistsUtf8(std::string_view path) {
  const std::wstring wide = Utf8ToWide(path);
  if (wide.empty()) return false;
  return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

int CodePointCount(std::string_view utf8) {
  int count = 0;
  for (std::size_t i = 0; i < utf8.size();) {
    const auto lead = static_cast<unsigned char>(utf8[i]);
    std::size_t need = 1;
    if ((lead & 0x80) == 0) {
      need = 1;
    } else if ((lead & 0xE0) == 0xC0) {
      need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      need = 4;
    } else {
      ++count;
      ++i;
      continue;
    }
    if (i + need > utf8.size()) {
      count += static_cast<int>(utf8.size() - i);
      break;
    }
    bool ok = true;
    for (std::size_t j = 1; j < need; ++j) {
      const auto cont = static_cast<unsigned char>(utf8[i + j]);
      if ((cont & 0xC0) != 0x80) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      ++count;
      ++i;
      continue;
    }
    ++count;
    i += need;
  }
  return count;
}
