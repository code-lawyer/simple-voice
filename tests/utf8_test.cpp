#include "harness.hpp"
#include "utf8.hpp"

void Utf8CountsCharactersNotBytes() {
  CHECK(CodePointCount("张杉") == 2);
  CHECK(CodePointCount("a") == 1);
  CHECK(CodePointCount("") == 0);
}
