#include "harness.hpp"
#include "lexicon.hpp"
#include "sqlite3.h"
#include "segmenter.hpp"

#include <atomic>
#include <thread>

void ConcurrentSegmentationAndLearningKeepTheTextIntact() {
  Segmenter segmenter("third_party/cppjieba/dict");
  std::atomic<bool> valid{true};
  const std::string text = "明天杉客明去见客户";
  std::thread learn([&] {
    for (int i = 0; i < 200; ++i) segmenter.Remember("杉客明");
  });
  auto cut = [&] {
    for (int i = 0; i < 200; ++i) {
      std::string joined;
      for (const auto& token : segmenter.Cut(text)) joined += token.text;
      if (joined != text) valid = false;
    }
  };
  std::thread first(cut), second(cut);
  learn.join(); first.join(); second.join();
  CHECK(valid.load());
  bool remembered = false;
  for (const auto& token : segmenter.Cut(text)) if (token.text == "杉客明") remembered = true;
  CHECK(remembered);
}

void ConcurrentLexiconReadsWritesAndDeletesAreSerialized() {
  CHECK(sqlite3_threadsafe() != 0);
  Lexicon lex(":memory:");
  lex.SaveTerm("张杉","zhangshan",1000);
  std::atomic<bool> valid{true};
  auto write = [&] {
    try {
      for (int i = 0; i < 200; ++i) lex.Commit("张杉", {{"张杉", "zhang shan"}}, 1000);
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
  std::thread recognition([&] {
    try {
      for (int i = 0; i < 200; ++i) {
        const auto result = lex.Apply("张山", {{"张山", "zhang shan"}}, 1000);
        if (result != "张山") valid = false;
      }
    } catch (...) { valid = false; }
  });
  first.join(); second.join(); menu.join(); recognition.join();
  CHECK(valid.load());
  const auto recent = lex.Recent(20);
  CHECK(recent.size() == 1);
  if (!recent.empty()) CHECK(recent[0].count == 400);
  CHECK(lex.Apply("张山", {{"张山", "zhang shan"}}, 1000) == "张山");
}
