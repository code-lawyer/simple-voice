#pragma once

#include <cstdio>
#include <filesystem>
#include <string>

struct Test {
  const char* name;
  void (*fn)();
};

inline int g_failed = 0;

const std::filesystem::path& TestScratchDirectory();

inline void Check(bool ok, const char* expr, const char* file, int line) {
  if (ok) return;
  ++g_failed;
  std::fprintf(stderr, "FAIL %s:%d %s\n", file, line, expr);
}

#define CHECK(e) Check(static_cast<bool>(e), #e, __FILE__, __LINE__)

inline std::string Lower(std::string text) {
  for (char& ch : text) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return text;
}

inline int RunTests(int argc, char** argv, const Test* tests, int count) {
  const std::string filter = argc > 1 ? Lower(argv[1]) : "";
  int ran = 0;
  for (int i = 0; i < count; ++i) {
    if (!filter.empty() && Lower(tests[i].name).find(filter) == std::string::npos) {
      continue;
    }
    ++ran;
    tests[i].fn();
  }
  if (ran == 0) {
    std::fprintf(stderr, "no tests matched\n");
    return 1;
  }
  if (g_failed != 0) {
    std::fprintf(stderr, "%d failed\n", g_failed);
    return 1;
  }
  std::printf("%d passed\n", ran);
  return 0;
}
