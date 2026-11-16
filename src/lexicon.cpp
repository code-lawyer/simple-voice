#include "lexicon.hpp"
#include "sqlite3.h"
#include "utf8.hpp"
#include <mutex>
#include <stdexcept>
#include <utility>
#include <ctime>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <filesystem>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
constexpr std::int64_t kDay = 86400;
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
  Statement terms(db, "DELETE FROM personal_terms WHERE (?1-last_commit_unix)>?2");
  terms.Int(1,now); terms.Int(2,kDropSeconds); terms.Done();
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
      "CREATE TABLE IF NOT EXISTS seen_phrase (text TEXT PRIMARY KEY);"
      "CREATE TABLE IF NOT EXISTS personal_terms (text TEXT PRIMARY KEY,pinyin TEXT NOT NULL,"
      "count INTEGER NOT NULL,last_commit_unix INTEGER NOT NULL,kind TEXT NOT NULL DEFAULT 'user');");
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

void Lexicon::Commit(const std::string& text, const std::vector<Token>&, std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  Purge(db_->raw, now);
  // Ordinary confirmation only refreshes terms the user explicitly saved.
  Statement touch(db_->raw,
      "UPDATE personal_terms SET count=MIN(count+1,1000000),last_commit_unix=?2 WHERE instr(?1,text)>0");
  touch.Text(1,text); touch.Int(2,now); touch.Done();
  transaction.Commit();
}
std::string Lexicon::Apply(const std::string& recognized, const std::vector<Token>&, std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Purge(db_->raw, now);
  // Frequency and homophones are not sufficient evidence to rewrite ASR.
  return recognized;
}
std::vector<Entry> Lexicon::Recent(int limit) { return Search("", limit); }
std::vector<Entry> Lexicon::Search(const std::string& query, int limit, int offset) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement statement(db_->raw,
      "SELECT text,pinyin,count,last_commit_unix,kind FROM personal_terms "
      "WHERE instr(lower(text),lower(?1))>0 OR instr(lower(pinyin),lower(?1))>0 "
      "ORDER BY last_commit_unix DESC,text LIMIT ?2 OFFSET ?3");
  statement.Text(1,query); statement.Int(2,std::clamp(limit,0,10000)); statement.Int(3,std::max(0,offset));
  std::vector<Entry> rows;
  while(statement.Row()) rows.push_back(ReadEntry(statement));
  return rows;
}
int Lexicon::Count() {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement statement(db_->raw,"SELECT count(*) FROM personal_terms");
  return statement.Row()?static_cast<int>(statement.Int(0)):0;
}
int Lexicon::LegacyCount() {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement statement(db_->raw,"SELECT (SELECT count(*) FROM entries)+(SELECT count(*) FROM seen_phrase)");
  return statement.Row()?static_cast<int>(statement.Int(0)):0;
}
void Lexicon::ClearLegacy() {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  Exec(db_->raw,"DELETE FROM entries; DELETE FROM seen_phrase;");
  transaction.Commit();
}
void Lexicon::SaveTerm(const std::string& text, const std::string& code, std::int64_t now, const std::string& previous) {
  if(text.empty() || CodePointCount(text)>128 || MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)==0 ||
      std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32 || c==127;}) ||
      code.empty() || code.size()>256 ||
      std::any_of(code.begin(),code.end(),[](unsigned char c){return !(c>='a'&&c<='z');}))
    throw std::invalid_argument("词语须为1至128字，输入码仅使用小写英文字母");
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  if(!previous.empty() && previous!=text) {
    Statement duplicate(db_->raw,"SELECT 1 FROM personal_terms WHERE text=?1");
    duplicate.Text(1,text);
    if(duplicate.Row()) throw std::invalid_argument("这个词语已经存在，请编辑已有词语");
    Statement remove(db_->raw,"DELETE FROM personal_terms WHERE text=?1"); remove.Text(1,previous); remove.Done();
  }
  Statement count(db_->raw,"SELECT count(*) FROM personal_terms WHERE text<>?1"); count.Text(1,text);
  if(count.Row() && count.Int(0)>=10000) throw std::invalid_argument("个人词语已达到10000条，请先整理后再添加");
  Statement save(db_->raw,
      "INSERT INTO personal_terms(text,pinyin,count,last_commit_unix,kind) VALUES(?1,?2,0,?3,'user') "
      "ON CONFLICT(text) DO UPDATE SET pinyin=excluded.pinyin,last_commit_unix=excluded.last_commit_unix");
  save.Text(1,text); save.Text(2,code); save.Int(3,now); save.Done(); transaction.Commit();
}
bool Lexicon::Delete(const std::string& text) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement drop(db_->raw,"DELETE FROM personal_terms WHERE text=?1");
  drop.Text(1,text); drop.Done(); return sqlite3_changes(db_->raw)>0;
}
bool Lexicon::ExportTerms(const std::wstring& file) {
  auto rows=Search("",10000);
  std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.text<b.text;});
  const std::filesystem::path target(file), temp(file+L".tmp");
  std::error_code error; std::filesystem::create_directories(target.parent_path(),error);
  if(error) return false;
  std::ostringstream body;
  body<<"# Rime table: explicitly saved personal terms. Generated locally.\n";
  // Official TSV directive: following rows are data, including # and #@ terms.
  body<<"# no comment\n";
  for(const auto& row:rows) body<<row.text<<'\t'<<row.pinyin<<"\t100\n";
  const auto content=body.str();
  { std::ifstream existing(target,std::ios::binary);
    if(existing && std::string((std::istreambuf_iterator<char>(existing)),{})==content) return true; }
  { std::ofstream out(temp,std::ios::binary|std::ios::trunc); if(!out) return false;
    out.write(content.data(),content.size());
    out.flush(); if(!out) return false;
  }
  return MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
}
