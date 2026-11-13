#include "harness.hpp"

void Utf8CountsCharactersNotBytes();

int main(int argc, char** argv) {
  const Test tests[] = {
      {"Utf8CountsCharactersNotBytes", Utf8CountsCharactersNotBytes},
  };
  return RunTests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
