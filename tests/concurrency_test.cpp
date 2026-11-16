#include "harness.hpp"
#include "lexicon.hpp"
#include "sqlite3.h"
#include "segmenter.hpp"

#include <atomic>
#include <thread>

void ConcurrentSegmentationKeepsTheTextIntact() {
  Segmenter segmenter("third_party/cppjieba/dict");
  std::atomic<bool> valid{true};
  const std::string text = "明天杉客明去见客户";
  auto cut = [&] {
    for (int i = 0; i < 200; ++i) {
      std::string joined;
      for (const auto& token : segmenter.Cut(text)) joined += token.text;
      if (joined != text) valid = false;
    }
  };
  std::thread first(cut), second(cut);
  first.join(); second.join();
  CHECK(valid.load());
}

void ConcurrentLexiconReadsWritesAndDeletesAreSerialized() {
  CHECK(sqlite3_threadsafe() != 0);
  Lexicon lex(":memory:");
  lex.SaveTerm("张杉","zhangshan",1000);
  std::atomic<bool> valid{true};
  auto write = [&] {
    try {
      for (int i = 0; i < 200; ++i) lex.Commit("张杉", 1000);
    } catch (...) { valid = false; }
  };
  std::thread first(write);
  std::thread second(write);
  std::thread menu([&] {
    try {
      for (int i = 0; i < 200; ++i) {
        lex.Recent(20);
        lex.SaveTerm("临时词","linshici",1000);
        lex.Delete("临时词");
      }
    } catch (...) { valid = false; }
  });
  std::thread retention([&] {
    try {
      for (int i = 0; i < 200; ++i) {
        lex.Commit("张山", 1000);  // An unsaved homophone must not create a term.
      }
    } catch (...) { valid = false; }
  });
  first.join(); second.join(); menu.join(); retention.join();
  CHECK(valid.load());
  const auto recent = lex.Recent(20);
  CHECK(recent.size() == 1);
  if (!recent.empty()) CHECK(recent[0].count == 400);
  CHECK(lex.Search("张山").empty());
}
