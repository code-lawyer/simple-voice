#include "lexicon.hpp"

#include "sqlite3.h"
#include "utf8.hpp"

#include <stdexcept>
#include <utility>

namespace {

constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kBiasSeconds = 180 * kDay;
constexpr std::int64_t kDropSeconds = 365 * kDay;

void Exec(sqlite3* db, const char* sql) {
  char* error = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : "sqlite error";
    sqlite3_free(error);
    throw std::runtime_error(message);
  }
}

std::string JoinPinyin(const std::vector<Token>& tokens) {
  std::string joined;
  for (const Token& token : tokens) {
    if (!joined.empty()) joined.push_back(' ');
    joined += token.pinyin;
  }
  return joined;
}

bool Better(int count, std::int64_t when, int best_count, std::int64_t best_when) {
  if (count != best_count) return count > best_count;
  return when > best_when;
}

}  // namespace

struct Lexicon::Db {
  sqlite3* raw = nullptr;
};

Lexicon::Lexicon(const std::string& db_path) : db_(std::make_unique<Db>()) {
  if (sqlite3_open(db_path.c_str(), &db_->raw) != SQLITE_OK) {
    throw std::runtime_error("failed to open lexicon");
  }
  Exec(db_->raw,
       "CREATE TABLE IF NOT EXISTS entries ("
       "text TEXT PRIMARY KEY,"
       "pinyin TEXT NOT NULL,"
       "count INTEGER NOT NULL,"
       "last_commit_unix INTEGER NOT NULL,"
       "kind TEXT NOT NULL);"
       "CREATE TABLE IF NOT EXISTS seen_phrase (text TEXT PRIMARY KEY);");
}

Lexicon::~Lexicon() {
  if (db_ && db_->raw) sqlite3_close(db_->raw);
}

void Lexicon::Commit(const std::string& full_text, const std::vector<Token>& tokens, std::int64_t now_unix) {
  Exec(db_->raw, "BEGIN");
  sqlite3_stmt* purge = nullptr;
  sqlite3_prepare_v2(db_->raw,
                      "DELETE FROM entries WHERE (?1 - last_commit_unix) > ?2",
                      -1, &purge, nullptr);
  sqlite3_bind_int64(purge, 1, now_unix);
  sqlite3_bind_int64(purge, 2, kDropSeconds);
  sqlite3_step(purge);
  sqlite3_finalize(purge);

  sqlite3_stmt* word = nullptr;
  sqlite3_prepare_v2(db_->raw,
                      "INSERT INTO entries(text, pinyin, count, last_commit_unix, kind) "
                      "VALUES(?1, ?2, 1, ?3, 'word') "
                      "ON CONFLICT(text) DO UPDATE SET "
                      "count = count + 1, last_commit_unix = excluded.last_commit_unix, "
                      "pinyin = excluded.pinyin",
                      -1, &word, nullptr);
  for (const Token& token : tokens) {
    if (CodePointCount(token.text) < 2 || CodePointCount(token.text) > 8) continue;
    sqlite3_reset(word);
    sqlite3_clear_bindings(word);
    sqlite3_bind_text(word, 1, token.text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(word, 2, token.pinyin.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(word, 3, now_unix);
    sqlite3_step(word);
  }
  sqlite3_finalize(word);

  if (CodePointCount(full_text) > 8) {
    sqlite3_stmt* find = nullptr;
    sqlite3_prepare_v2(db_->raw, "SELECT 1 FROM entries WHERE text = ?1 AND kind = 'phrase'", -1, &find, nullptr);
    sqlite3_bind_text(find, 1, full_text.c_str(), -1, SQLITE_TRANSIENT);
    const bool exists = sqlite3_step(find) == SQLITE_ROW;
    sqlite3_finalize(find);
    const std::string pinyin = JoinPinyin(tokens);
    if (exists) {
      sqlite3_stmt* bump = nullptr;
      sqlite3_prepare_v2(db_->raw,
                          "UPDATE entries SET count = count + 1, last_commit_unix = ?2, pinyin = ?3 "
                          "WHERE text = ?1",
                          -1, &bump, nullptr);
      sqlite3_bind_text(bump, 1, full_text.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(bump, 2, now_unix);
      sqlite3_bind_text(bump, 3, pinyin.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(bump);
      sqlite3_finalize(bump);
    } else {
      sqlite3_stmt* seen = nullptr;
      sqlite3_prepare_v2(db_->raw, "SELECT 1 FROM seen_phrase WHERE text = ?1", -1, &seen, nullptr);
      sqlite3_bind_text(seen, 1, full_text.c_str(), -1, SQLITE_TRANSIENT);
      const bool again = sqlite3_step(seen) == SQLITE_ROW;
      sqlite3_finalize(seen);
      if (!again) {
        sqlite3_stmt* mark = nullptr;
        sqlite3_prepare_v2(db_->raw, "INSERT INTO seen_phrase(text) VALUES(?1)", -1, &mark, nullptr);
        sqlite3_bind_text(mark, 1, full_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(mark);
        sqlite3_finalize(mark);
      } else {
        sqlite3_stmt* add = nullptr;
        sqlite3_prepare_v2(db_->raw,
                            "INSERT INTO entries(text, pinyin, count, last_commit_unix, kind) "
                            "VALUES(?1, ?2, 2, ?3, 'phrase')",
                            -1, &add, nullptr);
        sqlite3_bind_text(add, 1, full_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(add, 2, pinyin.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(add, 3, now_unix);
        sqlite3_step(add);
        sqlite3_finalize(add);
        sqlite3_stmt* drop = nullptr;
        sqlite3_prepare_v2(db_->raw, "DELETE FROM seen_phrase WHERE text = ?1", -1, &drop, nullptr);
        sqlite3_bind_text(drop, 1, full_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(drop);
        sqlite3_finalize(drop);
      }
    }
  }
  Exec(db_->raw, "COMMIT");
}

std::string Lexicon::Apply(const std::string& recognized, const std::vector<Token>& tokens, std::int64_t now_unix) {
  sqlite3_stmt* purge = nullptr;
  sqlite3_prepare_v2(db_->raw, "DELETE FROM entries WHERE (?1 - last_commit_unix) > ?2", -1, &purge, nullptr);
  sqlite3_bind_int64(purge, 1, now_unix);
  sqlite3_bind_int64(purge, 2, kDropSeconds);
  sqlite3_step(purge);
  sqlite3_finalize(purge);

  sqlite3_stmt* query = nullptr;
  sqlite3_prepare_v2(db_->raw,
                      "SELECT text, pinyin, count, last_commit_unix, kind FROM entries "
                      "WHERE count >= 2 AND (?1 - last_commit_unix) <= ?2",
                      -1, &query, nullptr);
  sqlite3_bind_int64(query, 1, now_unix);
  sqlite3_bind_int64(query, 2, kBiasSeconds);

  struct Row {
    std::string text;
    std::string pinyin;
    int count;
    std::int64_t when;
    std::string kind;
  };
  std::vector<Row> rows;
  while (sqlite3_step(query) == SQLITE_ROW) {
    Row row;
    row.text = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    row.pinyin = reinterpret_cast<const char*>(sqlite3_column_text(query, 1));
    row.count = sqlite3_column_int(query, 2);
    row.when = sqlite3_column_int64(query, 3);
    row.kind = reinterpret_cast<const char*>(sqlite3_column_text(query, 4));
    rows.push_back(std::move(row));
  }
  sqlite3_finalize(query);

  bool full_ok = !tokens.empty();
  for (const Token& token : tokens) {
    if (token.pinyin.empty()) full_ok = false;
  }
  if (full_ok) {
    const std::string full = JoinPinyin(tokens);
    const Row* best = nullptr;
    for (const Row& row : rows) {
      if (row.kind != "phrase" || row.pinyin != full) continue;
      if (best == nullptr || Better(row.count, row.when, best->count, best->when)) best = &row;
    }
    if (best != nullptr) return best->text;
  }

  std::string result;
  for (const Token& token : tokens) {
    const Row* best = nullptr;
    if (!token.pinyin.empty()) {
      for (const Row& row : rows) {
        if (row.kind != "word" || row.pinyin != token.pinyin) continue;
        if (best == nullptr || Better(row.count, row.when, best->count, best->when)) best = &row;
      }
    }
    result += best != nullptr ? best->text : token.text;
  }
  if (tokens.empty()) return recognized;
  return result;
}

std::vector<Entry> Lexicon::Recent(int limit) {
  sqlite3_stmt* query = nullptr;
  sqlite3_prepare_v2(db_->raw,
                      "SELECT text, pinyin, count, last_commit_unix, kind FROM entries "
                      "ORDER BY last_commit_unix DESC LIMIT ?1",
                      -1, &query, nullptr);
  sqlite3_bind_int(query, 1, limit);
  std::vector<Entry> entries;
  while (sqlite3_step(query) == SQLITE_ROW) {
    Entry entry;
    entry.text = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
    entry.pinyin = reinterpret_cast<const char*>(sqlite3_column_text(query, 1));
    entry.count = sqlite3_column_int(query, 2);
    entry.last_commit_unix = sqlite3_column_int64(query, 3);
    entry.kind = reinterpret_cast<const char*>(sqlite3_column_text(query, 4));
    entries.push_back(std::move(entry));
  }
  sqlite3_finalize(query);
  return entries;
}

bool Lexicon::Delete(const std::string& text) {
  sqlite3_stmt* drop = nullptr;
  sqlite3_prepare_v2(db_->raw, "DELETE FROM entries WHERE text = ?1", -1, &drop, nullptr);
  sqlite3_bind_text(drop, 1, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(drop);
  sqlite3_finalize(drop);
  const int removed = sqlite3_changes(db_->raw);
  sqlite3_stmt* seen = nullptr;
  sqlite3_prepare_v2(db_->raw, "DELETE FROM seen_phrase WHERE text = ?1", -1, &seen, nullptr);
  sqlite3_bind_text(seen, 1, text.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(seen);
  sqlite3_finalize(seen);
  return removed > 0 || sqlite3_changes(db_->raw) > 0;
}
