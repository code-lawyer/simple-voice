#include "harness.hpp"
#include "rime_session.hpp"
#include "settings.hpp"
#include "lexicon.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>
#include <string>
#include <windows.h>
#include <algorithm>
#include <ctime>

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
  const auto path = TestScratchDirectory() / ("rime-user-" + std::to_string(++n));
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

void ChinesePunctuationCommitsFromTheKeyboard() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  struct Example { int vk; bool shift; const char* text; };
  const Example keys[] = {
      {VK_OEM_MINUS,true,"——"}, {'6',true,"……"}, {'4',true,"￥"},
      {VK_OEM_3,false,"·"}, {VK_OEM_3,true,"～"}, {VK_OEM_5,false,"、"},
      {VK_OEM_COMMA,false,"，"}, {VK_OEM_PERIOD,false,"。"},
      {VK_OEM_1,false,"；"}, {VK_OEM_1,true,"："}, {VK_OEM_2,true,"？"}, {'1',true,"！"},
      {'9',true,"（"}, {'0',true,"）"}, {VK_OEM_4,false,"【"}, {VK_OEM_6,false,"】"},
      {VK_OEM_COMMA,true,"《"}, {VK_OEM_PERIOD,true,"》"},
      {VK_OEM_7,true,"“"}, {VK_OEM_7,true,"”"}, {VK_OEM_7,false,"‘"}, {VK_OEM_7,false,"’"},
      {'2',true,"@"}, {'3',true,"#"}, {'5',true,"%"}, {'7',true,"&"}, {'8',true,"*"},
      {VK_OEM_PLUS,true,"+"}, {VK_OEM_2,false,"/"}, {VK_OEM_5,true,"|"}
  };
  for (const auto& key : keys) {
    const auto view = session.ProcessKey(key.vk, key.shift);
    if (view.commit != key.text) std::fprintf(stderr, "vk=%02x shift=%d: expected [%s], got [%s]\n",
                                              key.vk, key.shift, key.text, view.commit.c_str());
    CHECK(view.commit == key.text && !view.composing);
  }
  TypeNihao(session);
  CHECK(session.ProcessKey(VK_OEM_MINUS, true).commit == "你好——");
  CHECK(session.ProcessKey(VK_OEM_MINUS, true).commit == "——");
  session.ProcessKey('N',false);
  session.ProcessKey('I',false);
  const auto first = session.view().page;
  CHECK(!first.empty());
  CHECK(session.ProcessKey(VK_OEM_PLUS,false).page != first);
  CHECK(session.ProcessKey(VK_OEM_MINUS,false).page == first);
}

void PunctuationChoiceSurvivesLearningChanges() {
  const auto root = std::filesystem::path(TempUserDir());
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), (root / "rime").wstring()));
  const auto toggle = [&] { return session.ProcessKeyWithModifiers(VK_OEM_PERIOD, false, true, false); };
  CHECK(toggle().ascii_punctuation);
  InputPreferences preferences;
  preferences.learning = false;
  CHECK(SaveInputPreferences((root / "preferences.ini").wstring(), preferences));
  TypeNihao(session);
  CHECK(session.ProcessKey(VK_OEM_MINUS,true).commit == "你好_");
  CHECK(!toggle().ascii_punctuation);
  CHECK(session.ProcessKey('6',true).commit == "……");
  for (int key : {'X','I'}) session.ProcessKey(key,false);
  session.ProcessKey(VK_OEM_7,false);
  for (int key : {'A','N'}) session.ProcessKey(key,false);
  CHECK(session.ProcessKey(VK_RETURN,false).commit == "xi'an");
}
void UnknownLettersCommitRawOnEnter() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  for (int i = 0; i < 4; ++i) session.ProcessKey(0x58, false);
  RimeView done = session.ProcessKey(0x0D, false);
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
void EnterPreservesRawPinyin() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  TypeNihao(session);
  RimeView entered = session.ProcessKey(0x0D, false);
  CHECK(entered.commit == "nihao");
  const std::vector<std::pair<std::vector<int>,std::string>> edits{
    {{VK_LEFT},"nihao"}, {{VK_HOME},"nihao"},
    {{VK_LEFT,VK_DELETE},"niao"}, {{VK_LEFT,VK_BACK},"nhao"},
    {{VK_LEFT,VK_END},"nihao"}
  };
  for(const auto& [keys,expected]:edits) {
    TypeNihao(session);
    for(int key:keys) session.ProcessKey(key,false);
    const auto done=session.ProcessKey(VK_RETURN,false);
    CHECK(done.commit==expected);
    CHECK(!done.composing && done.preedit.empty());
    CHECK(session.ProcessKey(VK_RETURN,false).commit.empty());
  }
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

void RimeLeavesBusyUserDictionaryUntouched() {
  const auto user = TempUserDir();
  const auto database = std::filesystem::path(user) / "voiceime.userdb";
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), user));
  TypeNihao(session);
  CHECK(session.ProcessKey(VK_SPACE, false).commit == "你好");
  session.Shutdown();
  const auto current_manifest = [&] {
    std::ifstream input(database / "CURRENT");
    std::string name;
    std::getline(input, name);
    return name;
  };
  const auto original = current_manifest();
  CHECK(!original.empty());
  // A running older TSF host holds this handle for the lifetime of its DB.
  const auto owner = CreateFileW((database / "LOCK").c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(owner != INVALID_HANDLE_VALUE);
  if (owner == INVALID_HANDLE_VALUE) return;
  CHECK(LockFile(owner, 0, 0, MAXDWORD, MAXDWORD));
  CHECK(!session.Start(SharedDataDir(), user));
  session.Shutdown();
  CHECK(current_manifest() == original);
  CHECK(CloseHandle(owner));
  CHECK(session.Start(SharedDataDir(), user));
  TypeNihao(session);
  CHECK(session.ProcessKey(VK_SPACE, false).commit == "你好");
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

void ModernInputAndPersonalTermsSurviveLiveSessions() {
  const auto root = TestScratchDirectory() / "rime-quality";
  const auto user=root/"rime"; std::filesystem::create_directories(user);
  const auto personal_table=user/"voiceime_personal.txt";
  InputPreferences preferences; CHECK(SaveInputPreferences((root/"preferences.ini").wstring(),preferences));
  Lexicon terms(":memory:"); terms.SaveTerm("轻音个人术语甲","qysyx",std::time(nullptr));
  terms.SaveTerm("#include","qyinclude",std::time(nullptr));
  terms.SaveTerm("#@metadata","qymetadata",std::time(nullptr));
  terms.SaveTerm("# no comment","qycomment",std::time(nullptr));
  CHECK(terms.ExportTerms((user/"voiceime_personal.txt").wstring()));
  RimeSession first,second; CHECK(first.Start(SharedDataDir(),user.wstring())); CHECK(second.Start(SharedDataDir(),user.wstring()));
  auto type=[](RimeSession& session,const std::string& keys) {
    session.Clear();
    for(unsigned char c:keys) session.ProcessKey(c>='a'&&c<='z'?c-32:c,c>='A'&&c<='Z');
  };
  auto contains=[](const RimeView& view,const std::string& expected) { return std::find(view.page.begin(),view.page.end(),expected)!=view.page.end(); };
  for(const auto& pair:std::vector<std::pair<std::string,std::string>>{
      {"weixin","微信"},{"dayuyanmoxing","大语言模型"},{"rgzn","人工智能"},{"nh","你好"},{"zhognwen","中文"},
      {"python","Python"},{"codex","Codex"},{"GitHub","GitHub"},{"qysyx","轻音个人术语甲"},
      {"qyinclude","#include"},{"qymetadata","#@metadata"},{"qycomment","# no comment"}}) {
    type(first,pair.first); if(!contains(first.view(),pair.second)) std::fprintf(stderr,"missing candidate: %s -> %s\n",pair.first.c_str(),pair.second.c_str());
    CHECK(contains(first.view(),pair.second));
  }
  type(first,"qysyx"); // Keep this generation alive in a composing session.
  const auto original_time=std::filesystem::last_write_time(personal_table);
  const auto original_size=std::filesystem::file_size(personal_table);
  terms.SaveTerm("轻音个人术语乙","qysyx",std::time(nullptr),"轻音个人术语甲");
  CHECK(terms.ExportTerms(personal_table.wstring()));
  // Fast replacement can preserve the observable write timestamp, even for
  // a rename that leaves the generated table exactly the same size.
  std::filesystem::last_write_time(personal_table,original_time);
  CHECK(std::filesystem::file_size(personal_table)==original_size);
  type(second,"qysyx"); CHECK(contains(second.view(),"轻音个人术语乙")); CHECK(!contains(second.view(),"轻音个人术语甲"));
  CHECK(contains(first.view(),"轻音个人术语甲"));
  type(first,"qysyx"); CHECK(contains(first.view(),"轻音个人术语乙"));
  first.Clear(); second.Clear(); preferences.learning=false;
  CHECK(SaveInputPreferences((root/"preferences.ini").wstring(),preferences));
  type(first,"qysyx"); CHECK(contains(first.view(),"轻音个人术语乙"));
  CHECK(first.ProcessKey(VK_SPACE,false).commit=="轻音个人术语乙");
  terms.Delete("轻音个人术语乙"); CHECK(terms.ExportTerms(personal_table.wstring()));
  std::filesystem::last_write_time(personal_table,original_time);
  type(first,"qysyx"); CHECK(!contains(first.view(),"轻音个人术语乙"));
  first.Clear(); type(first,"nihaoo"); first.ProcessKey(VK_LEFT,false); first.ProcessKey(VK_DELETE,false);
  CHECK(first.ProcessKey(VK_RETURN,false).commit=="nihao");
  first.Shutdown(); second.Shutdown(); CHECK(second.Start(SharedDataDir(),user.wstring()));
  type(second,"qyinclude"); CHECK(second.ProcessKey(VK_SPACE,false).commit=="#include");
  type(second,"qysyx"); CHECK(!contains(second.view(),"轻音个人术语乙"));
  type(second,"nihao"); CHECK(second.ProcessKey(VK_SPACE,false).commit=="你好");
}
