#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Token {
  std::string text;
  std::string pinyin;
};

struct Entry {
  std::string text;
  std::string pinyin;
  int count = 0;
  std::int64_t last_commit_unix = 0;
  std::string kind;
};

class Lexicon {
 public:
  explicit Lexicon(const std::string& db_path);
  ~Lexicon();
  Lexicon(const Lexicon&) = delete;
  Lexicon& operator=(const Lexicon&) = delete;

  void Commit(const std::string& full_text, const std::vector<Token>& tokens, std::int64_t now_unix);
  std::string Apply(const std::string& recognized, const std::vector<Token>& tokens, std::int64_t now_unix);
  std::vector<Entry> Recent(int limit);
  bool Delete(const std::string& text);

 private:
  struct Db;
  std::unique_ptr<Db> db_;
};
