#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class WordSource { Pinyin=1, Voice=2 };
enum ReviewReason { UserEdited=1, DraftDiffers=2, ShortRecognitionRetried=4 };
enum class ReviewFilter { Frequent, Uncertain, All };

struct ReviewWord {
  std::string text, example;
  int count=0, reasons=0, sources=0;
};
struct ReviewChoice {
  std::string candidate, word, code;
  bool speech=false, from_example=false;
};
struct ReviewState {
  bool enabled=false;
  std::int64_t since=0;
};
constexpr std::int64_t kReviewWindow=7*24*60*60;
