#include "harness.hpp"
#include "rime_session.hpp"

#include <cstdint>
#include <filesystem>
#include <atomic>
#include <thread>
#include <string>

namespace {

bool ContainsCjk(const std::string& text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text.data());
  const auto* end = p + text.size();
  while (p < end) {
    uint32_t cp = 0;
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0 && p + 1 < end) {
      cp = (*p & 0x1F) << 6 | (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0 && p + 2 < end) {
      cp = (*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0 && p + 3 < end) {
      cp = (*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
      p += 4;
    } else {
      ++p;
      continue;
    }
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
  }
  return false;
}

std::wstring SharedDataDir() {
  return (std::filesystem::path(VOICEIME_SOURCE_DIR) / "rime-data").wstring();
}

std::wstring TempUserDir() {
  static int n = 0;
  const std::filesystem::path path =
      std::filesystem::path(VOICEIME_SOURCE_DIR) / "build" / ("rime-user-" + std::to_string(++n));
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  std::filesystem::create_directories(path);
  return path.wstring();
}

bool NameHasUserDb(const std::wstring& name) {
  std::string lower;
  lower.reserve(name.size());
  for (wchar_t ch : name) {
    char ascii = ch < 128 ? static_cast<char>(ch) : '?';
    if (ascii >= 'A' && ascii <= 'Z') ascii = static_cast<char>(ascii - 'A' + 'a');
    lower.push_back(ascii);
  }
  return lower.find("voiceime") != std::string::npos && lower.find("userdb") != std::string::npos;
}

bool DirectoryContainsUserDb(const std::wstring& dir) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (NameHasUserDb(entry.path().filename().wstring())) return true;
  }
  return false;
}

void TypeNihao(RimeSession& session) {
  for (int vk : {0x4E, 0x49, 0x48, 0x41, 0x4F}) session.ProcessKey(vk, false);
}

}  // namespace

void NihaoCommitsTheWeightedWord() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  TypeNihao(session);
  CHECK(session.view().composing);
  CHECK(!session.view().page.empty());
  CHECK(session.view().page[0] == "你好");
  RimeView done = session.ProcessKey(0x20, false);
  CHECK(done.commit == "你好");
  CHECK(!done.composing);
  CHECK(session.view().commit == done.commit);
  CHECK(session.view().composing == done.composing);
}
void LongPinyinProducesHanzi() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  const int keys[] = {0x4A, 0x49, 0x4E, 0x54, 0x49, 0x41, 0x4E, 0x54, 0x49, 0x41,
                      0x4E, 0x51, 0x49, 0x42, 0x55, 0x43, 0x55, 0x4F};
  for (int vk : keys) session.ProcessKey(vk, false);
  CHECK(!session.view().page.empty());
  CHECK(ContainsCjk(session.view().page[0]));
}
void PeriodCommitsChinesePunct() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  RimeView done = session.ProcessKey(0xBE, false);
  CHECK(done.commit == "。");
}
void UnknownLettersCommitRawOnSpace() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  for (int i = 0; i < 4; ++i) session.ProcessKey(0x58, false);
  RimeView done = session.ProcessKey(0x20, false);
  CHECK(done.commit == "xxxx");
  CHECK(!ContainsCjk(done.commit));
}
void NumpadSelectsThePageEntry() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  session.ProcessKey(0x4E, false);
  session.ProcessKey(0x49, false);
  CHECK(!session.view().page.empty());
  const std::string first = session.view().page[0];
  RimeView picked = session.ProcessKey(0x61, false);
  CHECK(picked.commit == first);
}
void ShiftOneCommitsFullwidthExclamation() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  CHECK(!session.view().composing);
  RimeView done = session.ProcessKey(0x31, true);
  CHECK(done.commit == "！");
}
void DigitSelectsThePageEntry() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  // nihao's page[1] is the prefix 你. Selecting it leaves hao composing, so
  // librime produces no commit. A finished syllable's entries cover the
  // whole code, and express_editor commits the one digit 2 selects.
  session.ProcessKey(0x4E, false);
  session.ProcessKey(0x49, false);
  CHECK(session.view().page.size() >= 2);
  const std::string second = session.view().page[1];
  RimeView picked = session.ProcessKey(0x32, false);
  CHECK(picked.commit == second);
}
void EscapeDropsCompositionWithoutCommit() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  TypeNihao(session);
  RimeView dropped = session.ProcessKey(0x1B, false);
  CHECK(dropped.commit.empty());
  CHECK(!dropped.composing);
}
void EnterCommitsNihaoOnAFreshUserDir() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  TypeNihao(session);
  RimeView entered = session.ProcessKey(0x0D, false);
  CHECK(entered.commit == "你好");
}
void SelectingACandidateCreatesAUserDb() {
  RimeSession session;
  const std::wstring user = TempUserDir();
  CHECK(session.Start(SharedDataDir(), user));
  TypeNihao(session);
  session.ProcessKey(0x20, false);
  session.Shutdown();
  CHECK(DirectoryContainsUserDb(user));
}

void RimeSessionsSurviveAnotherSessionShuttingDown() {
  const auto user = TempUserDir();
  RimeSession first;
  RimeSession second;
  CHECK(first.Start(SharedDataDir(), user));
  TypeNihao(first);
  CHECK(second.Start(SharedDataDir(), user));
  CHECK(first.ProcessKey(0x20, false).commit == "你好");
  first.Shutdown();
  TypeNihao(second);
  CHECK(second.view().page.size() == 5);
  CHECK(second.ProcessKey(0x20, false).commit == "你好");
  CHECK(first.Start(SharedDataDir(), user));
  second.Shutdown();
  TypeNihao(first);
  CHECK(first.ProcessKey(0x20, false).commit == "你好");
}

void RimeRejectsReconfigurationWhileAnotherSessionIsLive() {
  RimeSession first;
  RimeSession other;
  CHECK(first.Start(SharedDataDir(), TempUserDir()));
  CHECK(!other.Start(SharedDataDir(), TempUserDir()));
  TypeNihao(first);
  CHECK(first.ProcessKey(0x20, false).commit == "你好");
}

void RimeSessionsOnDifferentThreadsRemainIndependent() {
  const auto shared = SharedDataDir();
  const auto user = TempUserDir();
  RimeSession keeper;
  CHECK(keeper.Start(shared, user));
  std::atomic<bool> valid{true};
  auto exercise = [&] {
    for (int n = 0; n < 15; ++n) {
      RimeSession session;
      if (!session.Start(shared, user)) { valid = false; return; }
      TypeNihao(session);
      if (session.ProcessKey(0x20, false).commit != "你好") valid = false;
    }
  };
  std::thread first(exercise);
  std::thread second(exercise);
  first.join(); second.join();
  CHECK(valid.load());
  TypeNihao(keeper);
  CHECK(keeper.ProcessKey(0x20, false).commit == "你好");
}
