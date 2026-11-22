#include "harness.hpp"
#include "worker_recognizer.hpp"
#include "recording.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <thread>
#include <cstdio>
#include <vector>
namespace fs=std::filesystem;
namespace {
std::string root;
std::wstring worker;
void Fixture(const char* value) {SetEnvironmentVariableA("VOICEIME_MODEL_WORKER_FIXTURE",value);}
std::unique_ptr<Recognizer> Start(const char* fixture,unsigned timeout=3000) {
  Fixture(fixture);std::string error;
  auto result=MakeWorkerRecognizer(root,ModelSettings{},&error,worker,timeout);
  Fixture(nullptr);return result;
}
void RealPrivatePipesRoundTrip() {
  auto r=Start("normal");CHECK(r && r->Healthy());if(!r)return;
  r->Reset();const std::int16_t audio[320]={1234};r->AcceptAudio(audio,320);
  CHECK(r->Partial()=="测试草稿");CHECK(r->Finalize()=="测试最终文本");
  CHECK(r->Original()=="测试最终文本");r->Reset();CHECK(r->Partial().empty());
}
void DraftWorksWhileFinalModelLoads() {
  Fixture("staged");std::string error;
  auto r=MakeWorkerRecognizer(root,{},&error,worker,3000,nullptr,ModelStartup::DraftFirst);
  Fixture(nullptr);
  CHECK(r && r->Healthy());if(!r)return;
  CHECK(!r->FinalReady());
  const std::int16_t input[320]={1200};r->Reset();
  CHECK(r->AcceptAudio(input,320) && r->Partial()=="测试草稿");
  CHECK(!r->AcceptAudio(input,320) && r->Partial()=="测试草稿");
  CHECK(!r->FinalReady());
  CHECK(r->Finalize()=="测试最终文本");
  CHECK(r->FinalReady());r->Reset();CHECK(r->Partial().empty());
}
void LongReplyCrossesThePipeBuffer() {
  auto r = Start("large-final");
  CHECK(r && r->Healthy());
  if (!r) return;
  const std::string expected(128 * 1024, 'x');
  CHECK(r->Finalize() == expected);
  CHECK(r->Original() == expected);
  r->Reset();
  CHECK(r->Partial().empty() && r->Healthy());
}
void LongRecordingBudgetIsEnforcedAcrossWorkerRequests() {
  auto r=Start("normal");CHECK(r && r->Healthy());if(!r)return;
  const std::vector<std::int16_t> minute(16000*62,250);
  r->AcceptAudio(minute.data(),minute.size());CHECK(r->Healthy());
  CHECK(r->Partial()=="测试草稿");r->Reset();
  const std::vector<std::int16_t> audio(kRecordingSampleRate,250);
  for(int second=0;second<kMaxRecordingSeconds && r->Healthy();++second)
    r->AcceptAudio(audio.data(),audio.size());
  CHECK(r->Healthy());if(!r->Healthy())return;
  CHECK(r->Finalize()=="测试最终文本");
  const std::int16_t extra=250;r->AcceptAudio(&extra,1);
  CHECK(!r->Healthy());CHECK(!r->TakeWarning().empty());
  CHECK(Start("normal")!=nullptr);
}
void CancelStopsBlockedInferenceAndAllowsAnotherWorker() {
  auto r=Start("final-hang");CHECK(r!=nullptr);if(!r)return;
  auto final=std::async(std::launch::async,[&] {try {r->Finalize();return false;}catch(...){return true;}});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto began=std::chrono::steady_clock::now();r->Cancel();
  CHECK(final.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  CHECK(final.get());CHECK(!r->Healthy());
  CHECK(std::chrono::steady_clock::now()-began<std::chrono::seconds(2));
  auto next=Start("normal");CHECK(next && next->Healthy());
}
void LoadCrashAndTimeoutDoNotKillTheHost() {
  CHECK(!Start("init-crash"));
  const auto began=std::chrono::steady_clock::now();
  CHECK(!Start("init-hang",250));
  CHECK(std::chrono::steady_clock::now()-began<std::chrono::seconds(3));
  CHECK(Start("normal")!=nullptr);
}
void AudioCrashReturnsRecoverableFailure() {
  auto r=Start("audio-crash");CHECK(r!=nullptr);if(!r)return;
  const std::int16_t wave[320]={100};r->AcceptAudio(wave,320);
  CHECK(!r->Healthy());CHECK(!r->TakeWarning().empty());
}
void CachedTextRemainsAvailableDuringBlockedAudio() {
  auto r=Start("audio-hang");CHECK(r!=nullptr);if(!r)return;
  const std::int16_t wave[320]={100};r->AcceptAudio(wave,320);
  CHECK(r->Partial()=="测试草稿");
  auto audio=std::async(std::launch::async,[&]{r->AcceptAudio(wave,320);});
  CHECK(audio.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout);
  // These are read on the UI thread; none may wait for the inference RPC.
  auto cached=std::async(std::launch::async,[&]{
    return r->Partial()=="测试草稿" && r->Original().empty() && r->TakeWarning().empty();
  });
  const auto cache_ready=cached.wait_for(std::chrono::milliseconds(250));
  CHECK(cache_ready==std::future_status::ready);
  if(cache_ready==std::future_status::ready)CHECK(cached.get());
  const auto began=std::chrono::steady_clock::now();r->Cancel();
  CHECK(audio.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  audio.get();
  if(cache_ready!=std::future_status::ready)cached.get();
  CHECK(std::chrono::steady_clock::now()-began<std::chrono::seconds(2));
  CHECK(!r->Healthy());CHECK(r->Partial()=="测试草稿");
}
void LoadErrorReplySurvivesWorkerExitAndLoadIsCancellable() {
  Fixture("init-error");std::string error;
  CHECK(!MakeWorkerRecognizer(root,{},&error,worker,2000));
  CHECK(error=="测试：模型文件不匹配");
  Fixture("init-hang");std::atomic<bool> cancel{false};
  auto result=std::async(std::launch::async,[&]{return MakeWorkerRecognizer(root,{},&error,worker,30000,&cancel);});
  std::this_thread::sleep_for(std::chrono::milliseconds(100));cancel=true;
  CHECK(result.wait_for(std::chrono::seconds(2))==std::future_status::ready);CHECK(!result.get());Fixture(nullptr);
}
}
int main(int argc,char** argv) {
  wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);
  const auto base=fs::path(exe).parent_path();
  worker=(base/L"voiceime_model_worker_testhost.exe").wstring();
  auto directory=base/L"model-worker-fixture";fs::create_directories(directory);
  root=directory.string();
  for(const auto* name:{"streaming-bilingual/encoder-epoch-99-avg-1.int8.onnx",
      "streaming-bilingual/decoder-epoch-99-avg-1.onnx","streaming-bilingual/joiner-epoch-99-avg-1.int8.onnx",
      "streaming-bilingual/tokens.txt","final-qwen/conv_frontend.onnx","final-qwen/encoder.int8.onnx",
      "final-qwen/decoder.int8.onnx","final-qwen/tokenizer/vocab.json","final-qwen/tokenizer/merges.txt",
      "final-qwen/tokenizer/tokenizer_config.json"}) {
    const auto path=directory/name;fs::create_directories(path.parent_path());std::ofstream(path).put('x');
  }
  const Test tests[]={{"worker.PrivatePipesRoundTrip",RealPrivatePipesRoundTrip},
      {"worker.DraftBeforeFinalReady",DraftWorksWhileFinalModelLoads},
      {"worker.LongReplyCrossesThePipeBuffer",LongReplyCrossesThePipeBuffer},
      {"worker.LongRecordingLimit",LongRecordingBudgetIsEnforcedAcrossWorkerRequests},
      {"worker.CancelAndRecover",CancelStopsBlockedInferenceAndAllowsAnotherWorker},
      {"worker.LoadCrashAndTimeout",LoadCrashAndTimeoutDoNotKillTheHost},
      {"worker.AudioCrash",AudioCrashReturnsRecoverableFailure},
      {"worker.LoadErrorAndCancel",LoadErrorReplySurvivesWorkerExitAndLoadIsCancellable},
      {"worker.CachedTextDuringAudio",CachedTextRemainsAvailableDuringBlockedAudio}};
  const int result=RunTests(argc,argv,tests,sizeof(tests)/sizeof(tests[0]));Fixture(nullptr);return result;
}
