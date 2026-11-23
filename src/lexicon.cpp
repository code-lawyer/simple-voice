#include "lexicon.hpp"
#include "sha256.hpp"
#include "sqlite3.h"
#include "utf8.hpp"
#include <mutex>
#include <stdexcept>
#include <utility>
#include <ctime>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <filesystem>
#include <set>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
constexpr std::int64_t kDay = 86400;
constexpr std::int64_t kDropSeconds = 365 * kDay;
void Check(sqlite3* db, int code) {
  if((code&255)==SQLITE_BUSY || (code&255)==SQLITE_LOCKED) throw LexiconBusy(sqlite3_errmsg(db));
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
  void Blob(int index, const std::string& bytes) {
    Check(db_,sqlite3_bind_blob(stmt_,index,bytes.data(),static_cast<int>(bytes.size()),SQLITE_TRANSIENT));
  }
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
  std::string Blob(int index) const {
    const auto* bytes=static_cast<const char*>(sqlite3_column_blob(stmt_,index));
    return bytes?std::string(bytes,sqlite3_column_bytes(stmt_,index)):std::string{};
  }
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
bool Purge(sqlite3* db, std::int64_t now) {
  Statement purge(db, "DELETE FROM entries WHERE (?1 - last_commit_unix) > ?2");
  purge.Int(1, now); purge.Int(2, kDropSeconds); purge.Done();
  Statement seen(db, "DELETE FROM seen_phrase WHERE (?1 - last_seen_unix) > ?2");
  seen.Int(1, now); seen.Int(2, kDropSeconds); seen.Done();
  Statement terms(db, "DELETE FROM personal_terms WHERE (?1-last_commit_unix)>?2");
  terms.Int(1,now); terms.Int(2,kDropSeconds); terms.Done();
  return sqlite3_changes(db) != 0;
}
Entry ReadEntry(const Statement& query) {
  return {query.Text(0), query.Text(1), static_cast<int>(query.Int(2)), query.Int(3), query.Text(4)};
}
bool ValidTerm(const std::string& text,const std::string& code) {
  return !text.empty() && CodePointCount(text)<=128 &&
      MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0)>0 &&
      std::none_of(text.begin(),text.end(),[](unsigned char c){return c<32 || c==127;}) &&
      !code.empty() && code.size()<=256 &&
      std::all_of(code.begin(),code.end(),[](char c){return c>='a' && c<='z';});
}
void CheckTermCapacity(sqlite3* db,const std::string& text) {
  Statement count(db,"SELECT count(*) FROM personal_terms WHERE text<>?1");
  count.Text(1,text);
  if(count.Row() && count.Int(0)>=10000) throw std::invalid_argument("个人词语已达到10000条，请先整理后再添加");
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
  Exec(db_->raw,"PRAGMA secure_delete=ON;"
    "CREATE TABLE IF NOT EXISTS review_settings(id INTEGER PRIMARY KEY,enabled INTEGER NOT NULL,generation INTEGER NOT NULL);"
    "INSERT OR IGNORE INTO review_settings VALUES(1,0,0);"
    "CREATE TABLE IF NOT EXISTS review_hits(word TEXT NOT NULL,at INTEGER NOT NULL,source INTEGER NOT NULL,"
    "hits INTEGER NOT NULL,reason INTEGER NOT NULL,PRIMARY KEY(word,at,source));"
    "CREATE INDEX IF NOT EXISTS review_expiry ON review_hits(at);"
    "CREATE TABLE IF NOT EXISTS review_examples(word TEXT PRIMARY KEY,example TEXT NOT NULL,at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS review_dismissed(digest BLOB PRIMARY KEY,at_us INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS review_dismissed_expiry ON review_dismissed(at_us);"
    "CREATE TABLE IF NOT EXISTS review_speech_pending(word TEXT PRIMARY KEY);");
  ExpireReview(std::time(nullptr));
}
Lexicon::~Lexicon() = default;

bool Lexicon::Commit(const std::string& text, std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  const bool candidate_table_changed = Purge(db_->raw, now);
  // Ordinary confirmation only refreshes terms the user explicitly saved.
  Statement touch(db_->raw,
      "UPDATE personal_terms SET count=MIN(count+1,1000000),last_commit_unix=?2 WHERE instr(?1,text)>0");
  touch.Text(1,text); touch.Int(2,now); touch.Done();
  transaction.Commit();
  return candidate_table_changed;
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
  if(!ValidTerm(text,code))
    throw std::invalid_argument("词语须为1至128字，输入码仅使用小写英文字母");
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  if(!previous.empty() && previous!=text) {
    Statement duplicate(db_->raw,"SELECT 1 FROM personal_terms WHERE text=?1");
    duplicate.Text(1,text);
    if(duplicate.Row()) throw std::invalid_argument("这个词语已经存在，请编辑已有词语");
    Statement remove(db_->raw,"DELETE FROM personal_terms WHERE text=?1"); remove.Text(1,previous); remove.Done();
  }
  CheckTermCapacity(db_->raw,text);
  Statement save(db_->raw,
      "INSERT INTO personal_terms(text,pinyin,count,last_commit_unix,kind) VALUES(?1,?2,0,?3,'user') "
      "ON CONFLICT(text) DO UPDATE SET pinyin=excluded.pinyin,last_commit_unix=excluded.last_commit_unix");
  save.Text(1,text); save.Text(2,code); save.Int(3,now); save.Done(); transaction.Commit();
}
bool Lexicon::Delete(const std::string& text,Entry* removed) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement drop(db_->raw,"DELETE FROM personal_terms WHERE text=?1 RETURNING text,pinyin,count,last_commit_unix,kind");
  drop.Text(1,text);
  if(!drop.Row()) return false;
  Entry deleted;
  if(removed) deleted=ReadEntry(drop);
  drop.Done();
  if(removed) *removed=std::move(deleted);
  return true;
}
bool Lexicon::RestoreDeletedTerm(const Entry& removed) {
  if(!ValidTerm(removed.text,removed.pinyin) || removed.count<0 || removed.count>1000000 || removed.last_commit_unix<0)
    throw std::invalid_argument("无法恢复这条词语的数据");
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  CheckTermCapacity(db_->raw,removed.text);
  Statement add(db_->raw,"INSERT INTO personal_terms(text,pinyin,count,last_commit_unix,kind) "
      "VALUES(?1,?2,?3,?4,'user') ON CONFLICT(text) DO NOTHING");
  add.Text(1,removed.text); add.Text(2,removed.pinyin); add.Int(3,removed.count); add.Int(4,removed.last_commit_unix);
  add.Done();
  const bool restored=sqlite3_changes(db_->raw)>0;
  transaction.Commit();
  return restored;
}
void Lexicon::ReplaceTerms(const std::vector<Entry>& rows,const std::function<void()>& update) {
  if(rows.size()>10000)throw std::invalid_argument("备份词语数量超出限制");
  std::set<std::string> seen;
  for(const auto& row:rows) {
    if(!ValidTerm(row.text,row.pinyin) ||
       row.count<0 || row.count>1000000 || row.last_commit_unix<0 || !seen.insert(row.text).second)
      throw std::invalid_argument("备份词语格式无效");
  }
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);Exec(db_->raw,"DELETE FROM personal_terms");
  Statement add(db_->raw,"INSERT INTO personal_terms(text,pinyin,count,last_commit_unix,kind) VALUES(?1,?2,?3,?4,'user')");
  for(const auto& row:rows) {
    add.Text(1,row.text);add.Text(2,row.pinyin);add.Int(3,row.count);add.Int(4,row.last_commit_unix);add.Done();add.Reset();
  }
  if(update)update();transaction.Commit();
}
bool Lexicon::ExportTerms(const std::wstring& file) {
  const std::filesystem::path target(file), temp(file+L".tmp");
  std::error_code error;
  if(!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path(),error);
  if(error) return false;

  // All engines sharing the dictionary serialize the snapshot and its publication.
  // An older exporter must not replace a table produced from a newer database state.
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction transaction(db_->raw);
  Statement terms(db_->raw,"SELECT text,pinyin FROM personal_terms ORDER BY text");
  std::ostringstream body;
  body<<"# Rime table: explicitly saved personal terms. Generated locally.\n";
  // Official TSV directive: following rows are data, including # and #@ terms.
  body<<"# no comment\n";
  while(terms.Row()) body<<terms.Text(0)<<'\t'<<terms.Text(1)<<"\t100\n";
  const auto content=body.str();
  {
    std::ifstream existing(target,std::ios::binary|std::ios::ate);
    if(existing && existing.tellg()==static_cast<std::streamoff>(content.size())) {
      existing.seekg(0);
      if(std::equal(content.begin(),content.end(),std::istreambuf_iterator<char>(existing),
                    std::istreambuf_iterator<char>{})) {
        transaction.Commit();
        return true;
      }
    }
  }
  std::ofstream out(temp,std::ios::binary|std::ios::trunc);
  if(!out) return false;
  out.write(content.data(),content.size());
  out.close();
  if(!out || !MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
  transaction.Commit();
  return true;
}

namespace {
std::int64_t Microseconds(std::chrono::system_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
}
std::uint64_t CollectionGeneration(sqlite3* db) {
  Statement row(db,"SELECT generation FROM review_settings WHERE id=1 AND enabled");
  return row.Row()?row.Int(0):0;
}
}
ReviewState Lexicon::ReviewSettings(std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  review_epoch_=CollectionGeneration(db_->raw);
  Statement earliest(db_->raw,"SELECT MIN(at) FROM review_hits WHERE at>?1");
  earliest.Int(1,now-kReviewWindow); earliest.Row();
  return {ReviewEpoch()!=0,earliest.Int(0)};
}

void Lexicon::SetReviewEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  Statement update(db_->raw,"UPDATE review_settings SET enabled=?1,generation=generation+1 WHERE id=1 AND enabled<>?1");
  update.Int(1,enabled); update.Done();
  const auto generation=CollectionGeneration(db_->raw); tx.Commit();
  review_epoch_=generation;
}

void Lexicon::ClearReview() {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  Exec(db_->raw,"DELETE FROM review_hits; DELETE FROM review_examples; DELETE FROM review_dismissed;");
  Exec(db_->raw,"UPDATE review_settings SET generation=generation+1 WHERE id=1");
  const auto generation=CollectionGeneration(db_->raw); tx.Commit(); review_epoch_=generation;
}

void Lexicon::ExpireReview(std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  for(const char* sql:{"DELETE FROM review_hits WHERE at<=?1","DELETE FROM review_examples WHERE at<=?1"}) {
    Statement erase(db_->raw,sql); erase.Int(1,now-kReviewWindow); erase.Done();
  }
  Statement dismissed(db_->raw,"DELETE FROM review_dismissed WHERE at_us<=?1");
  dismissed.Int(1,Microseconds(std::chrono::system_clock::from_time_t(now-kReviewWindow)));
  dismissed.Done();
  tx.Commit();
  review_epoch_=CollectionGeneration(db_->raw);
}

void Lexicon::RecordReview(const std::vector<ReviewWord>& words,WordSource source,
                           std::int64_t at,std::uint64_t epoch) {
  RecordReview(words,source,std::chrono::system_clock::from_time_t(at),epoch);
}

void Lexicon::RecordReview(const std::vector<ReviewWord>& words,WordSource source,
                           std::chrono::system_clock::time_point receipt,std::uint64_t epoch) {
  if(!epoch || words.empty()) return;
  const auto at=std::chrono::duration_cast<std::chrono::seconds>(receipt.time_since_epoch()).count();
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  review_epoch_=CollectionGeneration(db_->raw);
  if(ReviewEpoch()!=epoch) return;
  // An earlier receipt can finish segmentation after another window has dismissed a word.
  Statement dismissed(db_->raw,"SELECT digest FROM review_dismissed WHERE at_us>=?1");
  dismissed.Int(1,Microseconds(receipt));
  std::set<std::string> ignored;
  while(dismissed.Row()) ignored.insert(dismissed.Blob(0));
  Statement hit(db_->raw,"INSERT INTO review_hits VALUES(?1,?2,?3,?4,?5) "
    "ON CONFLICT(word,at,source) DO UPDATE SET hits=hits+excluded.hits,reason=reason|excluded.reason");
  Statement example(db_->raw,"INSERT INTO review_examples VALUES(?1,?2,?3) ON CONFLICT(word) DO UPDATE "
    "SET example=excluded.example,at=excluded.at WHERE excluded.at>=review_examples.at");
  for(const auto& word:words) {
    if(!ignored.empty() && ignored.count(Sha256(word.text))) continue;
    hit.Text(1,word.text); hit.Int(2,at); hit.Int(3,static_cast<int>(source));
    hit.Int(4,word.count); hit.Int(5,word.reasons); hit.Done(); hit.Reset();
    example.Text(1,word.text); example.Text(2,word.example); example.Int(3,at); example.Done(); example.Reset();
  }
  tx.Commit();
}

std::vector<ReviewWord> Lexicon::ReviewWords(ReviewFilter filter,const std::string& query,
                                            int limit,int offset,std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement rows(db_->raw,
    "SELECT h.word,e.example,SUM(h.hits),MAX(h.reason&1)|MAX(h.reason&2)|MAX(h.reason&4),MAX(h.source&1)|MAX(h.source&2) "
    "FROM review_hits h JOIN review_examples e ON e.word=h.word AND e.at>?1 "
    "WHERE h.at>?1 AND instr(lower(h.word),lower(?2))>0 AND NOT EXISTS "
    "(SELECT 1 FROM personal_terms t WHERE t.text=h.word) GROUP BY h.word "
    "HAVING (?3=2 OR (?3=0 AND SUM(h.hits)>=2) OR (?3=1 AND MAX(h.reason)>0)) "
    "ORDER BY CASE WHEN ?3=0 AND (length(h.word)=1 OR h.word IN ('我们','你们','他们','这个','那个','就是','然后','可以')) "
    "THEN 1 ELSE 0 END,SUM(h.hits) DESC,MAX(h.at) DESC,h.word LIMIT ?4 OFFSET ?5");
  rows.Int(1,now-kReviewWindow); rows.Text(2,query); rows.Int(3,static_cast<int>(filter));
  rows.Int(4,std::clamp(limit,1,501)); rows.Int(5,std::max(0,offset));
  std::vector<ReviewWord> result;
  while(rows.Row()) result.push_back({rows.Text(0),rows.Text(1),static_cast<int>(rows.Int(2)),
                                      static_cast<int>(rows.Int(3)),static_cast<int>(rows.Int(4))});
  return result;
}

namespace {
constexpr char kLiveReviewCandidate[]=
  "SELECT 1 FROM review_hits h WHERE h.word=?1 AND h.at>?2 AND NOT EXISTS "
  "(SELECT 1 FROM personal_terms t WHERE t.text=h.word) LIMIT 1";

bool IsLiveCandidate(Statement& query,const std::string& word,std::int64_t now) {
  query.Text(1,word);
  query.Int(2,now-kReviewWindow);
  const bool live=query.Row();
  query.Reset();
  return live;
}

class DismissReviewWords {
 public:
  explicit DismissReviewWords(sqlite3* db)
      :hits_(db,"DELETE FROM review_hits WHERE word=?1"),
       example_(db,"DELETE FROM review_examples WHERE word=?1"),
       dismissed_(db,"INSERT INTO review_dismissed(digest,at_us) VALUES(?1,?2) "
                     "ON CONFLICT(digest) DO UPDATE SET at_us=MAX(at_us,excluded.at_us)"),
       time_(Microseconds(std::chrono::system_clock::now())) {}

  void Forget(const std::string& word) {
    for(Statement* row:{&hits_,&example_}) {
      row->Text(1,word); row->Done(); row->Reset();
    }
    // A digest lets older queued receipts be discarded without keeping the dismissed spelling or example.
    dismissed_.Blob(1,Sha256(word)); dismissed_.Int(2,time_); dismissed_.Done(); dismissed_.Reset();
  }
 private:
  Statement hits_,example_,dismissed_;
  std::int64_t time_;
};
}

void Lexicon::SkipReview(const std::vector<std::string>& candidates) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  DismissReviewWords dismissed(db_->raw);
  for(const auto& word:candidates) dismissed.Forget(word);
  tx.Commit();
}

bool Lexicon::ReviewCandidateExists(const std::string& candidate,std::int64_t now) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement row(db_->raw,kLiveReviewCandidate);
  return IsLiveCandidate(row,candidate,now);
}

void Lexicon::AcceptReview(const std::vector<ReviewChoice>& choices,std::int64_t now) {
  if(choices.empty()) return;
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  Statement alive(db_->raw,kLiveReviewCandidate);
  // Validate the original selection before a correction can itself create another selected word.
  for(const auto& choice:choices) {
    if(!ValidTerm(choice.word,choice.code)) throw std::invalid_argument("请核对词语与小写拼音输入码");
    if(choice.speech && Utf8ToWide(choice.word).find_first_of(L",，、")!=std::wstring::npos)
      throw std::invalid_argument("语音专名不能包含分隔符");
    if(!IsLiveCandidate(alive,choice.candidate,now))
      throw std::invalid_argument("所选候选已过期或已处理，请刷新");
  }
  Statement existing(db_->raw,"SELECT pinyin FROM personal_terms WHERE text=?1");
  Statement count(db_->raw,"SELECT count(*) FROM personal_terms");
  count.Row();
  auto term_count=count.Int(0);
  count.Reset();
  Statement add(db_->raw,"INSERT INTO personal_terms(text,pinyin,count,last_commit_unix,kind) "
     "VALUES(?1,?2,0,?3,'user')");
  Statement hint(db_->raw,"INSERT OR IGNORE INTO review_speech_pending VALUES(?1)");
  DismissReviewWords dismissed(db_->raw);
  for(const auto& choice:choices) {
    existing.Text(1,choice.word);
    if(existing.Row()) {
      if(existing.Text(0)!=choice.code)
        throw std::invalid_argument("词语“"+choice.word+"”已有不同的拼音输入码，请核对后再保存");
    } else {
      if(term_count>=10000) throw std::invalid_argument("个人词语已达到10000条，请先整理后再添加");
      add.Text(1,choice.word); add.Text(2,choice.code); add.Int(3,now); add.Done(); add.Reset();
      ++term_count;
    }
    existing.Reset();
    if(choice.speech) { hint.Text(1,choice.word); hint.Done(); hint.Reset(); }
    if(!choice.from_example || choice.word==choice.candidate) dismissed.Forget(choice.candidate);
  }
  tx.Commit();
}

std::vector<std::string> Lexicon::PendingSpeechNames() {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Statement row(db_->raw,"SELECT word FROM review_speech_pending ORDER BY word");
  std::vector<std::string> result; while(row.Row()) result.push_back(row.Text(0));
  return result;
}

void Lexicon::CompleteSpeechNames(const std::vector<std::string>& names) {
  std::lock_guard<std::mutex> lock(db_->mutex);
  Transaction tx(db_->raw);
  Statement remove(db_->raw,"DELETE FROM review_speech_pending WHERE word=?1");
  for(const auto& name:names) {remove.Text(1,name); remove.Done(); remove.Reset();}
  tx.Commit();
}
