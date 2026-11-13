#include "harness.hpp"
#include "segmenter.hpp"
#include "utf8.hpp"

#include <string>

namespace {

std::string JiebaDictDir() { return "third_party/cppjieba/dict"; }

}  // namespace

void CutsANameIntoTokensWithPinyin() {
  Segmenter seg(JiebaDictDir());
  const std::string text = "明天下午见客户";
  auto tokens = seg.Cut(text);
  CHECK(!tokens.empty());
  CHECK(CodePointCount(tokens[0].text) >= 2);
  CHECK(tokens[0].pinyin.find(' ') != std::string::npos || tokens[0].pinyin.find('g') != std::string::npos);
  std::string joined;
  bool any_pinyin = false;
  for (const auto& token : tokens) {
    joined += token.text;
    if (!token.pinyin.empty()) any_pinyin = true;
  }
  CHECK(joined == text);
  CHECK(any_pinyin);
}

void RememberKeepsAUserWord() {
  Segmenter seg(JiebaDictDir());
  const std::string word = "杉客明";
  bool whole = false;
  for (const auto& token : seg.Cut(word)) {
    if (token.text == word) whole = true;
  }
  CHECK(!whole);
  seg.Remember(word);
  seg.Remember("明");
  seg.Remember("杉客明杉客明杉客明");
  whole = false;
  for (const auto& token : seg.Cut(word)) {
    if (token.text == word) whole = true;
  }
  CHECK(whole);
}
