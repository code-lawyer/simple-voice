// Explicit deployment check, never part of CTest: activates the registered
// production DLL and leaves its real engine running, without starting capture.
#include "tsf_service.hpp"
#include "ipc.hpp"
#include "pipe_io.hpp"
#include <msctf.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace {
template<class T> struct ComPtr {
  T* value=nullptr;
  ~ComPtr() { if (value) value->Release(); }
};
struct Handle {
  HANDLE value=INVALID_HANDLE_VALUE;
  ~Handle() { if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void Hr(HRESULT result,const char* operation) {
  if (FAILED(result)) {
    std::fprintf(stderr,"%s HRESULT=%08lx\n",operation,static_cast<unsigned long>(result));
    throw std::runtime_error(operation);
  }
}
std::string Utf8(const std::wstring& text) {
  const auto size=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
  std::string result(size,'\0');
  if (size) WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr);
  return result;
}
void Pump() {
  MSG message{};
  while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
struct ActiveThread {
  ITfThreadMgrEx* manager;
  ~ActiveThread() { manager->Deactivate(); }
};
struct ActiveService {
  ITfTextInputProcessor* service;
  ~ActiveService() { service->Deactivate(); }
};
int Check(const std::filesystem::path& expected) {
  ComPtr<ITfInputProcessorProfiles> profiles;
  Hr(CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles,reinterpret_cast<void**>(&profiles.value)),"create profile manager");
  BOOL enabled=FALSE;
  Hr(profiles.value->IsEnabledLanguageProfile(CLSID_VoiceIme,0x0804,GUID_VoiceImeProfile,&enabled),"read registered profile");
  if (!enabled) throw std::runtime_error("VoiceIme language profile is disabled");
  std::puts("registered_profile_enabled=1");

  // CoCreateInstance resolves COM registration; do not LoadLibrary by path.
  ComPtr<ITfTextInputProcessor> service;
  Hr(CoCreateInstance(CLSID_VoiceIme,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfTextInputProcessor,reinterpret_cast<void**>(&service.value)),"create registered VoiceIme service");
  wchar_t loaded[32768]{};
  const auto module=GetModuleHandleW(L"voiceime_tsf.dll");
  if (!module || !GetModuleFileNameW(module,loaded,32768)) throw std::runtime_error("locate loaded VoiceIme DLL");
  std::printf("loaded_tsf=%s\n",Utf8(loaded).c_str());
  if (!std::filesystem::equivalent(loaded,expected/L"voiceime_tsf.dll")) throw std::runtime_error("registered COM loaded a different DLL");

  ComPtr<ITfThreadMgrEx> manager;
  Hr(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfThreadMgrEx,reinterpret_cast<void**>(&manager.value)),"create real TSF thread manager");
  TfClientId client=0;
  Hr(manager.value->ActivateEx(&client,TF_TMAE_NOACTIVATETIP|TF_TMAE_NOACTIVATEKEYBOARDLAYOUT|TF_TMAE_SECUREMODE),"activate isolated TSF host");
  ActiveThread active_thread{manager.value};
  Hr(service.value->Activate(manager.value,client),"activate registered service");
  ActiveService active_service{service.value};

  Handle pipe;
  const auto deadline=GetTickCount64()+30000;
  do {
    Pump();
    pipe.value=CreateFileW(L"\\\\.\\pipe\\VoiceIme",GENERIC_READ|GENERIC_WRITE,0,nullptr,
        OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (pipe.value!=INVALID_HANDLE_VALUE) break;
    Sleep(50);
  } while (GetTickCount64()<deadline);
  if (pipe.value==INVALID_HANDLE_VALUE) throw std::runtime_error("production engine was not ready within 30 seconds");
  using QueryServer=BOOL(WINAPI*)(HANDLE,PULONG);
  const auto query=reinterpret_cast<QueryServer>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetNamedPipeServerProcessId"));
  ULONG pid=0;
  if (!query || !query(pipe.value,&pid)) throw std::runtime_error("read production pipe server identity");
  Handle engine{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)};
  wchar_t executable[32768]{}; DWORD capacity=32768;
  if (!engine.value || !QueryFullProcessImageNameW(engine.value,0,executable,&capacity)) throw std::runtime_error("read running engine path");
  std::printf("engine_pid=%lu\nengine_path=%s\n",pid,Utf8(executable).c_str());
  if (!std::filesystem::equivalent(executable,expected/L"voiceime_engine.exe")) throw std::runtime_error("production pipe belongs to a different engine");

  Handle timer{CreateWaitableTimerW(nullptr,TRUE,nullptr)};
  LARGE_INTEGER due{}; due.QuadPart=-100000000;
  if (!timer.value || !SetWaitableTimer(timer.value,&due,0,nullptr,nullptr,FALSE)) throw std::runtime_error("create response deadline");
  std::string buffer;
  for (;;) {
    Message message; std::size_t used=0;
    const auto decoded=DecodeFrame(buffer,&message,&used);
    if (decoded==DecodeStatus::Invalid) throw std::runtime_error("production engine returned an invalid frame");
    if (decoded==DecodeStatus::Complete) {
      buffer.erase(0,used);
      if (message.type==MsgType::Phase) {
        std::printf("production_pipe_ready=1 phase=%s\n",message.text.c_str());
        if (message.text!="0") throw std::runtime_error("engine is not idle; check an existing voice session");
        break;
      }
      continue;
    }
    const auto read=ReadPipeChunk(pipe.value,nullptr,timer.value,50);
    buffer+=read.bytes;
    if (read.status!=PipeReadStatus::Data) throw std::runtime_error("production engine phase response timed out");
    Pump();
  }
  std::puts("PASS installed COM module and real production engine match the requested build; microphone was not started");
  return 0;
}
}

int main(int argc,char** argv) {
  if (argc!=2) return 2;
  const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  if (FAILED(initialized)) return 2;
  int result=1;
  try { result=Check(std::filesystem::absolute(argv[1])); }
  catch (const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); }
  CoUninitialize();
  return result;
}
