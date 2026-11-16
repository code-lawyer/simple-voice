#include "harness.hpp"
#include "data_backup.hpp"
#include "settings.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <ctime>
namespace fs=std::filesystem;
namespace {
fs::path Folder(const wchar_t* name) {
  const auto path=fs::path(L"build-review/backup-tests")/(std::wstring(name)+L"-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
  fs::create_directories(path);return path;
}
bool Fails(const std::function<void()>& run){try{run();return false;}catch(...){return true;}}
}
void BackupRestoresExplicitTermsAndSettings() {
  const auto data=Folder(L"roundtrip");Lexicon lexicon(":memory:");
  lexicon.SaveTerm("轻音输入法","qingyin",std::time(nullptr));
  InputPreferences input;input.learning=false;input.pause_ms=1200;
  CHECK(SaveInputPreferences((data/L"preferences.ini").wstring(),input));
  const auto backup=CreateDataBackup(lexicon,data.wstring(),data.wstring());
  lexicon.SaveTerm("另一个词","lingyige",std::time(nullptr));
  input.learning=true;CHECK(SaveInputPreferences((data/L"preferences.ini").wstring(),input));
  RestoreDataBackup(lexicon,data.wstring(),backup);
  CHECK(lexicon.Count()==1 && lexicon.Search("轻音").size()==1);
  CHECK(!LoadInputPreferences((data/L"preferences.ini").wstring()).learning);
  CHECK(fs::is_directory(data/L"backups"));CHECK(!RecoverDataRestore(lexicon,data.wstring()));
}
void BackupCorruptionAndWriteFailurePreserveData() {
  const auto data=Folder(L"failure");Lexicon lexicon(":memory:");
  lexicon.SaveTerm("旧词语","jiuciyu",std::time(nullptr));
  CHECK(SaveInputPreferences((data/L"preferences.ini").wstring(),{}));
  CHECK(SaveSettings((data/L"config.json").string(),{DefaultHotkey()}));
  const auto backup=CreateDataBackup(lexicon,data.wstring(),data.wstring());
  lexicon.SaveTerm("新词语","xinciyu",std::time(nullptr));
  InputPreferences next;next.learning=false;CHECK(SaveInputPreferences((data/L"preferences.ini").wstring(),next));
  CHECK(SaveSettings((data/L"config.json").string(),{{1,0x74}}));
  HANDLE held=CreateFileW((data/L"config.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  CHECK(held!=INVALID_HANDLE_VALUE);CHECK(Fails([&]{RestoreDataBackup(lexicon,data.wstring(),backup);}));CloseHandle(held);
  CHECK(lexicon.Count()==2);CHECK(!LoadInputPreferences((data/L"preferences.ini").wstring()).learning);
  CHECK(!fs::exists(data/L"restore-recovery.backup"));
  {std::fstream file(fs::path(backup)/L"SimpleVoice.backup",std::ios::in|std::ios::out|std::ios::binary);file.seekp(40);file.put('x');}
  CHECK(Fails([&]{RestoreDataBackup(lexicon,data.wstring(),backup);}));CHECK(lexicon.Count()==2);
}
void InterruptedRestoreRecoversBeforeLoadingPreferences() {
  const auto data=Folder(L"recovery");Lexicon lexicon(":memory:");
  lexicon.SaveTerm("原来的词","yuanlaideci",std::time(nullptr));
  const auto backup=CreateDataBackup(lexicon,data.wstring(),data.wstring());
  fs::copy_file(fs::path(backup)/L"SimpleVoice.backup",data/L"restore-recovery.backup");
  lexicon.ReplaceTerms({});CHECK(SaveSkin((data/L"appearance.json").wstring(),UiSkin::DeepSpace));
  CHECK(RecoverDataRestore(lexicon,data.wstring()));CHECK(lexicon.Count()==1);
  CHECK(!fs::exists(data/L"appearance.json"));CHECK(!RecoverDataRestore(lexicon,data.wstring()));
}
