#include "harness.hpp"
#include "ipc.hpp"
#include "lexicon.hpp"
#include "resample.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "sqlite3.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <stdexcept>

namespace {
std::string Frame(std::uint32_t length, unsigned char type = 9) {
  std::string result(5, '\0');
  for (int i = 0; i < 4; ++i) result[i] = static_cast<char>(length >> (i * 8));
  result[4] = static_cast<char>(type); return result;
}
struct TestDb {
  std::string path;
  sqlite3* raw = nullptr;
  TestDb() {
    static int serial = 0;
    path = "build/hardening-" + std::to_string(++serial) + ".sqlite";
    std::remove(path.c_str());
    if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) throw std::runtime_error("test db open");
  }
  ~TestDb() { sqlite3_close(raw); std::remove(path.c_str()); }
  void Sql(const std::string& sql) {
    if (sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(raw));
  }
  std::int64_t Scalar(const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(raw, sql, -1, &statement, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(raw));
    const int rc = sqlite3_step(statement);
    const auto result = rc == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : -1;
    sqlite3_finalize(statement); return result;
  }
};
template<class F> bool Throws(F action) {
  try { action(); } catch (const std::exception&) { return true; }
  return false;
}
}

void MalformedFramesAreRejectedWithoutConsumingOrLooping() {
  for (auto length : {0u, 0xFFFFFFFCu, 0xFFFFFFFFu, 65537u}) {
    Message message; std::size_t used = 99;
    CHECK(DecodeFrame(Frame(length), &message, &used) == DecodeStatus::Invalid);
    CHECK(used == 0);
  }
  for (auto bytes : {Frame(1, 255), Frame(2, 9), Frame(2, 8), Frame(2, 11)}) {
    CHECK(DecodeFrame(bytes, nullptr, nullptr) == DecodeStatus::Invalid);
  }
  // Adversarial lengths must always either reject or request bounded input.
  std::uint32_t seed = 1;
  for (int i = 0; i < 2000; ++i) {
    seed = seed * 1664525u + 1013904223u;
    std::size_t used = 1;
    auto frame = Frame(seed, 3);
    const auto status = DecodeFrame(frame, nullptr, &used);
    CHECK(status != DecodeStatus::Complete || (used > 0 && used <= frame.size()));
  }
}

void CorrelatedFramesSurviveFragmentationAndHaveBounds() {
  Message message; message.type = MsgType::InsertText;
  message.text = "你好，完整文本。"; message.request_id = 0x12345678ABCDEF01ULL;
  const auto bytes = Encode(message);
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    std::size_t used = 99;
    CHECK(DecodeFrame(std::string_view(bytes).substr(0, size), nullptr, &used) == DecodeStatus::Incomplete);
    CHECK(used == 0);
  }
  Message decoded; std::size_t used = 0;
  CHECK(DecodeFrame(bytes + bytes, &decoded, &used) == DecodeStatus::Complete);
  CHECK(used == bytes.size()); CHECK(decoded.request_id == message.request_id); CHECK(decoded.text == message.text);
  message.type = MsgType::InsertResult; message.text = "fail";
  CHECK(Decode(Encode(message), &decoded, &used)); CHECK(decoded.request_id == message.request_id);
  message.type = MsgType::CancelInsert; message.text.clear();
  CHECK(Decode(Encode(message), &decoded, &used)); CHECK(decoded.request_id == message.request_id);
  message.request_id = 0; CHECK(Encode(message).empty());
  message.type = MsgType::Draft; message.text.assign(kMaxFrameLength + 1, 'x');
  CHECK(Encode(message).empty());
}

void CommittingWaitsForConfirmationAndEmptyResultsNeverCommit() {
  Session session;
  session.Hotkey(); session.OnSpeechEnded(); session.OnFinal("完整文字"); session.Enter();
  std::string text;
  CHECK(session.phase() == Phase::Committing); CHECK(session.TakeCommit(&text));
  CHECK(!session.TakeLexiconWrite(&text));
  session.Hotkey(); session.Escape(); session.Enter();
  CHECK(session.phase() == Phase::Committing); CHECK(!session.mic_open()); CHECK(!session.TakeCommit(&text));
  session.ConfirmInserted(); CHECK(session.phase() == Phase::Idle);
  CHECK(session.TakeLexiconWrite(&text)); CHECK(text == "完整文字"); CHECK(!session.TakeLexiconWrite(&text));
  session.Hotkey(); session.Enter(); session.OnSpeechEnded(); session.OnFinal("");
  CHECK(session.phase() == Phase::Pending); CHECK(!session.TakeCommit(&text));
  session.Enter(); CHECK(!session.TakeCommit(&text)); CHECK(!session.TakeLexiconWrite(&text));
}

void SettingsRejectCorruptionAndUseTheSameHotkeyPolicy() {
  const char* bad[] = {"{", "{\"modifiers\":0,\"vk\":65}", "{\"modifiers\":8,\"vk\":119}",
      "{\"modifiers\":0,\"vk\":123}", "{\"modifiers\":-1,\"vk\":119}",
      "{\"modifiers\":0,\"vk\":999999999999999999999}", "{\"modifiers\":0,\"vk\":0119}",
      "{\"modifiers\":0,\"vk\":119}junk", "{\"vk\":119,\"vk\":120}",
      "{\"modifiers\":0,\"vk\":119,\"extra\":1}"};
  for (const auto* json : bad) {
    const auto settings = ParseSettings(json);
    CHECK(settings.hotkey.vk == DefaultHotkey().vk); CHECK(settings.hotkey.modifiers == DefaultHotkey().modifiers);
  }
  CHECK(ParseSettings("{ \n\"vk\": 119, \"modifiers\": 1\r\n}").hotkey.vk == 119);
  CHECK(ParseSettings(std::string(5000, '9')).hotkey.vk == DefaultHotkey().vk);
  const std::string path = "build/hardening-settings.json";
  CHECK(SaveSettings(path, {DefaultHotkey()}));
  CHECK(!SaveSettings(path, {{0, 65}}));
  CHECK(LoadSettings(path).hotkey.vk == DefaultHotkey().vk);
  CHECK(LoadSettings(std::wstring(L"build/hardening-settings.json")).hotkey.vk == DefaultHotkey().vk);
  std::remove(path.c_str());
}

void LexiconStatementFailuresRollBackAndCanRecover() {
  TestDb db; Lexicon lex(db.path); lex.SaveTerm("好词","haoci",1000);
  db.Sql("CREATE TRIGGER deny_word BEFORE INSERT ON personal_terms WHEN NEW.text='坏词' BEGIN SELECT RAISE(ABORT,'injected'); END");
  CHECK(Throws([&] { lex.SaveTerm("坏词","huaici",1000,"好词"); }));
  CHECK(lex.Search("好词").size()==1); CHECK(lex.Search("坏词").empty());
  db.Sql("DROP TRIGGER deny_word"); lex.SaveTerm("坏词","huaici",1000);
  db.Sql("CREATE TRIGGER deny_delete BEFORE DELETE ON personal_terms BEGIN SELECT RAISE(ABORT,'injected'); END");
  CHECK(Throws([&] { lex.Delete("好词"); })); CHECK(lex.Count()==2);
  db.Sql("DROP TRIGGER deny_delete"); CHECK(lex.Delete("好词"));
  db.Sql("DROP TABLE personal_terms"); CHECK(Throws([&] { lex.Recent(10); }));
}
void BusyLexiconTransactionDoesNotPoisonLaterWrites() {
  TestDb db; Lexicon lex(db.path); db.Sql("BEGIN IMMEDIATE");
  CHECK(Throws([&] { lex.SaveTerm("好词","haoci",1000); }));
  db.Sql("ROLLBACK"); lex.SaveTerm("好词","haoci",1000); CHECK(lex.Count()==1);
}

void LegacyPhraseRetentionStartsOnceAndExpiresAfter365Days() {
  TestDb db;
  db.Sql("CREATE TABLE seen_phrase(text TEXT PRIMARY KEY); INSERT INTO seen_phrase VALUES('第一次出现的完整长句')");
  const auto before = std::time(nullptr);
  { Lexicon lex(db.path); }
  const auto upgraded = db.Scalar("SELECT last_seen_unix FROM seen_phrase");
  CHECK(upgraded >= before && upgraded <= std::time(nullptr));
  db.Sql("UPDATE seen_phrase SET last_seen_unix=last_seen_unix-10000");
  {
    Lexicon lex(db.path);
    CHECK(db.Scalar("SELECT last_seen_unix FROM seen_phrase") == upgraded - 10000);
    lex.Apply("", {}, upgraded + 366 * 86400LL);
    CHECK(db.Scalar("SELECT count(*) FROM seen_phrase") == 0);
    lex.Commit("第一次出现的完整长句", {}, upgraded + 366 * 86400LL);
    CHECK(lex.Recent(10).empty());
    CHECK(db.Scalar("SELECT count(*) FROM seen_phrase") == 0);
  }
}

void StreamingResamplingIsIndependentOfPacketBoundaries() {
  for (int rate : {8000,16000,32000,44100,48000,96000}) {
    std::vector<std::int16_t> input(rate);
    for (int i = 0; i < rate; ++i) input[i] = static_cast<std::int16_t>(12000 * std::sin(2 * 3.141592653589793 * 997 * i / rate));
    const auto whole = ResampleTo16k(input.data(), input.size(), rate);
    CHECK(whole.size() == 16000);
    Resampler16k resampler(rate);
    std::vector<std::int16_t> split;
    std::size_t offset = 0, serial = 0;
    while (offset < input.size()) {
      const auto count = std::min(input.size() - offset, ++serial % 17 == 0 ? std::size_t(1) : std::size_t(256));
      const auto block = resampler.Process(input.data()+offset, count);
      split.insert(split.end(), block.begin(), block.end()); offset += count;
    }
    const auto tail = resampler.Flush(); split.insert(split.end(), tail.begin(), tail.end());
    CHECK(split == whole); CHECK(resampler.Flush().empty());
    resampler.Reset();
    auto again = resampler.Process(input.data(), input.size());
    const auto end = resampler.Flush(); again.insert(again.end(), end.begin(), end.end());
    CHECK(again == whole);
  }
}

void DownsamplingRejectsAliasedHighFrequencyAudio() {
  auto rms = [](int hz) {
    std::vector<std::int16_t> input(48000);
    for (int i = 0; i < 48000; ++i) input[i] = static_cast<std::int16_t>(12000 * std::sin(2 * 3.141592653589793 * hz * i / 48000));
    const auto output = ResampleTo16k(input.data(), input.size(), 48000);
    double energy = 0;
    for (std::size_t i = 100; i < output.size()-100; ++i) energy += static_cast<double>(output[i])*output[i];
    return std::sqrt(energy / (output.size()-200));
  };
  CHECK(rms(1000) > 8000);
  CHECK(rms(12000) < 200);
}
