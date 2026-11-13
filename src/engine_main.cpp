#include "confirm_box.hpp"
#include "lexicon.hpp"
#include "recognizer.hpp"
#include "resample.hpp"
#include "segmenter.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "sherpa_recognizer.hpp"
#include "vad.hpp"

#include "ipc.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kPipeMessage = WM_APP + 1;
constexpr UINT kPartialMessage = WM_APP + 2;
constexpr UINT kEndpointMessage = WM_APP + 3;
constexpr UINT kFinalMessage = WM_APP + 5;
constexpr UINT kAnnounceMessage = WM_APP + 6;
constexpr UINT kTrayMessage = WM_APP + 4;
constexpr UINT kQuitCommand = 1;
constexpr UINT kRecordCommand = 2;
constexpr UINT kRecentBase = 100;
const GUID kFloatFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  return wide;
}

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return "";
  const int count = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(count > 0 ? count : 0, '\0');
  if (count > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), count, nullptr, nullptr);
  return utf8;
}

bool IsFloatFormat(const WAVEFORMATEX* format) {
  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
    const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return IsEqualGUID(extended->SubFormat, kFloatFormat) == TRUE;
  }
  return false;
}

std::vector<std::int16_t> ToMono16(const BYTE* data, UINT32 frames, const WAVEFORMATEX* format) {
  const int channels = format->nChannels > 0 ? format->nChannels : 1;
  const bool floating = IsFloatFormat(format);
  std::vector<std::int16_t> mono(frames);
  for (UINT32 frame = 0; frame < frames; ++frame) {
    double sum = 0;
    for (int channel = 0; channel < channels; ++channel) {
      const std::size_t index = static_cast<std::size_t>(frame) * static_cast<std::size_t>(channels) + channel;
      if (floating) {
        float sample = 0;
        std::memcpy(&sample, data + index * sizeof(float), sizeof(float));
        sum += sample;
      } else if (format->wBitsPerSample == 16) {
        std::int16_t sample = 0;
        std::memcpy(&sample, data + index * sizeof(std::int16_t), sizeof(std::int16_t));
        sum += static_cast<double>(sample) / 32768.0;
      } else if (format->wBitsPerSample == 32) {
        std::int32_t sample = 0;
        std::memcpy(&sample, data + index * sizeof(std::int32_t), sizeof(std::int32_t));
        sum += static_cast<double>(sample) / 2147483648.0;
      }
    }
    double average = sum / channels;
    if (average > 1) average = 1;
    if (average < -1) average = -1;
    mono[frame] = static_cast<std::int16_t>(average * 32767.0);
  }
  return mono;
}

Rect ForegroundClient() {
  HWND window = GetForegroundWindow();
  RECT rect{};
  if (window == nullptr || !GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0) {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rect, 0);
    return Rect{rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top};
  }
  POINT origin{0, 0};
  ClientToScreen(window, &origin);
  return Rect{origin.x, origin.y, rect.right, rect.bottom};
}

bool WriteAll(HANDLE pipe, const std::string& bytes) {
  if (bytes.empty()) return true;
  OVERLAPPED overlap{};
  overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (overlap.hEvent == nullptr) return false;
  DWORD wrote = 0;
  BOOL ok = WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, &overlap);
  if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(pipe, &overlap, &wrote, TRUE);
  CloseHandle(overlap.hEvent);
  return ok == TRUE && wrote == bytes.size();
}

std::string DictDir() {
  const char* candidates[] = {"third_party/cppjieba/dict/jieba.dict.utf8", nullptr};
  if (GetFileAttributesA(candidates[0]) != INVALID_FILE_ATTRIBUTES) return "third_party/cppjieba/dict";
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe);
  const auto slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) path.resize(slash);
  path += L"\\..\\third_party\\cppjieba\\dict\\jieba.dict.utf8";
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    path.resize(path.size() - std::wstring(L"\\jieba.dict.utf8").size());
    return WideToUtf8(path);
  }
  return "";
}

std::string ModelDir() {
  if (GetFileAttributesA("models\\streaming") != INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesA("models/streaming") != INVALID_FILE_ATTRIBUTES) {
    return "models";
  }
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe);
  const auto slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) path.resize(slash);
  if (GetFileAttributesW((path + L"\\models\\streaming").c_str()) != INVALID_FILE_ATTRIBUTES) {
    return WideToUtf8(path + L"\\models");
  }
  if (GetFileAttributesW((path + L"\\..\\models\\streaming").c_str()) != INVALID_FILE_ATTRIBUTES) {
    return WideToUtf8(path + L"\\..\\models");
  }
  return "models";
}

class FakeRecognizer : public Recognizer {
 public:
  void AcceptAudio(const std::int16_t*, std::size_t count) override {
    std::lock_guard<std::mutex> lock(mu_);
    samples_ += count;
  }
  std::string Partial() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return std::to_string(samples_) + "假字";
  }
  std::string Finalize() override { return Partial() + "。"; }
  void Reset() override {
    std::lock_guard<std::mutex> lock(mu_);
    samples_ = 0;
  }

 private:
  mutable std::mutex mu_;
  std::size_t samples_ = 0;
};

class EngineLock {
 public:
  EngineLock() {
    handle_ = CreateMutexW(nullptr, FALSE, L"Local\\VoiceImeEngine");
    if (handle_ == nullptr) return;
    const DWORD waited = WaitForSingleObject(handle_, 0);
    owned_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
  }
  ~EngineLock() {
    if (owned_) ReleaseMutex(handle_);
    if (handle_ != nullptr) CloseHandle(handle_);
  }
  bool owned() const { return owned_; }

 private:
  HANDLE handle_ = nullptr;
  bool owned_ = false;
};

class Engine {
 public:
  int Run(bool fake);

 private:
  LRESULT OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  void OnPipe(const Message& message);
  void OnPartial();
  void OnEndpoint();
  void StartCapture();
  void StopCapture();
  void UpdateBox();
  void CaptureMain();
  void PipeMain();
  void ClientLoop(HANDLE pipe);
  void AddTray();
  void RemoveTray();
  void BeginRecordHotkey();
  void CloseRecord();
  void ApplyRecordedHotkey(int modifiers, int vk);
  LRESULT OnRecord(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  bool WriteClient(const Message& message);
  void Broadcast(const Message& message);
  void PublishPhase();
  void RequestCaret();
  void PublishCommit();
  void StoreLexicon();
  void BeginFinalize();
  void PublishFinal(const std::string& final_text);

  ConfirmBox box_;
  Session session_;
  Vad vad_{VadConfig{16000, 500, 60000, 800}};
  std::unique_ptr<Recognizer> recognizer_;
  std::unique_ptr<Lexicon> lexicon_;
  std::unique_ptr<Segmenter> segmenter_;
  std::thread capture_thread_;
  std::thread pipe_thread_;
  std::thread finalize_thread_;
  int finalize_generation_ = 0;
  std::string pending_insert_;
  std::atomic<bool> stop_capture_{false};
  std::atomic<bool> stop_pipe_{false};
  HANDLE wake_ = nullptr;
  HANDLE listen_pipe_ = INVALID_HANDLE_VALUE;
  HANDLE active_pipe_ = INVALID_HANDLE_VALUE;
  std::vector<HANDLE> clients_;
  std::vector<std::thread> client_threads_;
  std::mutex write_mu_;
  HANDLE write_event_ = nullptr;
  std::vector<std::pair<HANDLE, std::string>> outgoing_;
  std::mutex inbox_mu_;
  std::deque<std::pair<HANDLE, Message>> inbox_;
  bool tray_added_ = false;
  Settings settings_{DefaultHotkey()};
  std::string config_path_;
  std::vector<std::string> recent_menu_;
  HWND record_hwnd_ = nullptr;
  bool caret_known_ = false;
  Rect caret_{0, 0, 0, 0};
  Rect window_rect_{0, 0, 0, 0};
  std::string mic_error_;
  std::atomic<bool> partial_posted_{false};

  static LRESULT Hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* self) {
    return static_cast<Engine*>(self)->OnMessage(hwnd, message, wparam, lparam);
  }
  static LRESULT CALLBACK RecordProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

int Engine::Run(bool fake) {
  EngineLock instance_lock;
  if (!instance_lock.owned()) {
    std::fprintf(stderr, "voiceime_engine already running\n");
    return 0;
  }
  const std::wstring data = [] {
    wchar_t buffer[MAX_PATH];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring dir = count > 0 ? std::wstring(buffer, count) : L".";
    dir += L"\\VoiceIme";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
  }();
  lexicon_ = std::make_unique<Lexicon>(WideToUtf8(data + L"\\lexicon.sqlite"));
  config_path_ = WideToUtf8(data + L"\\config.json");
  settings_ = LoadSettings(config_path_);
  const std::string dict = DictDir();
  if (!dict.empty()) segmenter_ = std::make_unique<Segmenter>(dict);
  if (fake) {
    recognizer_ = std::make_unique<FakeRecognizer>();
    std::fprintf(stderr, "voiceime_engine --fake\n");
  } else {
    recognizer_ = MakeSherpaRecognizer(ModelDir(), segmenter_.get(), lexicon_.get());
    if (!recognizer_) return 2;
    std::fprintf(stderr, "voiceime_engine\n");
  }

  if (!box_.Create(GetModuleHandleW(nullptr), &Engine::Hook, this)) return 1;
  AddTray();
  write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  pipe_thread_ = std::thread([this] { PipeMain(); });

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  ++finalize_generation_;
  CloseRecord();
  stop_pipe_ = true;
  if (write_event_ != nullptr) SetEvent(write_event_);
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    if (listen_pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(listen_pipe_, nullptr);
    for (HANDLE client : clients_) CancelIoEx(client, nullptr);
  }
  StopCapture();
  if (finalize_thread_.joinable()) finalize_thread_.join();
  if (pipe_thread_.joinable()) pipe_thread_.join();
  for (std::thread& client : client_threads_) {
    if (client.joinable()) client.join();
  }
  if (write_event_ != nullptr) {
    CloseHandle(write_event_);
    write_event_ = nullptr;
  }
  RemoveTray();
  return 0;
}

void Engine::AddTray() {
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = box_.hwnd();
  icon.uID = 1;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = kTrayMessage;
  icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
  const std::wstring tip = Utf8ToWide("语音输入法");
  wcsncpy(icon.szTip, tip.c_str(), 127);
  tray_added_ = Shell_NotifyIconW(NIM_ADD, &icon) == TRUE;
}

void Engine::RemoveTray() {
  if (!tray_added_) return;
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = box_.hwnd();
  icon.uID = 1;
  Shell_NotifyIconW(NIM_DELETE, &icon);
  tray_added_ = false;
}

void Engine::BeginRecordHotkey() {
  if (record_hwnd_ != nullptr) {
    SetForegroundWindow(record_hwnd_);
    return;
  }
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = RecordProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = L"VoiceImeHotkey";
  RegisterClassW(&window_class);
  const std::wstring title = Utf8ToWide("按下新的快捷键");
  record_hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"VoiceImeHotkey", title.c_str(),
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 240, 240, 360, 140, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), this);
  if (record_hwnd_ != nullptr) SetForegroundWindow(record_hwnd_);
}

void Engine::CloseRecord() {
  if (record_hwnd_ == nullptr) return;
  HWND hwnd = record_hwnd_;
  record_hwnd_ = nullptr;
  DestroyWindow(hwnd);
}

void Engine::ApplyRecordedHotkey(int modifiers, int vk) {
  const Hotkey next{modifiers, vk};
  if (!HotkeyAccepted(next)) {
    CloseRecord();
    return;
  }
  Settings updated = settings_;
  updated.hotkey = next;
  SaveSettings(config_path_, updated);
  const Settings loaded = LoadSettings(config_path_);
  if (loaded.hotkey.modifiers == next.modifiers && loaded.hotkey.vk == next.vk) settings_ = loaded;
  CloseRecord();
}

LRESULT CALLBACK Engine::RecordProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto* self = reinterpret_cast<Engine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
  return self->OnRecord(hwnd, message, wparam, lparam);
}

LRESULT Engine::OnRecord(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
    const int vk = static_cast<int>(wparam);
    if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
        vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
      return 0;
    }
    if (vk == VK_ESCAPE) {
      CloseRecord();
      return 0;
    }
    int modifiers = 0;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) modifiers |= 1;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) modifiers |= 2;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) modifiers |= 4;
    ApplyRecordedHotkey(modifiers, vk);
    return 0;
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(hwnd, &paint);
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const std::wstring text = Utf8ToWide("按下新的快捷键。Esc 取消。");
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    EndPaint(hwnd, &paint);
    return 0;
  }
  if (message == WM_CLOSE) {
    CloseRecord();
    return 0;
  }
  if (message == WM_DESTROY) {
    if (record_hwnd_ == hwnd) record_hwnd_ = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Engine::UpdateBox() {
  if (session_.phase() == Phase::Idle) {
    box_.Hide();
    return;
  }
  const std::string text = mic_error_.empty() ? session_.box_text() : mic_error_;
  Rect caret{0, 0, 0, 0};
  Rect window = ForegroundClient();
  if (caret_known_) {
    const bool both_zero = caret_.x == 0 && caret_.y == 0 && caret_.w == 0 && caret_.h == 0 && window_rect_.x == 0 &&
                            window_rect_.y == 0 && window_rect_.w == 0 && window_rect_.h == 0;
    if (!both_zero) {
      caret = caret_;
      window = window_rect_;
    }
  }
  box_.ShowText(text, caret, window);
}

bool Engine::WriteClient(const Message& message) {
  std::lock_guard<std::mutex> lock(write_mu_);
  HANDLE target = active_pipe_;
  if (target == INVALID_HANDLE_VALUE && !clients_.empty()) target = clients_.back();
  if (target == INVALID_HANDLE_VALUE || write_event_ == nullptr) return false;
  outgoing_.emplace_back(target, Encode(message));
  SetEvent(write_event_);
  return true;
}

void Engine::Broadcast(const Message& message) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (write_event_ == nullptr) return;
  const std::string bytes = Encode(message);
  for (HANDLE client : clients_) outgoing_.emplace_back(client, bytes);
  if (!clients_.empty()) SetEvent(write_event_);
}

void Engine::PublishPhase() {
  Message phase;
  phase.type = MsgType::Phase;
  phase.text = std::to_string(static_cast<int>(session_.phase()));
  Broadcast(phase);
}

void Engine::RequestCaret() {
  Message query;
  query.type = MsgType::QueryCaret;
  Broadcast(query);
}

void Engine::StoreLexicon() {
  std::string stored;
  if (!session_.TakeLexiconWrite(&stored) || !lexicon_) return;
  std::vector<Token> tokens;
  if (segmenter_) {
    segmenter_->Remember(stored);
    tokens = segmenter_->Cut(stored);
    for (const Token& token : tokens) segmenter_->Remember(token.text);
  }
  lexicon_->Commit(stored, tokens, static_cast<std::int64_t>(std::time(nullptr)));
}

void Engine::PublishCommit() {
  std::string commit;
  if (!session_.TakeCommit(&commit)) return;
  pending_insert_ = commit;
  Message done;
  done.type = MsgType::CommitDone;
  done.text = commit;
  if (!WriteClient(done)) {
    session_.RestorePending(pending_insert_);
    pending_insert_.clear();
    PublishPhase();
  }
}

void Engine::BeginFinalize() {
  if (session_.phase() == Phase::Listening) session_.OnSpeechEnded();
  if (session_.phase() != Phase::Finalizing) return;
  if (finalize_thread_.joinable()) return;
  const int generation = ++finalize_generation_;
  PublishPhase();
  finalize_thread_ = std::thread([this, generation] {
    std::string text = recognizer_ ? recognizer_->Finalize() : std::string();
    auto* held = new std::string(std::move(text));
    if (box_.hwnd() == nullptr ||
        !PostMessageW(box_.hwnd(), kFinalMessage, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(held))) {
      delete held;
    }
  });
}

void Engine::PublishFinal(const std::string& final_text) {
  session_.OnFinal(final_text);
  std::fprintf(stderr, "final: %s\n", final_text.c_str());
  PublishCommit();
  if (session_.phase() != Phase::Idle) RequestCaret();
  UpdateBox();
  PublishPhase();
}

void Engine::StartCapture() {
  StopCapture();
  mic_error_.clear();
  vad_.Reset();
  stop_capture_ = false;
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  capture_thread_ = std::thread([this] { CaptureMain(); });
}

void Engine::StopCapture() {
  stop_capture_ = true;
  if (wake_ != nullptr) SetEvent(wake_);
  if (capture_thread_.joinable()) capture_thread_.join();
  if (wake_ != nullptr) {
    CloseHandle(wake_);
    wake_ = nullptr;
  }
  stop_capture_ = false;
}

void Engine::OnPartial() {
  partial_posted_ = false;
  if (session_.phase() != Phase::Listening) return;
  if (!mic_error_.empty()) {
    session_.OnDraft(mic_error_);
  } else {
    session_.OnDraft(recognizer_->Partial());
  }
  UpdateBox();
}

void Engine::OnEndpoint() {
  StopCapture();
  if (session_.phase() != Phase::Listening) return;
  BeginFinalize();
  UpdateBox();
}

void Engine::OnPipe(const Message& message) {
  if (message.type == MsgType::InsertAck) {
    if (message.text == "ok") {
      StoreLexicon();
    } else if (!pending_insert_.empty()) {
      session_.RestorePending(pending_insert_);
    }
    pending_insert_.clear();
    UpdateBox();
    PublishPhase();
    return;
  }
  if (message.type == MsgType::Caret) {
    caret_known_ = true;
    caret_ = Rect{message.caret_x, message.caret_y, message.caret_w, message.caret_h};
    window_rect_ = Rect{message.window_x, message.window_y, message.window_w, message.window_h};
    UpdateBox();
    return;
  }
  if (message.type == MsgType::Enter) {
    if (session_.phase() == Phase::Listening) {
      session_.Enter();
      StopCapture();
      BeginFinalize();
      UpdateBox();
      PublishPhase();
      return;
    }
    session_.Enter();
    PublishCommit();
    UpdateBox();
    PublishPhase();
    return;
  }
  if (message.type == MsgType::Cancel) {
    ++finalize_generation_;
    StopCapture();
    if (finalize_thread_.joinable()) finalize_thread_.join();
    session_.Escape();
    pending_insert_.clear();
    mic_error_.clear();
    caret_known_ = false;
    UpdateBox();
    PublishPhase();
    return;
  }
  if (message.type != MsgType::StartListen) return;
  if (session_.phase() == Phase::Listening) {
    StopCapture();
    session_.Hotkey();
    BeginFinalize();
    UpdateBox();
    return;
  }
  if (session_.phase() == Phase::Finalizing) return;
  if (finalize_thread_.joinable()) finalize_thread_.join();
  recognizer_->Reset();
  caret_known_ = false;
  session_.Hotkey();
  StartCapture();
  RequestCaret();
  UpdateBox();
  PublishPhase();
}

LRESULT Engine::OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kPipeMessage) {
    std::deque<std::pair<HANDLE, Message>> local;
    {
      std::lock_guard<std::mutex> lock(inbox_mu_);
      local.swap(inbox_);
    }
    for (const auto& item : local) {
      {
        std::lock_guard<std::mutex> lock(write_mu_);
        if (item.first != INVALID_HANDLE_VALUE) active_pipe_ = item.first;
      }
      OnPipe(item.second);
    }
    return 0;
  }
  if (message == kAnnounceMessage) {
    PublishPhase();
    return 0;
  }
  if (message == kFinalMessage) {
    auto* text = reinterpret_cast<std::string*>(lparam);
    const int generation = static_cast<int>(wparam);
    if (finalize_thread_.joinable()) finalize_thread_.join();
    if (text != nullptr) {
      if (generation == finalize_generation_ && session_.phase() == Phase::Finalizing) PublishFinal(*text);
      delete text;
    }
    return 0;
  }
  if (message == kPartialMessage) {
    OnPartial();
    return 0;
  }
  if (message == kEndpointMessage) {
    OnEndpoint();
    return 0;
  }
  if (message == kTrayMessage && (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP)) {
    HMENU menu = CreatePopupMenu();
    HMENU recent = CreatePopupMenu();
    recent_menu_.clear();
    if (lexicon_) {
      for (const Entry& entry : lexicon_->Recent(20)) {
        if (recent_menu_.size() >= 20) break;
        recent_menu_.push_back(entry.text);
        AppendMenuW(recent, MF_STRING, kRecentBase + static_cast<UINT>(recent_menu_.size() - 1),
                    Utf8ToWide(entry.text).c_str());
      }
    }
    if (recent_menu_.empty()) {
      AppendMenuW(recent, MF_STRING | MF_GRAYED, kRecentBase, Utf8ToWide("（空）").c_str());
    }
    AppendMenuW(menu, MF_STRING, kRecordCommand, Utf8ToWide("设置快捷键").c_str());
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recent), Utf8ToWide("最近记住的词").c_str());
    AppendMenuW(menu, MF_STRING, kQuitCommand, Utf8ToWide("退出").c_str());
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    return 0;
  }
  if (message == WM_COMMAND) {
    const UINT id = LOWORD(wparam);
    if (id == kQuitCommand) {
      DestroyWindow(hwnd);
      return 0;
    }
    if (id == kRecordCommand) {
      BeginRecordHotkey();
      return 0;
    }
    if (lexicon_ != nullptr && id >= kRecentBase && id < kRecentBase + recent_menu_.size()) {
      lexicon_->Delete(recent_menu_[id - kRecentBase]);
      return 0;
    }
  }
  if (message == WM_DESTROY) {
    RemoveTray();
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Engine::ClientLoop(HANDLE pipe) {
  PostMessageW(box_.hwnd(), kAnnounceMessage, 0, 0);
  OVERLAPPED overlap{};
  overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::string buffer;
  auto take_outgoing = [this, pipe] {
    std::vector<std::string> mine;
    std::lock_guard<std::mutex> lock(write_mu_);
    for (auto it = outgoing_.begin(); it != outgoing_.end();) {
      if (it->first == pipe) {
        mine.push_back(std::move(it->second));
        it = outgoing_.erase(it);
      } else {
        ++it;
      }
    }
    if (outgoing_.empty() && write_event_ != nullptr) ResetEvent(write_event_);
    return mine;
  };
  auto deliver = [this, pipe](std::string& pending) {
    for (;;) {
      Message decoded;
      std::size_t used = 0;
      if (!Decode(pending, &decoded, &used)) break;
      pending.erase(0, used);
      {
        std::lock_guard<std::mutex> lock(inbox_mu_);
        inbox_.emplace_back(pipe, std::move(decoded));
      }
      PostMessageW(box_.hwnd(), kPipeMessage, 0, 0);
    }
  };
  while (!stop_pipe_ && overlap.hEvent != nullptr) {
    const std::vector<std::string> mine = take_outgoing();
    if (!mine.empty()) {
      bool failed = false;
      for (const std::string& bytes : mine) {
        if (!WriteAll(pipe, bytes)) failed = true;
      }
      if (failed || stop_pipe_) break;
      continue;
    }
    ResetEvent(overlap.hEvent);
    char chunk[4096];
    DWORD got = 0;
    const BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), &got, &overlap);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
      HANDLE waits[2] = {overlap.hEvent, write_event_};
      const DWORD which = WaitForMultipleObjects(write_event_ == nullptr ? 1 : 2, waits, FALSE, INFINITE);
      if (stop_pipe_ || which == WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &overlap);
        DWORD partial = 0;
        if (GetOverlappedResult(pipe, &overlap, &partial, TRUE) && partial > 0) {
          buffer.append(chunk, chunk + partial);
          deliver(buffer);
        }
        if (stop_pipe_) break;
        continue;
      }
      if (!GetOverlappedResult(pipe, &overlap, &got, FALSE) || got == 0) break;
    } else if (!ok || got == 0) {
      break;
    }
    buffer.append(chunk, chunk + got);
    deliver(buffer);
  }
  if (overlap.hEvent != nullptr) CloseHandle(overlap.hEvent);
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    if (active_pipe_ == pipe) active_pipe_ = INVALID_HANDLE_VALUE;
    clients_.erase(std::remove(clients_.begin(), clients_.end(), pipe), clients_.end());
  }
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
}

void Engine::PipeMain() {
  while (!stop_pipe_) {
    HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\VoiceIme", PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (stop_pipe_) return;
      Sleep(100);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      listen_pipe_ = pipe;
    }
    OVERLAPPED connect{};
    connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL connected = FALSE;
    if (connect.hEvent != nullptr) {
      connected = ConnectNamedPipe(pipe, &connect);
      if (!connected) {
        const DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
          while (!stop_pipe_ && WaitForSingleObject(connect.hEvent, 200) == WAIT_TIMEOUT) {
          }
          DWORD unused = 0;
          connected = !stop_pipe_ && GetOverlappedResult(pipe, &connect, &unused, FALSE);
        } else if (error == ERROR_PIPE_CONNECTED) {
          connected = TRUE;
        }
      }
      CloseHandle(connect.hEvent);
    }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      if (listen_pipe_ == pipe) listen_pipe_ = INVALID_HANDLE_VALUE;
    }
    if (!connected || stop_pipe_) {
      CloseHandle(pipe);
      if (stop_pipe_) return;
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      clients_.push_back(pipe);
    }
    client_threads_.emplace_back([this, pipe] { ClientLoop(pipe); });
  }
}

void Engine::CaptureMain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  WAVEFORMATEX* format = nullptr;
  bool started = false;
  auto fail = [&](const char* reason) {
    mic_error_ = reason;
    PostMessageW(box_.hwnd(), kPartialMessage, 0, 0);
  };
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator))) ||
      enumerator == nullptr) {
    fail("没有麦克风");
  } else if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) || device == nullptr) {
    fail("没有麦克风");
  } else if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
             client == nullptr || FAILED(client->GetMixFormat(&format)) || format == nullptr) {
    fail("没有麦克风");
  } else if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, format,
                                        nullptr)) ||
             FAILED(client->SetEventHandle(wake_)) ||
             FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) ||
             capture == nullptr || FAILED(client->Start())) {
    fail("没有麦克风");
  } else {
    started = true;
  }

  while (started && !stop_capture_) {
    const DWORD waited = WaitForSingleObject(wake_, 200);
    if (stop_capture_) break;
    if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) break;
    UINT32 packet = 0;
    if (FAILED(capture->GetNextPacketSize(&packet))) break;
    while (packet > 0 && !stop_capture_) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
        packet = 0;
        break;
      }
      std::vector<std::int16_t> mono;
      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        mono.assign(frames, 0);
      } else {
        mono = ToMono16(data, frames, format);
      }
      capture->ReleaseBuffer(frames);
      std::vector<std::int16_t> samples =
          format->nSamplesPerSec == 16000 ? std::move(mono) : ResampleTo16k(mono.data(), mono.size(), format->nSamplesPerSec);
      recognizer_->AcceptAudio(samples.data(), samples.size());
      const VadEvent event = vad_.Push(samples.data(), samples.size());
      if (event == VadEvent::Endpoint || event == VadEvent::ForcedEndpoint) {
        if (!stop_capture_) PostMessageW(box_.hwnd(), kEndpointMessage, 0, 0);
        stop_capture_ = true;
        break;
      }
      if (!partial_posted_.exchange(true)) PostMessageW(box_.hwnd(), kPartialMessage, 0, 0);
      if (FAILED(capture->GetNextPacketSize(&packet))) packet = 0;
    }
  }

  if (started && client != nullptr) client->Stop();
  if (format != nullptr) CoTaskMemFree(format);
  if (capture != nullptr) capture->Release();
  if (client != nullptr) client->Release();
  if (device != nullptr) device->Release();
  if (enumerator != nullptr) enumerator->Release();
  if (com == S_OK) CoUninitialize();
}

}  // namespace

int main(int argc, char** argv) {
  bool fake = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--fake") == 0) fake = true;
  }
  Engine engine;
  return engine.Run(fake);
}
