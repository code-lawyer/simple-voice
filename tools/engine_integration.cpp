#include "ipc.hpp"
#include "pipe_io.hpp"
#include "settings.hpp"
#include "sqlite3.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <deque>
#include <filesystem>
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
  explicit Client(const std::wstring& name) {
    const auto deadline = GetTickCount64() + 5000;
    do {
      pipe_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      if (pipe_ != INVALID_HANDLE_VALUE) break;
      Sleep(20);
    } while (GetTickCount64() < deadline);
    Require(pipe_ != INVALID_HANDLE_VALUE, "isolated engine accepts pipe client");
  }
  ~Client() { if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_); }
  void Send(MsgType type, std::string text = "", bool fragment = false, std::uint64_t id = 0) {
    Message msg;
    msg.type = type;
    msg.text = std::move(text);
    msg.request_id = id ? id : (type == MsgType::InsertResult ? last_commit_ : 0);
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
  void Raw(const std::string& bytes) {
    Timer timeout; Require(WritePipeBytes(pipe_, bytes, timeout.value), "raw frame sent to isolated engine");
  }
  bool Closed() {
    Timer timeout(1500);
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
      messages_.push_back(std::move(msg));
    }
  }
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
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
  Host(const std::filesystem::path& executable, const std::filesystem::path& data, const wchar_t* fixture) {
    static int serial = 0;
    id_ = std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++serial);
    std::filesystem::create_directories(data);
    SetEnvironmentVariableW(L"VOICEIME_TEST_ID", id_.c_str());
    SetEnvironmentVariableW(L"VOICEIME_TEST_DATA", data.c_str());
    SetEnvironmentVariableW(L"VOICEIME_TEST_CAPTURE", fixture);
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
}  // namespace

int main(int argc, char** argv) {
  try {
    wchar_t own[MAX_PATH]{};
    GetModuleFileNameW(nullptr, own, MAX_PATH);
    const auto host = std::filesystem::path(own).parent_path() / "voiceime_engine_testhost.exe";
    const auto output = std::filesystem::absolute(argc > 1 ? argv[1] : "build/engine-integration");
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
      Host engine(host, output / "silence", L"silence");
      Client client(engine.Pipe());
      client.Phase("0");
      client.Send(MsgType::StartListen);
      client.Phase("3");
      Require(client.Quiet(100), "60 seconds of initial silence reaches pending without committing");
      client.Send(MsgType::Cancel);
      client.Phase("0");
      engine.Stop();
    }
    {
      Host engine(host, output / "automatic-preview", L"speech-end");
      Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("3");
      Require(client.Quiet(120),"speech followed by 500ms silence automatically previews without an Enter or insert");
      wchar_t preview[2048]{}; GetWindowTextW(engine.Window(),preview,2048);
      Require(std::wstring(preview).find(L"假字。")!=std::wstring::npos,"automatic endpoint displays the calibrated final in the popup");
      client.Send(MsgType::Enter); const auto final=client.Wait(MsgType::InsertText).text;
      const int count=MultiByteToWideChar(CP_UTF8,0,final.data(),static_cast<int>(final.size()),nullptr,0);
      std::wstring final_wide(count,L'\0'); MultiByteToWideChar(CP_UTF8,0,final.data(),static_cast<int>(final.size()),final_wide.data(),count);
      Require(std::wstring(preview).find(final_wide)!=std::wstring::npos,"document receives the same final text that was previewed");
      client.Phase("4"); client.Send(MsgType::InsertResult,"ok"); client.Phase("0"); engine.Stop();
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
      Host engine(host,data,L"slow-final"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("1");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);
      const auto settings=engine.Window(L"SimpleVoicePreferences"); Require(settings!=nullptr,"native preferences open during recording");
      SendDlgItemMessageW(settings,57,BM_SETCHECK,BST_CHECKED,0); SendMessageW(settings,WM_COMMAND,23,0);
      Require(LoadInputPreferences((data/"preferences.ini").wstring()).chinese_only,"changed model preference is saved for next utterance");
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
      Host engine(host,data,L"model-slow"); Client client(engine.Pipe()); client.Phase("0");
      client.Send(MsgType::StartListen); client.Phase("3"); client.Phase("1"); Sleep(40);
      client.Send(MsgType::Enter); client.Phase("2"); client.Phase("3");
      SendMessageW(engine.Window(),WM_COMMAND,5,0);
      const auto editor=engine.Window(L"SimpleVoicePreviewEditor");
      Require(editor!=nullptr,"previous utterance has an editable preview");
      SendMessageW(engine.Window(),WM_COMMAND,3,0);
      const auto settings=engine.Window(L"SimpleVoicePreferences");
      SendDlgItemMessageW(settings,57,BM_SETCHECK,BST_CHECKED,0); SendMessageW(settings,WM_COMMAND,23,0);
      client.Send(MsgType::StartListen); client.Phase("3");
      Require(!IsWindow(editor),"accepting a queued recording closes the previous utterance editor");
      SendMessageW(engine.Window(),WM_COMMAND,5,0);
      Require(!engine.Window(L"SimpleVoicePreviewEditor"),"model preparation cannot open an editor for an absent preview");
      client.Send(MsgType::Enter);
      Require(client.Quiet(100),"Enter while preparing cannot insert the previous utterance");
      client.Phase("1"); client.Send(MsgType::Cancel); client.Phase("0"); engine.Stop();
    }
    std::printf("%d engine integration checks passed (isolated pipe, synthetic capture)\n", checks);
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL %s\n", e.what());
    return 1;
  }
}
