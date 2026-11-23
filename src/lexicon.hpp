#pragma once
#include "word_review.hpp"
#include <atomic>
#include <chrono>
#include <ctime>
#include <stdexcept>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>

struct Entry {
  std::string text;
  std::string pinyin;
  int count = 0;
  std::int64_t last_commit_unix = 0;
  std::string kind;
};
class LexiconBusy : public std::runtime_error {
 public: using std::runtime_error::runtime_error;
};

class Lexicon {
 public:
  explicit Lexicon(const std::string& db_path);
  ~Lexicon();
  Lexicon(const Lexicon&) = delete;
  Lexicon& operator=(const Lexicon&) = delete;

  // Returns true when expiry removed a term from the pinyin candidate table.
  bool Commit(const std::string& full_text, std::int64_t now_unix);
  std::vector<Entry> Recent(int limit);
  bool Delete(const std::string& text, Entry* removed = nullptr);
  // Undo never overwrites a word recreated after deletion.
  bool RestoreDeletedTerm(const Entry& removed);
  // Only an explicit user action creates a personal term. Commit only touches
  // existing terms; legacy automatic records never rewrite recognition.
  void SaveTerm(const std::string& text, const std::string& input_code,
                std::int64_t now_unix, const std::string& previous_text = "");
  std::vector<Entry> Search(const std::string& query, int limit = 200, int offset = 0);
  int Count();
  int LegacyCount();
  void ClearLegacy();
  bool ExportTerms(const std::wstring& file);
  ReviewState ReviewSettings(std::int64_t now=std::time(nullptr));
  void SetReviewEnabled(bool enabled);
  void ClearReview();
  void ExpireReview(std::int64_t now);
  // Zero while disabled. A change to collection or an explicit clear invalidates queued work.
  std::uint64_t ReviewEpoch() const { return review_epoch_.load(); }
  void RecordReview(const std::vector<ReviewWord>& words, WordSource source, std::int64_t at,
                    std::uint64_t epoch);
  void RecordReview(const std::vector<ReviewWord>& words, WordSource source,
                    std::chrono::system_clock::time_point receipt, std::uint64_t epoch);
  std::vector<ReviewWord> ReviewWords(ReviewFilter filter, const std::string& query,
                                     int limit, int offset, std::int64_t now);
  void AcceptReview(const std::vector<ReviewChoice>& choices, std::int64_t now);
  void SkipReview(const std::vector<std::string>& candidates);
  bool ReviewCandidateExists(const std::string& candidate, std::int64_t now);
  std::vector<std::string> PendingSpeechNames();
  void CompleteSpeechNames(const std::vector<std::string>& names);
  // Validate before opening the transaction. A failed accompanying settings
  // update rolls the dictionary back with the same transaction.
  void ReplaceTerms(const std::vector<Entry>& terms, const std::function<void()>& settings_update = {});

 private:
  struct Db;
  std::unique_ptr<Db> db_;
  std::atomic<std::uint64_t> review_epoch_{0};
};
