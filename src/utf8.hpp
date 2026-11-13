#pragma once

#include <string>
#include <string_view>

int CodePointCount(std::string_view utf8);
std::wstring Utf8ToWide(std::string_view utf8);
std::string WideToUtf8(std::wstring_view wide);
bool FileExistsUtf8(std::string_view path);
