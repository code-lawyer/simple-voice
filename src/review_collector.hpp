#pragma once
#include "lexicon.hpp"
#include "segmenter.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

std::vector<ReviewWord> ExtractReviewWords(const Segmenter& segmenter,WordSource source,
    const std::string& written,const std::string& recognized={},const std::string& draft={},bool short_retry=false);

class ReviewCollector {
 public:
  ReviewCollector(Lexicon& lexicon,std::string dictionary);
  ~ReviewCollector();
  void Submit(WordSource source,const std::string& written,const std::string& recognized={},
              const std::string& draft={},bool short_retry=false);
  bool Failed() const { return failed_.load(); }
 private:
  struct Input {
    struct Text { std::string written,recognized,draft; };
    WordSource source;
    Text text;
    std::chrono::system_clock::time_point receipt;
    std::uint64_t epoch;
    bool short_retry;
  };
  Lexicon& lexicon_;
  std::string dictionary_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Input> pending_;
  std::size_t pending_bytes_=0;
  bool stopping_=false;
  std::chrono::steady_clock::time_point shutdown_deadline_;
  std::atomic<bool> failed_{false};
  std::thread worker_;
  void Run();
  bool StoreWhenAvailable(const Input& input,const std::vector<ReviewWord>& words);
};
