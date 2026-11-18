#include "ipc.hpp"
#include "pipe_io.hpp"
#include "ipc_security.hpp"
#include "settings.hpp"
#include "preferences_controls.hpp"
#include "model_settings.hpp"
#include "sqlite3.h"
#include "lexicon.hpp"
#include "data_backup.hpp"
#include "utf8.hpp"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
bool WaitWindowClosed(HWND window) {
  const auto deadline=GetTickCount64()+1000;
  while(IsWindow(window) && GetTickCount64()<deadline) Sleep(10);
  return !IsWindow(window);
}
void Require(bool ok, const char* name) {
  if (!ok) throw std::runtime_error(name);
  ++checks;
  std::printf("PASS %s\n", name);
}
void ShowModelPage(HWND settings) {
  const bool compact=IsWindowVisible(preference::Field(settings,preference::CompactNavigation));
  const int id=compact?preference::CompactNavigation:preference::Navigation;
  const auto navigation=preference::Field(settings,id);
  const auto selection=SendMessageW(navigation,compact?CB_SETCURSEL:LB_SETCURSEL,4,0);
  SendMessageW(settings,WM_COMMAND,MAKEWPARAM(id,compact?CBN_SELCHANGE:LBN_SELCHANGE),reinterpret_cast<LPARAM>(navigation));
  const auto model=preference::Field(settings,preference::ModelChoice);
  if(!IsWindowVisible(model)) {
    std::fprintf(stderr,"Model page visibility: frame=%d pane=%d nav=%d selection=%lld choice=%d own-style=%lx\n",
                 IsWindowVisible(settings),IsWindowVisible(preference::Field(settings,preference::Content)),
                 IsWindowVisible(navigation),static_cast<long long>(selection),IsWindowVisible(model),
                 static_cast<unsigned long>(GetWindowLongPtrW(model,GWL_STYLE)));
  }
  Require(IsWindowVisible(model),"model actions run on the visible model page");
}
struct Timer {
  HANDLE value = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  explicit Timer(int ms = 5000) {
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(ms) * 10000;
    SetWaitableTimer(value, &due, 0, nullptr, nullptr, FALSE);
  }
  ~Timer() { CloseHandle(value); }
};
class Client {
 public:
  explicit Client(const std::wstring& name, bool authenticate = true) {
    const auto deadline = GetTickCount64() + 5000;
    do {
      pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe_ != INVALID_HANDLE_VALUE) break;
      Sleep(20);
    } while (GetTickCount64() < deadline);
    Require(pipe_ != INVALID_HANDLE_VALUE, "isolated engine accepts pipe client");
    if (authenticate) {
      WNDCLASSW window_class{};
      window_class.lpfnWndProc = DefWindowProcW;
      window_class.hInstance = GetModuleHandleW(nullptr);
      window_class.lpszClassName = L"VoiceImeTsfHost";
      RegisterClassW(&window_class);
      window_ = CreateWindowW(window_class.lpszClassName, L"", WS_POPUP, 0,0,0,0,
                              nullptr,nullptr,window_class.hInstance,nullptr);
      const auto server = PipeServerPeer(pipe_);
      Require(server && server->is_image(ExecutableDirectory()+L"\\voiceime_engine_testhost.exe"),
              "fixture verifies the engine process identity");
      server_process_ = server->id();
      Message initial;
      Require(ClientHandshake(pipe_,window_,nullptr,initial,buffer_),"client window handshake completes");
      state_id_ = initial.state_id;
      messages_.push_back(initial);
      DecodeAvailable();
    }
  }
  ~Client() { if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_); if(window_)DestroyWindow(window_); }
  HWND Window() const { return window_; }
  DWORD Server() const { return server_process_; }
  std::uint64_t State() const { return state_id_; }
  void SendAt(MsgType type, std::uint64_t state) { Message m; m.type=type; m.state_id=state; Raw(Encode(m)); }
  void Send(MsgType type, std::string text = "", bool fragment = false, std::uint64_t id = 0) {
    Message msg;
    msg.type = type;
    msg.text = std::move(text);
    msg.request_id = id ? id : (type == MsgType::InsertResult ? last_commit_ : 0);
    if (IsVoiceControl(type)) msg.state_id = state_id_;
    const auto bytes = Encode(msg);
    Timer timeout;
    if (fragment) {
      Require(WritePipeBytes(pipe_, bytes.substr(0, 2), timeout.value), "fragmented request begins");
      Sleep(1100);
      Require(WritePipeBytes(pipe_, bytes.substr(2), timeout.value), "fragmented request completes after polling timeout");
    } else Require(WritePipeBytes(pipe_, bytes, timeout.value), "request sent");
  }
  Message Wait(MsgType type, const char* text = nullptr) {
    Timer timeout;
    for (;;) {
      for (auto it = messages_.begin(); it != messages_.end(); ++it) {
        if (it->type == type && (text == nullptr || it->text == text)) {
          Message found = *it;
          messages_.erase(it);
          if (found.type == MsgType::InsertText) last_commit_ = found.request_id;
          return found;
        }
      }
      const auto result = ReadPipeChunk(pipe_, nullptr, timeout.value, 50);
      buffer_ += result.bytes;
      DecodeAvailable();
      if (result.status != PipeReadStatus::Data) throw std::runtime_error("engine response timeout/disconnect");
    }
  }
  void Phase(const char* phase) { Wait(MsgType::Phase, phase); }
  bool HasCommit() const {
    return std::any_of(messages_.begin(), messages_.end(),
                       [](const Message& m) { return m.type == MsgType::InsertText; });
  }
  bool Quiet(int ms) {
    Timer timeout(ms);
    for (;;) {
      const auto result = ReadPipeChunk(pipe_, nullptr, timeout.value, 20);
      buffer_ += result.bytes;
      DecodeAvailable();
      if (result.status != PipeReadStatus::Data) return !HasCommit();
    }
  }
  bool StaysListening(int ms) {
    Quiet(ms);
    return !HasCommit() && std::none_of(messages_.begin(),messages_.end(),[](const Message& message) {
      return message.type==MsgType::Phase && message.text!="1";
    });
  }
  void Raw(const std::string& bytes) {
    Timer timeout; Require(WritePipeBytes(pipe_, bytes, timeout.value), "raw frame sent to isolated engine");
  }
  bool Closed(int milliseconds = 1500) {
    Timer timeout(milliseconds);
    for (;;) {
      const auto result = ReadPipeChunk(pipe_, nullptr, timeout.value, 20);
      if (result.status == PipeReadStatus::Closed) return true;
      if (result.status != PipeReadStatus::Data) return false;
    }
  }
 private:
  std::uint64_t last_commit_ = 0;
  void DecodeAvailable() {
    for (;;) {
      Message msg;
      std::size_t used = 0;
      if (!Decode(buffer_, &msg, &used)) return;
      buffer_.erase(0, used);
      if (msg.type == MsgType::Phase) state_id_ = msg.state_id;
      messages_.push_back(std::move(msg));
    }
  }
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HWND window_ = nullptr;
  DWORD server_process_ = 0;
  std::uint64_t state_id_ = 0;
  std::string buffer_;
  std::deque<Message> messages_;
};
struct WindowSearch { DWORD pid; const wchar_t* name=L"VoiceImeConfirm"; HWND window = nullptr; };
BOOL CALLBACK FindEngine(HWND hwnd, LPARAM arg) {
  auto* search = reinterpret_cast<WindowSearch*>(arg);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  wchar_t name[80]{};
  GetClassNameW(hwnd, name, 80);
  if (pid == search->pid && std::wstring(name) == search->name) search->window = hwnd;
  return TRUE;
}
class Host {
 public:
  Host(const std::filesystem::path& executable, const std::filesystem::path& data, const wchar_t* fixture,
       const wchar_t* transcript=nullptr,const wchar_t* download_program=nullptr) {
    static int serial = 0;
    id_ = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++serial);
    std::filesystem::create_directories(data);
    SetEnvironmentVariableW(L"VOICEIME_TEST_ID", id_.c_str());
    SetEnvironmentVariableW(L"VOICEIME_TEST_DATA", data.c_str());
    SetEnvironmentVariableW(L"VOICEIME_TEST_CAPTURE", fixture);
    SetEnvironmentVariableW(L"VOICEIME_TEST_TRANSCRIPT", transcript);
    SetEnvironmentVariableW(L"VOICEIME_TEST_DOWNLOAD_PROGRAM",download_program);
    SetEnvironmentVariableW(L"VOICEIME_TEST_FOREGROUND",std::to_wstring(GetCurrentProcessId()).c_str());
    std::wstring command = L"\"" + executable.wstring() + L"\" --fake";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    const BOOL created = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_);
    SetEnvironmentVariableW(L"VOICEIME_TEST_ID", nullptr);
    SetEnvironmentVariableW(L"VOICEIME_TEST_DATA", nullptr);
    SetEnvironmentVariableW(L"VOICEIME_TEST_CAPTURE", nullptr);
    SetEnvironmentVariableW(L"VOICEIME_TEST_TRANSCRIPT", nullptr);
    SetEnvironmentVariableW(L"VOICEIME_TEST_DOWNLOAD_PROGRAM",nullptr);
    SetEnvironmentVariableW(L"VOICEIME_TEST_FOREGROUND", nullptr);
    Require(created != FALSE, "isolated test engine starts");
    CloseHandle(process_.hThread);
    process_.hThread = nullptr;
    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job_) {
      SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
      AssignProcessToJobObject(job_, process_.hProcess);
    }
  }
  ~Host() {
    if (process_.hProcess && WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
      PostMessageW(Window(), WM_CLOSE, 0, 0);
      // Fallback only targets the isolated child created above.
      if (WaitForSingleObject(process_.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(process_.hProcess, 99);
    }
    if (job_) CloseHandle(job_);
    if (process_.hProcess) CloseHandle(process_.hProcess);
  }
  std::wstring Pipe() const { return L"\\\\.\\pipe\\VoiceImeQA_" + id_; }
  HWND Window(const wchar_t* name=L"VoiceImeConfirm") const {
    WindowSearch search{process_.dwProcessId,name};
    EnumWindows(FindEngine, reinterpret_cast<LPARAM>(&search));
    return search.window;
  }
  DWORD Handles() const { DWORD count=0; GetProcessHandleCount(process_.hProcess,&count); return count; }
  void Foreground(DWORD process) { SendMessageW(Window(),WM_APP+100,process,0); }
  void Stop() {
    Require(PostMessageW(Window(), WM_CLOSE, 0, 0) != FALSE, "engine exit requested");
    Require(WaitForSingleObject(process_.hProcess, 5000) == WAIT_OBJECT_0, "engine joins pending IO on exit");
    DWORD code = 99;
    GetExitCodeProcess(process_.hProcess, &code);
    Require(code == 0, "engine exits normally");
  }
 private:
  PROCESS_INFORMATION process_{};
  HANDLE job_ = nullptr;
  std::wstring id_;
};

void ModelIdlePreference(const std::filesystem::path& host,const std::filesystem::path& output) {
  const auto data=output/("model-idle-"+std::to_string(GetTickCount64()));
  const auto file=(data/"speech-models.ini").wstring();
  constexpr WPARAM idle_timeout=5*60*1000;
  auto open_model_page=[](const Host& engine) {
    SendMessageW(engine.Window(),WM_COMMAND,3,0);
    const auto settings=engine.Window(L"SimpleVoicePreferences");
    Require(settings!=nullptr,"native preferences open for the idle-release test");
    ShowModelPage(settings);
    return settings;
  };
  {
    Host engine(host,data,L"normal"); Client client(engine.Pipe()); client.Phase("0");
    auto settings=open_model_page(engine);
    const auto idle=preference::Field(settings,preference::ModelIdle);
    Require(SendMessageW(idle,BM_GETCHECK,0,0)==BST_CHECKED,"idle release starts enabled by default");
    const auto choice=preference::Field(settings,preference::ModelChoice);
    SendMessageW(choice,CB_SETCURSEL,static_cast<int>(SpeechModel::SenseVoice),0);
    SendMessageW(idle,BM_CLICK,0,0);
    Require(SendMessageW(idle,BM_GETCHECK,0,0)==BST_UNCHECKED,"clicking the native idle checkbox turns it off");
    SendMessageW(settings,WM_CLOSE,0,0);
    settings=open_model_page(engine);
    Require(SendMessageW(preference::Field(settings,preference::ModelIdle),BM_GETCHECK,0,0)==BST_UNCHECKED,
            "idle release stays off after closing and reopening preferences without activating a model");
    const auto saved=LoadModelSettings(file);
    Require(saved.idle_minutes==0 && saved.model==SpeechModel::Qwen3,
            "turning off idle release saves the preference without applying the browsed model");
    SendMessageW(engine.Window(),WM_APP+102,idle_timeout,0);
    client.Send(MsgType::StartListen);
    Require(client.Wait(MsgType::Phase).text=="1","with idle release off, recording starts without preparing the model again");
    client.Send(MsgType::Cancel);client.Phase("0");engine.Stop();
  }
  {
    Host engine(host,data,L"normal"); Client client(engine.Pipe()); client.Phase("0");
    const auto settings=open_model_page(engine);
    const auto idle=preference::Field(settings,preference::ModelIdle);
    Require(SendMessageW(idle,BM_GETCHECK,0,0)==BST_UNCHECKED,"idle release stays off after restarting the engine");
    HANDLE held=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    Require(held!=INVALID_HANDLE_VALUE,"fixture prevents replacement of the saved model settings");
    SendMessageW(idle,BM_CLICK,0,0);
    CloseHandle(held);
    wchar_t feedback[128]{};
    SendMessageW(preference::Field(settings,preference::StatusLine),WM_GETTEXT,128,reinterpret_cast<LPARAM>(feedback));
    Require(SendMessageW(idle,BM_GETCHECK,0,0)==BST_UNCHECKED && LoadModelSettings(file).idle_minutes==0 &&
            std::wstring(feedback).find(L"保存失败")!=std::wstring::npos,
            "a failed save restores the checkbox and reports the failure");
    SendMessageW(idle,BM_CLICK,0,0);
    Require(LoadModelSettings(file).idle_minutes==5,"re-enabling idle release restores the five-minute timeout");
    SendMessageW(engine.Window(),WM_APP+102,idle_timeout,0);
    client.Send(MsgType::StartListen);
    Require(client.Wait(MsgType::Phase).text=="3","with idle release on, five idle minutes trigger preparation on the next recording");
    client.Phase("1");client.Send(MsgType::Cancel);client.Phase("0");engine.Stop();
  }
  {
    Host engine(host,data,L"normal"); Client client(engine.Pipe()); client.Phase("0");
    client.Send(MsgType::StartListen);client.Phase("1");
    const auto settings=open_model_page(engine);
    SendMessageW(preference::Field(settings,preference::ModelChoice),CB_SETCURSEL,static_cast<int>(SpeechModel::SenseVoice),0);
    SendMessageW(preference::Field(settings,preference::ModelUse),BM_CLICK,0,0);
    Require(IsWindowEnabled(preference::Field(settings,preference::ModelCancel)),"model change is queued during recording");
    SendMessageW(preference::Field(settings,preference::ModelIdle),BM_CLICK,0,0);
    const auto saved=LoadModelSettings(file);
    Require(saved.model==SpeechModel::Qwen3 && saved.idle_minutes==0,"idle release can be saved independently of a queued model change");
    client.Send(MsgType::Cancel);client.Phase("0");
    client.Send(MsgType::StartListen);client.Phase("1");
    const auto activated=LoadModelSettings(file);
    Require(activated.model==SpeechModel::SenseVoice && activated.idle_minutes==0,
            "completing the queued model change retains the last saved idle preference");
    client.Send(MsgType::Cancel);client.Phase("0");engine.Stop();
  }
}

void DownloadFixture(const std::filesystem::path& program,bool complete) {
  std::filesystem::create_directories(program/"tools");
  std::ofstream script(program/"tools/download_models.ps1");
  script<<R"PS(param([string]$Root,[string]$OnlyModel,[string]$StatusFile)
$ErrorActionPreference='Stop'
[IO.File]::AppendAllText((Join-Path $Root 'download-count.txt'),'1')
$gate=Join-Path $Root 'allow-download-completion'
$wait=[Diagnostics.Stopwatch]::StartNew()
while(!(Test-Path -LiteralPath $gate)) {
  if($wait.ElapsedMilliseconds -ge 8000) { throw 'Timed out waiting for the test to release the download.' }
  Start-Sleep -Milliseconds 20
}
)PS";
  if(complete)script<<R"PS($files=@('streaming-bilingual/encoder-epoch-99-avg-1.int8.onnx',
'streaming-bilingual/decoder-epoch-99-avg-1.onnx','streaming-bilingual/joiner-epoch-99-avg-1.int8.onnx',
'streaming-bilingual/tokens.txt','final-qwen/conv_frontend.onnx','final-qwen/encoder.int8.onnx',
'final-qwen/decoder.int8.onnx','final-qwen/tokenizer/vocab.json','final-qwen/tokenizer/merges.txt',
'final-qwen/tokenizer/tokenizer_config.json')
foreach($file in $files) {
  $path=Join-Path (Join-Path $Root 'models') $file
  [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
  [IO.File]::WriteAllText($path,'test fixture only')
}
)PS";
  script<<"[IO.File]::WriteAllText($StatusFile,'fixture completed')\nexit 0\n";
}

void IndependentClient(const std::filesystem::path& executable, const std::wstring& pipe,
                       const wchar_t* mode, std::uintptr_t window = 0) {
  const auto command_line = L"\"" + executable.wstring() + L"\" --ipc-child \"" + pipe + L"\" " +
      mode + L" " + std::to_wstring(window);
  auto command = command_line;
  STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESHOWWINDOW; startup.wShowWindow=SW_HIDE;
  PROCESS_INFORMATION child{};
  Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
      nullptr,nullptr,&startup,&child)!=FALSE,"independent IPC regression client starts");
  CloseHandle(child.hThread);
  const auto wait=WaitForSingleObject(child.hProcess,7000);
  if(wait!=WAIT_OBJECT_0)TerminateProcess(child.hProcess,99);
  DWORD code=99; GetExitCodeProcess(child.hProcess,&code); CloseHandle(child.hProcess);
  Require(wait==WAIT_OBJECT_0 && code==0,"independent IPC regression client meets its authorization expectation");
}

int ChildClient(const char* pipe_text, const char* mode_text, const char* window_text) {
  const std::string name(pipe_text),mode(mode_text);
  Require(name.rfind("\\\\.\\pipe\\VoiceImeQA_",0)==0,"child refuses production endpoints");
  const std::wstring pipe(name.begin(),name.end());
  if(mode=="claim" || mode=="background") {
    Client client(pipe); client.Phase("3");
    if(mode=="background") {
      client.Send(MsgType::StartListen); client.Send(MsgType::Enter); client.Send(MsgType::Cancel);
      Require(client.Quiet(200),"verified background process receives no pending text");
    } else {
      WindowSearch search{client.Server()}; EnumWindows(FindEngine,reinterpret_cast<LPARAM>(&search));
      Require(search.window!=nullptr,"find isolated engine foreground fixture");
      SendMessageW(search.window,WM_APP+100,GetCurrentProcessId(),0);
      client.Send(MsgType::Enter);
      Require(!client.Wait(MsgType::InsertText).text.empty(),"verified new foreground process may confirm after switching applications");
      client.Send(MsgType::InsertResult,"ok"); client.Phase("0");
    }
  } else {
    Client client(pipe,false);
    Message attempt;
    attempt.state_id=1;
    if(mode=="hello") { attempt.type=MsgType::ClientHello; attempt.client_window=std::stoull(window_text); }
    else attempt.type=mode=="enter"?MsgType::Enter:mode=="cancel"?MsgType::Cancel:MsgType::StartListen;
    client.Raw(Encode(attempt));
    Require(client.Closed(),"unauthenticated or borrowed-window client is disconnected");
  }
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if(argc==5 && std::string(argv[1])=="--ipc-child") return ChildClient(argv[2],argv[3],argv[4]);
    wchar_t own[MAX_PATH]{};
    GetModuleFileNameW(nullptr, own, MAX_PATH);
    const auto host = std::filesystem::path(own).parent_path() / "voiceime_engine_testhost.exe";
    const auto output = std::filesystem::absolute(argc > 1 ? argv[1] : "build/engine-integration");
    if(argc<3 || std::string(argv[2])=="--model-idle-only") {
      ModelIdlePreference(host,output);
      if(argc>=3) {std::printf("%d idle-release checks passed\n",checks);return 0;}
    }
    if(argc<3 || std::string(argv[2])=="--manual-stop-only") {
      for(const auto* fixture:{L"speech-end",L"soft-speech",L"long-speech",L"silence"}) {
        const auto data=output/fixture;
        std::filesystem::create_directories(data);
        InputPreferences old;old.pause_ms=500;
        Require(SaveInputPreferences((data/"preferences.ini").wstring(),old),"legacy pause setting is retained for compatibility");
        Host engine(host,data,fixture);Client client(engine.Pipe());client.Phase("0");
        client.Send(MsgType::StartListen);client.Phase("1");
        const int wait=std::wstring(fixture)==L"speech-end" || std::wstring(fixture)==L"soft-speech"?1250:350;
        Require(client.StaysListening(wait),"silence, quiet speech and more than 60 seconds never finalize before manual stop");
        client.Send(MsgType::Enter);client.Phase("2");client.Phase("3");
        Require(client.Quiet(80),"manual stop only prepares a preview; it never inserts text");
        wchar_t preview[2048]{};GetWindowTextW(engine.Window(),preview,2048);
        client.Send(MsgType::Enter);const auto final=client.Wait(MsgType::InsertText).text;
        Require(std::wstring(preview).find(Utf8ToWide(final))!=std::wstring::npos,"second confirmation inserts exactly the calibrated preview");
        client.Phase("4");client.Send(MsgType::InsertResult,"ok");client.Phase("0");engine.Stop();
      }
      {
        Host engine(host,output/"recording-limit",L"recording-limit");
        Client client(engine.Pipe());client.Phase("0");client.Send(MsgType::StartListen);client.Phase("1");
        const auto listening=client.State();client.Phase("5");
        Require(client.Quiet(150),"10-minute limit stops capture without calibration or insertion");
        wchar_t stopped[2048]{};GetWindowTextW(engine.Window(),stopped,2048);
        Require(std::wstring(stopped).find(L"10 分钟上限")!=std::wstring::npos &&
                std::wstring(stopped).find(L"9600000假字")!=std::wstring::npos,
                "limit notice preserves the entire bounded recording and the draft");
        client.SendAt(MsgType::Enter,listening);client.Phase("2");client.Phase("3");
        Require(client.Quiet(80),"a stop racing with the limit calibrates but cannot insert");
        client.SendAt(MsgType::Enter,listening);client.Phase("3");
        Require(client.Quiet(80),"a stale stop cannot confirm a completed preview");
        client.Send(MsgType::Cancel);client.Phase("0");
        client.Send(MsgType::StartListen);client.Phase("5");
        client.Send(MsgType::StartListen);client.Phase("2");client.Phase("3");
        Require(client.Quiet(80),"the voice hotkey also calibrates a stopped recording without starting over");
        client.Send(MsgType::Cancel);client.Phase("0");
        client.Send(MsgType::StartListen);client.Phase("5");
        client.Send(MsgType::Cancel);client.Phase("0");
        Require(client.Quiet(80),"a recording stopped at the limit can be discarded without calibration");
        engine.Stop();
      }
      {
        Host engine(host,output/"practice-limit",L"recording-limit");
        Client client(engine.Pipe());client.Phase("0");
        SendMessageW(engine.Window(),WM_COMMAND,3,0);const auto settings=engine.Window(L"SimpleVoicePreferences");
        Require(preference::Field(settings,52)==nullptr,"obsolete automatic-pause selector is removed");
        SendMessageW(settings,WM_COMMAND,72,0);client.Phase("5");
        Require(client.Quiet(80),"practice at the limit waits for an explicit calibration request");
        SendMessageW(settings,WM_COMMAND,72,0);client.Phase("2");client.Phase("3");
        Require(client.Quiet(80),"practice calibration never writes into the current application");
        SendMessageW(settings,WM_COMMAND,73,0);client.Phase("0");engine.Stop();
      }
      if(argc>=3) {std::printf("%d manual-stop checks passed\n",checks);return 0;}
    }
    if(argc<3 || (std::string(argv[2])!="--practice-only" && std::string(argv[2])!="--review-only")) {
    {
      Host engine(host,output/"authorization",L"manual");
      Client origin(engine.Pipe()); origin.Phase("0");
      for(const auto* mode:{L"start",L"enter",L"cancel",L"hello"})
        IndependentClient(own,engine.Pipe(),mode,reinterpret_cast<std::uintptr_t>(origin.Window()));
      Require(origin.Quiet(80),"unknown clients cannot activate capture or obtain text");
      { Client silent(engine.Pipe(),false); Require(silent.Closed(4500),"silent handshake expires without retaining a worker"); }
      origin.Send(MsgType::StartListen); origin.Send(MsgType::Cancel); origin.Phase("0");
      Require(origin.Quiet(50),"Esc immediately after Start works before the phase round trip");
      origin.Send(MsgType::StartListen); origin.Send(MsgType::Enter); origin.Phase("3");
      Require(origin.Quiet(50),"Enter immediately after Start only stops and previews");
      origin.Send(MsgType::Cancel); origin.Phase("0");
      origin.Send(MsgType::StartListen); origin.Send(MsgType::StartListen); origin.Phase("3");
      Require(origin.Quiet(50),"a quick repeated hotkey stops without confirming a preview");
      origin.Send(MsgType::Cancel); origin.Phase("0");
      origin.Send(MsgType::StartListen); origin.Phase("1");
      const auto before_final=origin.State();
      origin.Send(MsgType::Enter); origin.Phase("3");
      origin.SendAt(MsgType::Cancel,before_final); origin.Phase("0");
      Require(origin.Quiet(50),"Esc can cancel the same utterance across a finalization race");
      origin.Send(MsgType::StartListen); origin.Phase("1");
      const auto listening=origin.State();
      origin.Send(MsgType::Enter); origin.Phase("3");
      origin.SendAt(MsgType::Enter,listening); origin.Phase("3");
      Require(origin.Quiet(80),"an Enter from the listening state cannot confirm a later preview");
      IndependentClient(own,engine.Pipe(),L"background");
      wchar_t title[2048]{}; GetWindowTextW(engine.Window(),title,2048);
      Require(std::wstring(title).find(L"假字。")!=std::wstring::npos,"background commands leave the pending preview intact");
      IndependentClient(own,engine.Pipe(),L"claim"); origin.Phase("0");
      Require(origin.Quiet(80),"foreground switch delivers text only to the confirming client");
      engine.Foreground(GetCurrentProcessId());
      origin.Send(MsgType::StartListen); origin.Phase("1");
      origin.SendAt(MsgType::Cancel,listening); origin.Phase("1");
      Require(origin.Quiet(50),"a stale cancel cannot erase a new utterance");
      origin.Send(MsgType::Cancel); origin.Phase("0");
      for (int transition=0; transition<8; ++transition) {
        origin.Send(MsgType::StartListen);
        { Client joining(engine.Pipe()); joining.Phase("1"); }
        origin.Phase("1"); origin.Send(MsgType::Enter);
        { Client joining(engine.Pipe()); joining.Phase("3"); }
        origin.Phase("3"); origin.Send(MsgType::Cancel); origin.Phase("0");
      }
      engine.Stop();
    }
    int text_case=0;
    for(const auto* recognized:{L"今天处理20个文件。",L"今天使用Codex处理文件。",L"把理论用于实践。",L"张山到了。"}) {
      const auto data=output/("text-preservation-"+std::to_string(++text_case));
      std::filesystem::create_directories(data);
      {
        Lexicon lex((data/"lexicon.sqlite").string());
        for(const auto* saved:{"今天处理10个文件。","今天使用Claude处理文件。","时间","张杉"})
          lex.SaveTerm(saved,"test",std::time(nullptr));
      }
      Host engine(host,data,L"manual",recognized); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("3");
      client.Send(MsgType::Enter);
      Require(client.Wait(MsgType::InsertText).text==WideToUtf8(recognized),
              "saved sentences, digits, English names and homophones never rewrite the confirmed transcript");
      client.Send(MsgType::InsertResult,"ok"); client.Phase("0"); engine.Stop();
    }
    {
      Host engine(host,output/"prepare-fast-cancel",L"model-slow");
      Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Send(MsgType::Cancel); client.Phase("0");
      Require(client.Quiet(650),"fast cancellation during model preparation never opens capture later");
      engine.Stop();
    }
    for(bool disconnect:{false,true}) {
      Host engine(host,output/(disconnect?"prepare-disconnect":"prepare-focus"),L"model-slow");
      {
        Client client(engine.Pipe()); client.Phase("0");
        client.Send(MsgType::StartListen); client.Phase("3");
        if(!disconnect) {
          engine.Foreground(0); client.Phase("0");
          Require(client.Quiet(100),"losing foreground while preparing never starts capture");
        }
      }
      Sleep(650); engine.Foreground(GetCurrentProcessId());
      Client observer(engine.Pipe()); observer.Phase("0");
      Require(observer.Quiet(50),"prepared model cannot resurrect a disconnected or unfocused request");
      engine.Stop();
    }
    {
      const auto data=output/"lexicon-failure";
      std::filesystem::create_directories(data);
      sqlite3* db=nullptr;
      Require(sqlite3_open((data/"lexicon.sqlite").string().c_str(),&db)==SQLITE_OK,"fault database opens");
      const char* sql="CREATE TABLE IF NOT EXISTS entries(text TEXT PRIMARY KEY,pinyin TEXT NOT NULL,count INTEGER NOT NULL,last_commit_unix INTEGER NOT NULL,kind TEXT NOT NULL);"
          "CREATE TABLE IF NOT EXISTS personal_terms(text TEXT PRIMARY KEY,pinyin TEXT NOT NULL,count INTEGER NOT NULL,last_commit_unix INTEGER NOT NULL,kind TEXT NOT NULL); INSERT OR IGNORE INTO personal_terms VALUES('假字','jiazi',0,strftime('%s','now'),'user'); CREATE TRIGGER IF NOT EXISTS reject_learning BEFORE UPDATE ON personal_terms BEGIN SELECT RAISE(ABORT,'injected write failure'); END;";
      const auto code=sqlite3_exec(db,sql,nullptr,nullptr,nullptr);
      sqlite3_close(db);
      Require(code==SQLITE_OK,"learning failure is injected into isolated data");
      Host engine(host,data,L"manual"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("3");
      client.Send(MsgType::Enter); client.Wait(MsgType::InsertText); client.Phase("4");
      client.Send(MsgType::InsertResult,"ok"); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Cancel); client.Phase("0");
      Require(client.Quiet(100),"learning failure does not kill the engine or replay inserted text");
      engine.Stop();
    }
    {
      Host engine(host, output / "correlation", L"manual");
      Client first(engine.Pipe()); first.Phase("0");
      Client other(engine.Pipe()); other.Phase("0");
      first.Send(MsgType::StartListen); first.Phase("1");
      first.Send(MsgType::StartListen); first.Phase("3");
      first.Send(MsgType::Enter);
      const auto request = first.Wait(MsgType::InsertText); first.Phase("4");
      Require(request.request_id != 0, "insert carries a nonzero correlation ID");
      first.Send(MsgType::StartListen); first.Send(MsgType::Cancel);
      other.Send(MsgType::InsertResult,"fail",false,request.request_id);
      first.Send(MsgType::InsertResult,"fail",false,request.request_id+1);
      first.Quiet(120);
      wchar_t title[2048]{}; GetWindowTextW(engine.Window(),title,2048);
      Require(std::wstring(title).find(L"正在写入正文") != std::wstring::npos,
              "unconfirmed insert blocks restart and ignores wrong client or ID");
      first.Send(MsgType::InsertResult,"ok",false,request.request_id); first.Phase("0");
      first.Send(MsgType::StartListen); first.Phase("1");
      first.Send(MsgType::InsertResult,"fail",false,request.request_id);
      first.Quiet(120); GetWindowTextW(engine.Window(),title,2048);
      Require(std::wstring(title).find(L"正在听") != std::wstring::npos,
              "late duplicate failure cannot overwrite a new recording");
      first.Send(MsgType::Cancel); first.Phase("0");
      engine.Stop();
    }
    {
      Host engine(host, output / "disconnect-insert", L"manual");
      {
        Client client(engine.Pipe()); client.Phase("0");
        client.Send(MsgType::StartListen); client.Phase("1");
        client.Send(MsgType::Enter); client.Phase("3");
        client.Send(MsgType::Enter); client.Wait(MsgType::InsertText); client.Phase("4");
      }
      Sleep(250);
      Client reconnected(engine.Pipe()); reconnected.Phase("3");
      wchar_t title[2048]{}; GetWindowTextW(engine.Window(),title,2048);
      Require(std::wstring(title).find(L"写入结果待核实") != std::wstring::npos,
              "disconnect preserves text and asks user to check the document");
      Require(reconnected.Quiet(100), "reconnect never replays an uncertain insert automatically");
      reconnected.Send(MsgType::Cancel); reconnected.Phase("0");
      engine.Stop();
    }
    {
      Host engine(host, output / "timeout-insert", L"manual");
      Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("3");
      client.Send(MsgType::Enter);
      const auto request = client.Wait(MsgType::InsertText); client.Phase("4");
      Sleep(5200); client.Phase("3");
      Require(client.Wait(MsgType::CancelInsert).request_id == request.request_id,
              "confirmation timeout cancels queued insert by ID");
      SendMessageW(engine.Window(),WM_COMMAND,5,0);
      const auto editor=engine.Window(L"SimpleVoicePreviewEditor");
      Require(editor!=nullptr,"uncertain insertion remains editable");
      wchar_t notice[2048]{}; GetDlgItemTextW(editor,11,notice,2048);
      Require(std::wstring(notice).find(L"待核实")!=std::wstring::npos,"editor explains that saving cannot confirm a previous insert");
      SendMessageW(editor,WM_COMMAND,IDOK,0); client.Phase("3");
      Require(WaitWindowClosed(editor),"saving preview completes before it is reopened");
      GetWindowTextW(engine.Window(),notice,2048);
      Require(std::wstring(notice).find(L"写入结果待核实")!=std::wstring::npos,"saving unchanged preview retains uncertain insertion warning");
      SendMessageW(engine.Window(),WM_COMMAND,5,0);
      const auto edited=engine.Window(L"SimpleVoicePreviewEditor");
      Require(edited!=nullptr,"uncertain preview can be reopened");
      // Exercise the native EDIT buffer, not its cross-process window caption.
      SendDlgItemMessageW(edited,10,EM_SETSEL,0,-1);
      SendDlgItemMessageW(edited,10,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"手工修订文本"));
      SendMessageW(edited,WM_COMMAND,IDOK,0); client.Phase("3");
      Require(WaitWindowClosed(edited),"saving corrected preview closes its editor");
      GetWindowTextW(engine.Window(),notice,2048);
      Require(std::wstring(notice).find(L"待核实")!=std::wstring::npos && std::wstring(notice).find(L"手工修订文本")!=std::wstring::npos,"editing preserves both text and uncertainty");
      Require(client.Quiet(100),"saving an uncertain preview never replays insertion");
      client.Send(MsgType::Enter);
      Require(client.Wait(MsgType::InsertText).text=="手工修订文本","explicit retry inserts exactly the corrected preview");
      client.Phase("4"); client.Send(MsgType::InsertResult,"ok"); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::InsertResult,"fail",false,request.request_id);
      client.Quiet(100);
      wchar_t title[2048]{}; GetWindowTextW(engine.Window(),title,2048);
      Require(std::wstring(title).find(L"正在听") != std::wstring::npos,
              "expired result cannot alter a restarted session");
      client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    {
      Host engine(host, output / "invalid-and-churn", L"manual");
      Client anchor(engine.Pipe()); anchor.Phase("0"); Sleep(250);
      const auto baseline = engine.Handles();
      for (int batch=0; batch<2; ++batch) {
        for (int i=0; i<60; ++i) { Client transient(engine.Pipe()); transient.Phase("0"); }
        Sleep(500);
        const auto handles=engine.Handles();
        std::printf("churn batch=%d baseline=%lu current=%lu\n",batch+1,baseline,handles);
        Require(handles <= baseline+6, "completed connection workers release handles while engine stays alive");
      }
      {
        Client malformed(engine.Pipe()); malformed.Phase("0");
        malformed.Raw(std::string("\xfc\xff\xff\xff\x09",5));
        Require(malformed.Closed(), "overflow frame closes only the invalid connection");
      }
      anchor.Send(MsgType::StartListen); anchor.Phase("1");
      anchor.Send(MsgType::Cancel); anchor.Phase("0");
      Require(anchor.Quiet(100), "valid client remains usable after malformed peer disconnects");
      engine.Stop();
    }
    {
      Host engine(host, output / "manual", L"manual");
      Client first(engine.Pipe());
      first.Phase("0");
      Sleep(1100);
      first.Send(MsgType::StartListen, "", true);
      first.Phase("1");
      Require(first.Quiet(100), "listening never commits a draft");
      first.Send(MsgType::StartListen);
      first.Phase("3");
      Require(!first.HasCommit(), "pending final requires confirmation");
      first.Send(MsgType::Enter);
      const auto final = first.Wait(MsgType::InsertText).text;
      Require(final.find("。") != std::string::npos, "Enter emits the final text");
      first.Phase("4");
      first.Send(MsgType::InsertResult, "fail");
      first.Phase("3");
      first.Send(MsgType::Enter);
      Require(first.Wait(MsgType::InsertText).text == final, "rejected insert retains the full final for retry");
      first.Phase("4");
      first.Send(MsgType::InsertResult, "ok");
      first.Phase("0");
      first.Send(MsgType::StartListen);
      first.Phase("1");
      first.Send(MsgType::Cancel);
      first.Phase("0");
      Require(first.Quiet(100), "Esc discards without commit");
      first.Send(MsgType::StartListen);
      first.Phase("1");
      first.Send(MsgType::Enter);
      first.Phase("3");
      Require(first.Quiet(120), "first Enter stops recording and previews without inserting");
      wchar_t preview[2048]{}; GetWindowTextW(engine.Window(),preview,2048);
      Require(std::wstring(preview).find(L"假字。")!=std::wstring::npos,"punctuated final is visible in the confirmation popup before insertion");
      first.Send(MsgType::Enter);
      Require(!first.Wait(MsgType::InsertText).text.empty(), "second Enter confirms the previewed final");
      first.Phase("4");
      first.Send(MsgType::InsertResult, "ok");
      first.Phase("0");
      first.Send(MsgType::StartListen);
      first.Phase("1");
      first.Send(MsgType::StartListen);
      first.Phase("3");
      first.Send(MsgType::StartListen);
      first.Phase("1");
      Require(first.Quiet(100), "hotkey discards pending final and starts a fresh recording");
      first.Send(MsgType::Cancel);
      first.Phase("0");
      {
        Client second(engine.Pipe());
        second.Phase("0");
        first.Send(MsgType::StartListen);
        first.Phase("1");
        second.Phase("1");
        first.Send(MsgType::StartListen);
        first.Phase("3");
        second.Phase("3");
        second.Send(MsgType::Enter);
        Require(!second.Wait(MsgType::InsertText).text.empty(), "second client confirms into its current document");
        first.Phase("4");
        Require(first.Quiet(100), "commit is not broadcast to the other document");
        second.Send(MsgType::InsertResult, "ok");
        second.Phase("0");
      }
      {
        Client reconnected(engine.Pipe());
        reconnected.Phase("0");
        reconnected.Send(MsgType::StartListen);
        reconnected.Phase("1");
        reconnected.Send(MsgType::Cancel);
        reconnected.Phase("0");
        Require(reconnected.Quiet(100), "client disconnect and reconnect preserve cancellation");
      }
      engine.Stop();
    }
    {
      Host engine(host, output / "error", L"error");
      Client client(engine.Pipe());
      client.Phase("0");
      client.Send(MsgType::StartListen);
      client.Phase("3");
      wchar_t title[512]{};
      GetWindowTextW(engine.Window(), title, 512);
      Require(std::wstring(title).find(L"麦克风") != std::wstring::npos, "capture failure gives a visible diagnostic");
      client.Send(MsgType::Enter);
      Require(client.Quiet(150), "capture error cannot commit diagnostic text");
      client.Send(MsgType::Cancel);
      client.Phase("0");
      client.Send(MsgType::StartListen);
      client.Phase("3");
      client.Send(MsgType::Cancel);
      client.Phase("0");
      Require(client.Quiet(100), "capture failure can be cancelled and retried");
      engine.Stop();
    }
    {
      Host engine(host, output / "slow-preview", L"slow-final");
      Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("2");
      client.Send(MsgType::Enter); client.Phase("3");
      Require(client.Quiet(120),"Enter during calibration cannot queue an automatic insert");
      client.Send(MsgType::Cancel); client.Phase("0");
      Require(client.Quiet(100),"calibrated preview can still be cancelled without document changes"); engine.Stop();
    }
    {
      const auto data=output/"appearance";
      std::filesystem::create_directories(data);
      Require(SaveSkin((data/"appearance.json").wstring(),UiSkin::Titanium),"appearance fixture starts at Titanium");
      Require(SaveSettings((data/"config.json").string(),Settings{Hotkey{1,VK_F8}}),"custom hotkey fixture persists");
      Host engine(host,data,L"slow-final"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("3");
      wchar_t before[2048]{}; GetWindowTextW(engine.Window(),before,2048);
      for (auto skin : {UiSkin::DeepSpace,UiSkin::Daylight,UiSkin::Titanium}) {
        SendMessageW(engine.Window(),WM_COMMAND,30+static_cast<UINT>(skin),0);
        Require(LoadSkin((data/"appearance.json").wstring())==skin,"native skin command persists selected appearance");
        Require(LoadSettings((data/"config.json").string()).hotkey.vk==VK_F8,"skin command preserves the user's hotkey");
        wchar_t after[2048]{}; GetWindowTextW(engine.Window(),after,2048);
        Require(std::wstring(before)==after && client.Quiet(80),"switching skin retains the calibrated preview without inserting text");
      }
      HANDLE held=CreateFileW((data/"appearance.json").c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
      Require(held!=INVALID_HANDLE_VALUE,"appearance file replacement can be fault-injected");
      SendMessageW(engine.Window(),WM_COMMAND,32,0); CloseHandle(held);
      Require(LoadSkin((data/"appearance.json").wstring())==UiSkin::Titanium,"failed skin save retains the previous selection");
      client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    {
      const auto data=output/"model-selection-race";
      std::filesystem::create_directories(data);
      InputPreferences initial; initial.guide_dismissed=true; Require(SaveInputPreferences((data/"preferences.ini").wstring(),initial),"model profile fixture starts bilingual");
      Require(SaveModelSettings((data/"speech-models.ini").wstring(),{}),"model profile fixture resets the selected model");
      Host engine(host,data,L"slow-final"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);
      const auto settings=engine.Window(L"SimpleVoicePreferences"); Require(settings!=nullptr,"native preferences open during recording");
      ShowModelPage(settings);
      SendMessageW(preference::Field(settings,60),CB_SETCURSEL,1,0); SendMessageW(settings,WM_COMMAND,63,0);
      Require(LoadModelSettings((data/"speech-models.ini").wstring()).model==SpeechModel::Qwen3,"recording keeps the old model until the replacement is validated");
      client.Send(MsgType::StartListen); client.Phase("2"); client.Phase("3");
      wchar_t text[2048]{}; GetWindowTextW(engine.Window(),text,2048);
      Require(std::wstring(text).find(L"假字。")!=std::wstring::npos,"stop hotkey finishes the current model without discarding the draft");
      client.Send(MsgType::Cancel); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("2");
      const auto began=GetTickCount64(); client.Send(MsgType::Cancel); client.Phase("0");
      Require(GetTickCount64()-began<200,"cancel responds before slow final inference returns");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("3"); Require(client.Quiet(100),"late cancelled result cannot insert or replace the next preview");
      client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    {
      Host engine(host,output/"model-preparing",L"model-slow"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("3");
      wchar_t title[1024]{}; GetWindowTextW(engine.Window(),title,1024);
      Require(std::wstring(title).find(L"准备")!=std::wstring::npos,"model preparation is visible before microphone capture");
      const auto began=GetTickCount64(); client.Send(MsgType::Cancel); client.Phase("0");
      Require(GetTickCount64()-began<200,"model loading does not block cancellation");
      Require(client.Quiet(650),"cancelled model preparation never commits");
      GetWindowTextW(engine.Window(),title,1024);
      Require(!IsWindowVisible(engine.Window()),"late model readiness does not restart a cancelled recording");
      client.Send(MsgType::StartListen); client.Phase("1"); client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    {
      Host engine(host,output/"model-retry",L"model-fail-once"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("3"); Require(client.Quiet(650),"failed model preparation cannot insert");
      wchar_t title[1024]{}; GetWindowTextW(engine.Window(),title,1024);
      Require(std::wstring(title).find(L"未能准备")!=std::wstring::npos,"model loading failure gives retry guidance");
      client.Send(MsgType::Enter); Require(client.Quiet(80),"model error cannot be confirmed as text");
      client.Send(MsgType::StartListen); client.Phase("1");
      client.Send(MsgType::Enter); client.Phase("3"); Require(client.Quiet(100),"retry succeeds without automatic insertion");
      client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    {
      const auto data=output/"preview-generation";
      std::filesystem::create_directories(data);
      InputPreferences initial; initial.guide_dismissed=true;
      Require(SaveInputPreferences((data/"preferences.ini").wstring(),initial),"editor generation fixture starts bilingual");
      Require(SaveModelSettings((data/"speech-models.ini").wstring(),{}),"preview generation resets the selected model");
      Host engine(host,data,L"model-slow"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("3"); client.Phase("1"); Sleep(40);
      client.Send(MsgType::Enter); client.Phase("2"); client.Phase("3");
      SendMessageW(engine.Window(),WM_COMMAND,5,0);
      const auto editor=engine.Window(L"SimpleVoicePreviewEditor");
      Require(editor!=nullptr,"previous utterance has an editable preview");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);
      const auto settings=engine.Window(L"SimpleVoicePreferences");
      ShowModelPage(settings);
      SendMessageW(preference::Field(settings,60),CB_SETCURSEL,1,0); SendMessageW(settings,WM_COMMAND,63,0);
      client.Send(MsgType::StartListen); client.Phase("3");
      Require(!IsWindow(editor),"accepting a queued recording closes the previous utterance editor");
      SendMessageW(engine.Window(),WM_COMMAND,5,0);
      Require(!engine.Window(L"SimpleVoicePreviewEditor"),"model preparation cannot open an editor for an absent preview");
      client.Send(MsgType::Enter);
      Require(client.Quiet(100),"Enter while preparing cannot insert the previous utterance");
      client.Phase("1"); client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    {
      Host engine(host,output/"model-reset-failure",L"reset-error");Client client(engine.Pipe());client.Phase("0");
      client.Send(MsgType::StartListen);client.Phase("3");
      wchar_t message[2048]{};GetWindowTextW(engine.Window(),message,2048);
      Require(std::wstring(message).find(L"模型未能开始")!=std::wstring::npos,"model reset failure gives retry guidance without killing the engine");
      client.Send(MsgType::Enter);Require(client.Quiet(80),"model reset error is never inserted as user text");
      client.Send(MsgType::Cancel);client.Phase("0");engine.Stop();
    }
    }
    if(argc<3 || std::string(argv[2])!="--practice-only") {
    for(const auto* fixture:{L"reset-hang",L"audio-hang"}) {
      Host engine(host,output/fixture,fixture);Client client(engine.Pipe());client.Phase("0");
      client.Send(MsgType::StartListen);client.Phase("1");Sleep(80);
      if(std::wstring(fixture)==L"audio-hang") {
        const auto began=GetTickCount64();client.Send(MsgType::Enter);client.Phase("2");
        Require(GetTickCount64()-began<250,"stopping a blocked audio request immediately exposes cancellable finalization");
      }
      const auto began=GetTickCount64();client.Send(MsgType::Cancel);client.Phase("0");
      Require(GetTickCount64()-began<250,"blocked reset or audio cannot hold the UI cancellation path");
      engine.Stop();
    }
    for(bool complete:{true,false}) {
      const auto data=output/(std::string(complete?"download-complete-":"download-incomplete-")+std::to_string(GetTickCount64()));
      const auto program=data/"fixture-program";DownloadFixture(program,complete);
      Host engine(host,data,L"normal",nullptr,program.c_str());Client client(engine.Pipe());client.Phase("0");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);const auto settings=engine.Window(L"SimpleVoicePreferences");
      Require(settings!=nullptr,"download request opens the real preferences flow");
      ShowModelPage(settings);
      Require(SendMessageW(preference::Field(settings,69),WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"审查专名"))!=0,"download request accepts explicit name hints");
      wchar_t hints[128]{};SendMessageW(preference::Field(settings,69),WM_GETTEXT,128,reinterpret_cast<LPARAM>(hints));
      Require(std::wstring(hints)==L"审查专名","download fixture reads back the actual edit control contents");
      SendMessageW(settings,WM_COMMAND,63,0);
      Require(!IsWindowEnabled(preference::Field(settings,63)),"model activation is disabled while its request is in flight");
      const auto model_settings=(data/"speech-models.ini").wstring();
      if(complete) {
        SendMessageW(preference::Field(settings,preference::ModelIdle),BM_CLICK,0,0);
        Require(LoadModelSettings(model_settings).idle_minutes==0,"idle preference reaches disk while download is still in flight");
      }
      // Synthetic WM_COMMAND intentionally bypasses disabled controls to verify
      // the engine independently rejects competing activation requests.
      SendMessageW(preference::Field(settings,60),CB_SETCURSEL,1,0);SendMessageW(settings,WM_COMMAND,63,0);
      std::ofstream gate(data/"allow-download-completion");
      Require(gate.is_open(),"fixture releases the download after the competing request");
      gate.close();
      const auto deadline=GetTickCount64()+8000;
      while(!IsWindowEnabled(preference::Field(settings,63)) && GetTickCount64()<deadline) {Sleep(20);SendMessageW(settings,WM_TIMER,1,0);}
      Require(IsWindowEnabled(preference::Field(settings,63)),"download validation completes without leaving a busy settings page");
      const auto selected=LoadModelSettings(model_settings);
      if(complete) {
        Require(selected.model==SpeechModel::Qwen3 && selected.idle_minutes==0 && selected.hotwords=="审查专名",
            "automatic download keeps the latest idle preference and requested hints, and rejects a competing activation");
        Require(!selected.directories[0].empty(),"downloaded preset resolves from the isolated data directory");
      } else {
        Sleep(700);std::ifstream count(data/"download-count.txt");std::string starts;std::getline(count,starts);
        Require(starts=="1","incomplete shared model files report failure instead of an automatic download loop");
        Require(!std::filesystem::exists(data/"speech-models.ini"),"incomplete downloads leave the prior model selection untouched");
      }
      engine.Stop();
    }
    {
      const auto data=output/("restore-model-choice-"+std::to_string(GetTickCount64()));
      const auto source=data/"backup-source";std::filesystem::create_directories(source);
      InputPreferences legacy;legacy.chinese_only=true;
      Require(SaveInputPreferences((source/"preferences.ini").wstring(),legacy),"legacy backup contains its original input preferences");
      std::filesystem::path backup;
      {Lexicon terms((source/"lexicon.sqlite").string());backup=CreateDataBackup(terms,source.wstring(),data.wstring());}
      std::filesystem::create_directories(data/"restore-fixture");
      std::filesystem::copy_file(backup/"SimpleVoice.backup",data/"restore-fixture/SimpleVoice.backup");
      Host engine(host,data,L"normal");Client client(engine.Pipe());client.Phase("0");
      Require(SendMessageW(engine.Window(),WM_APP+101,0,0)!=0,"engine restores legacy settings through the production restore action");
      Require(LoadInputPreferences((data/"preferences.ini").wstring()).chinese_only,"legacy preference bytes were actually restored");
      Require(LoadModelSettings((data/"speech-models.ini").wstring(),true).model==SpeechModel::Qwen3,
          "restore pins the current model so a restart cannot import the backup's legacy model preference");
      engine.Stop();
    }
    }
    {
      Host engine(host,output/"first-use-practice",L"slow-final");Client client(engine.Pipe());client.Phase("0");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);
      const auto settings=engine.Window(L"SimpleVoicePreferences");Require(settings!=nullptr,"first-use practice is available in native preferences");
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("1");Sleep(45);
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("2");client.Phase("3");
      SendMessageW(settings,WM_TIMER,1,0);wchar_t preview[2048]{};SendMessageW(preference::Field(settings,75),WM_GETTEXT,2048,reinterpret_cast<LPARAM>(preview));
      Require(std::wstring(preview).find(L"假字。")!=std::wstring::npos,"practice displays calibrated text from the production capture path");
      SendMessageW(settings,WM_COMMAND,73,0);client.Phase("0");
      Require(client.Quiet(80),"practice confirmation never inserts into the external client");
      SendMessageW(preference::Field(settings,75),WM_GETTEXT,2048,reinterpret_cast<LPARAM>(preview));Require(std::wstring(preview).find(L"已确认")!=std::wstring::npos,"practice retains its own confirmed result");
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("1");
      SendMessageW(settings,WM_CLOSE,0,0);client.Phase("0");
      Require(client.Quiet(80),"closing first-use practice cancels capture without inserting");engine.Stop();
    }
    {
      const auto data=output/("practice-model-choice-"+std::to_string(GetTickCount64()));
      Host engine(host,data,L"normal");Client client(engine.Pipe());client.Phase("0");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);const auto settings=engine.Window(L"SimpleVoicePreferences");
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("1");Sleep(40);
      SendMessageW(preference::Field(settings,60),CB_SETCURSEL,1,0);SendMessageW(settings,WM_COMMAND,63,0);
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("2");client.Phase("3");
      SendMessageW(settings,WM_COMMAND,73,0);client.Phase("0");
      SendMessageW(settings,WM_COMMAND,72,0);
      const auto deadline=GetTickCount64()+3000;
      while(LoadModelSettings((data/"speech-models.ini").wstring()).model!=SpeechModel::SenseVoice && GetTickCount64()<deadline)Sleep(10);
      Require(LoadModelSettings((data/"speech-models.ini").wstring()).model==SpeechModel::SenseVoice,
          "the next practice validates the deferred model before opening capture");
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("1");SendMessageW(settings,WM_COMMAND,74,0);client.Phase("0");engine.Stop();
    }
    {
      Host engine(host,output/"practice-unknown",L"normal",L"这是<unk>");Client client(engine.Pipe());client.Phase("0");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);const auto settings=engine.Window(L"SimpleVoicePreferences");
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("1");Sleep(40);
      SendMessageW(settings,WM_COMMAND,72,0);client.Phase("2");client.Phase("3");
      SendMessageW(settings,WM_COMMAND,73,0);SendMessageW(settings,WM_TIMER,1,0);
      wchar_t text[2048]{};SendMessageW(preference::Field(settings,75),WM_GETTEXT,2048,reinterpret_cast<LPARAM>(text));
      Require(std::wstring(text).find(L"已确认")==std::wstring::npos && std::wstring(text).find(L"<unk>")!=std::wstring::npos,
          "practice does not acknowledge unknown speech as a successful final preview");
      SendMessageW(settings,WM_COMMAND,74,0);client.Phase("0");engine.Stop();
    }
    std::printf("%d engine integration checks passed (isolated pipe, synthetic capture)\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
