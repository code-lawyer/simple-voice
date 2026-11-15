#include "lexicon.hpp"
#include "sqlite3.h"
#include "utf8.hpp"
#include <mutex>
#include <stdexcept>
#include <utility>
#include <ctime>

namespace {
constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kBiasSeconds = 180 * kDay;
constexpr std::int64_t kDropSeconds = 365 * kDay;
void Check(sqlite3* db, int code) {
  if (code != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
void Exec(sqlite3* db, const char* sql) { Check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) : db_(db) {
    const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
    if (rc != SQLITE_OK) { sqlite3_finalize(stmt_); stmt_ = nullptr; Check(db, rc); }
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  void Text(int index, const std::string& text) { Check(db_, sqlite3_bind_text(stmt_, index, text.c_str(), -1, SQLITE_TRANSIENT)); }
  void Int(int index, std::int64_t value) { Check(db_, sqlite3_bind_int64(stmt_, index, value)); }
  bool Row() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    Check(db_, rc); return false;
  }
  void Done() { if (Row()) throw std::runtime_error("unexpected SQLite row"); }
  void Reset() { Check(db_, sqlite3_reset(stmt_)); Check(db_, sqlite3_clear_bindings(stmt_)); }
  std::string Text(int index) const {
    const auto* text = sqlite3_column_text(stmt_, index);
    return text ? reinterpret_cast<const char*>(text) : "";
  }
  std::int64_t Int(int index) const { return sqlite3_column_int64(stmt_, index); }
 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};
class Transaction {
 public:
  explicit Transaction(sqlite3* db) : db_(db) { Exec(db_, "BEGIN IMMEDIATE"); }
  ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
  void Commit() { Exec(db_, "COMMIT"); committed_ = true; }
 private:
  sqlite3* db_;
  bool committed_ = false;
};
void Purge(sqlite3* db, std::int64_t now) {
  Statement purge(db, "DELETE FROM entries WHERE (?1 - last_commit_unix) > ?2");
  purge.Int(1, now); purge.Int(2, kDropSeconds); purge.Done();
  Statement seen(db, "DELETE FROM seen_phrase WHERE (?1 - last_seen_unix) > ?2");
  seen.Int(1, now); seen.Int(2, kDropSeconds); seen.Done();
}
std::string JoinPinyin(const std::vector<Token>& tokens) {
  std::string joined;
  for (const Token& token : tokens) {
    if (token.pinyin.empty()) continue;
    if (!joined.empty()) joined.push_back(' ');
    joined += token.pinyin;
  }
  return joined;
}
bool Better(const Entry& row, const Entry* best) {
  return best == nullptr || row.count > best->count ||
      (row.count == best->count && row.last_commit_unix > best->last_commit_unix);
}
Entry ReadEntry(const Statement& query) {
  return {query.Text(0), query.Text(1), static_cast<int>(query.Int(2)), query.Int(3), query.Text(4)};
}
}  // namespace

struct Lexicon::Db {
  sqlite3* raw = nullptr;
  std::mutex mutex;
  ~Db() { if (raw) sqlite3_close(raw); }
};

Lexicon::Lexicon(const std::string& path) : db_(std::make_unique<Db>()) {
  const int rc = sqlite3_open_v2(path.c_str(), &db_->raw,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  Check(db_->raw, rc);
  Check(db_->raw, sqlite3_busy_timeout(db_->raw, 250));
  Exec(db_->raw,
      "CREATE TABLE IF NOT EXISTS entries (text TEXT PRIMARY KEY,pinyin TEXT NOT NULL,"
      "count INTEGER NOT NULL,last_commit_unix INTEGER NOT NULL,kind TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS seen_phrase (text TEXT PRIMARY KEY);");
  // Legacy rows have no date: start their retention period once, at upgrade.
  Transaction migration(db_->raw);
  bool dated = false;
  {
    Statement columns(db_->raw, "PRAGMA table_info(seen_phrase)");
    while (columns.Row()) if (columns.Text(1) == "last_seen_unix") dated = true;
  }
  if (!dated) {
    Exec(db_->raw, "ALTER TABLE seen_phrase ADD COLUMN last_seen_unix INTEGER NOT NULL DEFAULT 0");
    Statement date(db_->raw, "UPDATE seen_phrase SET last_seen_unix=?1");
    date.Int(1, static_cast<std::int64_t>(std::time(nullptr))); date.Done();
  }
  migration.Commit();
  Purge(db_->raw, static_cast<std::int64_t>(std::time(nullptr)));
}
Lexicon::~Lexicon() = default;

void Lexicon::Commit(const std::string& text, const std::vector<Token>& tokens, std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  Purge(db_->raw, now);
  Statement word(db_->raw,
      "INSERT INTO entries(text,pinyin,count,last_commit_unix,kind) VALUES(?1,?2,1,?3,'word') "
      "ON CONFLICT(text) DO UPDATE SET count=count+1,last_commit_unix=excluded.last_commit_unix,pinyin=excluded.pinyin");
  for (const Token& token : tokens) {
    const auto length = CodePointCount(token.text);
    if (length < 2 || length > 8) continue;
    word.Reset(); word.Text(1, token.text); word.Text(2, token.pinyin); word.Int(3, now); word.Done();
  }
  if (CodePointCount(text) > 8) {
    bool exists = false;
    {
      Statement find(db_->raw, "SELECT 1 FROM entries WHERE text=?1 AND kind='phrase'");
      find.Text(1, text); exists = find.Row();
    }
    const auto pinyin = JoinPinyin(tokens);
    if (exists) {
      Statement bump(db_->raw, "UPDATE entries SET count=count+1,last_commit_unix=?2,pinyin=?3 WHERE text=?1");
      bump.Text(1, text); bump.Int(2, now); bump.Text(3, pinyin); bump.Done();
    } else {
      bool again = false;
      {
        Statement seen(db_->raw, "SELECT 1 FROM seen_phrase WHERE text=?1");
        seen.Text(1, text); again = seen.Row();
      }
      if (!again) {
        Statement mark(db_->raw, "INSERT INTO seen_phrase(text,last_seen_unix) VALUES(?1,?2)");
        mark.Text(1, text); mark.Int(2, now); mark.Done();
      } else {
        Statement add(db_->raw,
            "INSERT INTO entries(text,pinyin,count,last_commit_unix,kind) VALUES(?1,?2,2,?3,'phrase')");
        add.Text(1, text); add.Text(2, pinyin); add.Int(3, now); add.Done();
        Statement drop(db_->raw, "DELETE FROM seen_phrase WHERE text=?1");
        drop.Text(1, text); drop.Done();
      }
    }
  }
  transaction.Commit();
}
std::string Lexicon::Apply(const std::string& recognized, const std::vector<Token>& tokens, std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Purge(db_->raw, now);
  Statement query(db_->raw,
      "SELECT text,pinyin,count,last_commit_unix,kind FROM entries WHERE count>=2 AND (?1-last_commit_unix)<=?2");
  query.Int(1, now); query.Int(2, kBiasSeconds);
  std::vector<Entry> rows;
  while (query.Row()) rows.push_back(ReadEntry(query));
  const auto full = JoinPinyin(tokens);
  if (!full.empty()) {
    const Entry* best = nullptr;
    for (const auto& row : rows) {
      if (row.kind == "phrase" && row.pinyin == full && Better(row, best)) best = &row;
    }
    if (best) return best->text;
  }
  std::string result;
  for (const auto& token : tokens) {
    const Entry* best = nullptr;
    if (!token.pinyin.empty()) {
      for (const auto& row : rows) {
        if (row.kind == "word" && row.pinyin == token.pinyin && Better(row, best)) best = &row;
      }
    }
    result += best ? best->text : token.text;
  }
  return tokens.empty() ? recognized : result;
}
std::vector<Entry> Lexicon::Recent(int limit) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement query(db_->raw,
      "SELECT text,pinyin,count,last_commit_unix,kind FROM entries ORDER BY last_commit_unix DESC LIMIT ?1");
  query.Int(1, limit);
  std::vector<Entry> entries;
  while (query.Row()) entries.push_back(ReadEntry(query));
  return entries;
}
bool Lexicon::Delete(const std::string& text) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  Statement drop(db_->raw, "DELETE FROM entries WHERE text=?1");
  drop.Text(1, text); drop.Done();
  const int removed = sqlite3_changes(db_->raw);
  Statement seen(db_->raw, "DELETE FROM seen_phrase WHERE text=?1");
  seen.Text(1, text); seen.Done();
  const bool changed = removed > 0 || sqlite3_changes(db_->raw) > 0;
  transaction.Commit();
  return changed;
}
