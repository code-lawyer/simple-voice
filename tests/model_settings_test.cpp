#include "harness.hpp"
#include "model_settings.hpp"
#include "utf8.hpp"
#include <filesystem>
#include <fstream>
#include <windows.h>

void ModelChoicesPersistAndRejectInvalidSettings() {
  ModelSettings settings;settings.model=SpeechModel::Whisper;
  settings.directories[0]=L"D:\\本地 模型\\qwen";
  settings.directories[2]=L"D:\\模型=目录\\Whisper";
  settings.hotwords="轻音,Simple Voice";settings.idle_minutes=10;
  ModelSettings parsed;
  CHECK(ParseModelSettings(SerializeModelSettings(settings),parsed));
  CHECK(parsed.model==SpeechModel::Whisper && parsed.directories==settings.directories);
  CHECK(parsed.hotwords==settings.hotwords && parsed.idle_minutes==10);
  const auto original=SerializeModelSettings(parsed);
  for(const auto& bad:{std::string(""),original+"model=qwen3\n",original+"extra=x\n",
      std::string("version=1\nmodel=arbitrary\n"),original.substr(0,original.size()-5)}) {
    CHECK(!ParseModelSettings(bad,parsed));
    CHECK(SerializeModelSettings(parsed)==original);
  }
  settings.hotwords="bad\nmodel=whisper";CHECK(SerializeModelSettings(settings).empty());
  settings.hotwords="";settings.idle_minutes=-1;CHECK(SerializeModelSettings(settings).empty());
}
void ModelSettingsSaveFailureKeepsWorkingChoice() {
  const auto path=L"build/model-settings-test.ini";
  ModelSettings old;CHECK(SaveModelSettings(path,old));
  HANDLE held=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
  CHECK(held!=INVALID_HANDLE_VALUE);
  ModelSettings next;next.model=SpeechModel::SenseVoice;
  CHECK(!SaveModelSettings(path,next));
  CHECK(LoadModelSettings(path).model==SpeechModel::Qwen3);
  CloseHandle(held);CHECK(SaveModelSettings(path,next));
  CHECK(LoadModelSettings(path).model==SpeechModel::SenseVoice);
  DeleteFileW(path);
  CHECK(LoadModelSettings(path,true).model==SpeechModel::ChineseLegacy);
}
void ModelChangesHaveExplicitRuntimeBoundaries() {
  ModelSettings a,b;CHECK(SameModelRuntime(a,b));
  b.idle_minutes=0;CHECK(SameModelRuntime(a,b));
  b.directories[2]=L"D:\\other";CHECK(SameModelRuntime(a,b));
  b.directories[0]=L"D:\\qwen";CHECK(!SameModelRuntime(a,b));
  b=a;b.hotwords="词语";CHECK(!SameModelRuntime(a,b));
  a.model=b.model=SpeechModel::SenseVoice;CHECK(SameModelRuntime(a,b));
  ModelFiles files;std::string error;
  CHECK(!ResolveModelFiles("build/does-not-contain-speech-models",a,files,error));
  CHECK(!error.empty());
}
