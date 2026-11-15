#include "confirm_box.hpp"
#include "product.hpp"
#include "ui_theme.hpp"
#include "lexicon.hpp"
#include "recognizer.hpp"
#include "resample.hpp"
#include "segmenter.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "hotkey_dialog.hpp"
#include "sherpa_recognizer.hpp"
#include "vad.hpp"

#include "ipc.hpp"
#include "pipe_io.hpp"

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
constexpr UINT kCaptureErrorMessage = WM_APP + 7;
constexpr UINT kClientGoneMessage = WM_APP + 8;
constexpr UINT_PTR kInsertTimer = 1;
constexpr UINT_PTR kAnchorTimer = 2;
using ClientId = std::uintptr_t;
struct ClientConnection { ClientId id; HANDLE pipe; DWORD process; };
struct ClientWorker { std::thread thread; std::shared_ptr<std::atomic<bool>> finished; };
constexpr UINT kTrayMessage = WM_APP + 4;
constexpr UINT kQuitCommand = 1;
constexpr UINT kRecordCommand = 2;
constexpr UINT kSkinBase = 30;
constexpr UINT kRecentBase = 100;
const GUID kFloatFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#ifdef VOICEIME_TESTING
// Fixture configuration exists only in the separate test executable.
std::wstring TestEnvironment(const wchar_t* key) {
  wchar_t value[32768]{};
  const DWORD count = GetEnvironmentVariableW(key, value, 32768);
  return count > 0 && count < 32768 ? std::wstring(value, count) : L"";
}
#endif

std::wstring PipeName() {
#ifdef VOICEIME_TESTING
  return L"\\\\.\\pipe\\VoiceImeQA_" + TestEnvironment(L"VOICEIME_TEST_ID");
#else
  return L"\\\\.\\pipe\\VoiceIme";
#endif
}
std::wstring MutexName() {
#ifdef VOICEIME_TESTING
  return L"Local\\VoiceImeQA_" + TestEnvironment(L"VOICEIME_TEST_ID");
#else
  return L"Local\\VoiceImeEngine";
#endif
}

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

std::wstring ExecutableDir() {
  wchar_t exe[MAX_PATH];
  const DWORD size = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!size || size >= MAX_PATH) return L"";
  std::wstring path(exe, size);
  const auto slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::string ResourceDir(const wchar_t* relative, const wchar_t* marker) {
  const auto exe = ExecutableDir();
  // Packaged assets win over the host application's working directory.
  // The parent fallback preserves the source-tree development layout.
  for (const auto& root : {exe, exe + L"\\..", std::wstring(L".")}) {
    const auto dir = root + L"\\" + relative;
    const auto file = dir + L"\\" + marker;
    const DWORD attr = GetFileAttributesW(file.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return WideToUtf8(dir);
  }
  return "";
}

std::string DictDir() { return ResourceDir(L"third_party\\cppjieba\\dict", L"jieba.dict.utf8"); }
std::string ModelDir() { return ResourceDir(L"models", L"streaming\\model.int8.onnx"); }

int CheckInstallation() {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  const auto dict = DictDir();
  for (const char* file : {"jieba.dict.utf8", "hmm_model.utf8", "user.dict.utf8", "idf.utf8", "stop_words.utf8", "pinyin.txt"}) {
    if (dict.empty() || GetFileAttributesW(Utf8ToWide(dict + "/" + file).c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::fprintf(stderr, "Missing dictionary: %s\n", file);
      return 2;
    }
  }
  if (GetFileAttributesW((ExecutableDir() + L"\\rime-data\\voiceime.schema.yaml").c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::fprintf(stderr, "Missing rime-data/voiceime.schema.yaml\n");
    return 2;
  }
  Segmenter segmenter(dict);
  auto recognizer = MakeSherpaRecognizer(ModelDir(), &segmenter, nullptr);
  if (!recognizer) return 2;
  std::printf("%s %s: dictionaries and all three local models loaded.\n", kProductName, SIMPLE_VOICE_VERSION);
  std::puts("No microphone, input-method registration or personal data was accessed.");
  return 0;
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
  std::string Finalize() override {
#ifdef VOICEIME_TESTING
    if (TestEnvironment(L"VOICEIME_TEST_CAPTURE")==L"slow-final") Sleep(250);
#endif
    return Partial() + "。";
  }
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
    handle_ = CreateMutexW(nullptr, FALSE, MutexName().c_str());
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
  void OnPipe(ClientId sender, const Message& message);
  void ShowMenu(HWND hwnd);
  void OnPartial();
  void OnEndpoint();
  void StartCapture();
  void StopCapture();
  void UpdateBox();
  void CaptureMain(int generation);
  void PipeMain();
  void ClientLoop(ClientId id, HANDLE pipe);
  void ReapClients();
  void LoseInsertConfirmation();
  void LexiconWarning(const char* diagnostic);
  void AddTray();
  void RemoveTray();
  void BeginRecordHotkey();
  void CloseRecord();
  static bool SaveRecordedHotkey(void* self, Hotkey next);

  bool WriteClient(const Message& message, ClientId target);
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
  int capture_generation_ = 0;
  std::string pending_insert_;
  std::uint64_t insert_id_ = 0, next_insert_id_ = 0;
  ClientId insert_client_ = 0;
  ClientId commit_client_ = 0;
  bool insert_uncertain_ = false;
  std::string lexicon_warning_;
  std::atomic<bool> stop_capture_{false};
  std::atomic<bool> stop_pipe_{false};
  HANDLE wake_ = nullptr;
  HANDLE listen_pipe_ = INVALID_HANDLE_VALUE;
  std::vector<ClientConnection> clients_;
  std::vector<ClientWorker> client_threads_;
  std::mutex write_mu_;
  HANDLE write_event_ = nullptr;
  HANDLE stop_event_ = nullptr;
  std::vector<std::pair<ClientId, std::string>> outgoing_;
  std::mutex inbox_mu_;
  std::deque<std::pair<ClientId, Message>> inbox_;
  bool tray_added_ = false;
  Settings settings_{DefaultHotkey()};
  std::wstring skin_path_;
  std::string config_path_;
  std::vector<std::string> recent_menu_;
  HotkeyDialog hotkey_dialog_;
  bool caret_known_ = false;
  DWORD caret_process_ = 0;
  ULONGLONG listening_started_ = 0;
  Rect caret_{0, 0, 0, 0};
  Rect window_rect_{0, 0, 0, 0};
  std::string mic_error_;
  std::atomic<bool> partial_posted_{false};

  static LRESULT Hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* self) {
    return static_cast<Engine*>(self)->OnMessage(hwnd, message, wparam, lparam);
  }

};

int Engine::Run(bool fake) {
#ifdef VOICEIME_TESTING
  if (TestEnvironment(L"VOICEIME_TEST_ID").empty() || TestEnvironment(L"VOICEIME_TEST_DATA").empty()) return 3;
  fake = true;
#endif
  EngineLock instance_lock;
  if (!instance_lock.owned()) {
    std::fprintf(stderr, "voiceime_engine already running\n");
    return 0;
  }
  const std::wstring data = [] {
#ifdef VOICEIME_TESTING
    const auto dir = TestEnvironment(L"VOICEIME_TEST_DATA");
#else
    wchar_t buffer[MAX_PATH];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring dir = count > 0 ? std::wstring(buffer, count) : L".";
    dir += L"\\VoiceIme";
#endif
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
  }();
  try { lexicon_ = std::make_unique<Lexicon>(WideToUtf8(data + L"\\lexicon.sqlite")); }
  catch (const std::exception& e) { LexiconWarning(e.what()); }
  config_path_ = WideToUtf8(data + L"\\config.json");
  settings_ = LoadSettings(config_path_);
  skin_path_ = data + L"\\appearance.json";
  box_.SetSkin(LoadSkin(skin_path_));
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
#ifndef VOICEIME_TESTING
  AddTray();
#endif
  write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  pipe_thread_ = std::thread([this] { PipeMain(); });

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (hotkey_dialog_.Translate(&message)) continue;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  ++finalize_generation_;
  CloseRecord();
  stop_pipe_ = true;
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  if (write_event_ != nullptr) SetEvent(write_event_);
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    if (listen_pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(listen_pipe_, nullptr);
    for (const auto& client : clients_) CancelIoEx(client.pipe, nullptr);
  }
  StopCapture();
  if (finalize_thread_.joinable()) finalize_thread_.join();
  if (pipe_thread_.joinable()) pipe_thread_.join();
  for (auto& client : client_threads_) {
    if (client.thread.joinable()) client.thread.join();
  }
  if (write_event_ != nullptr) {
    CloseHandle(write_event_);
    write_event_ = nullptr;
  }
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
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
  const std::wstring tip = std::wstring(kProductDisplayName) + L" · Simple Voice";
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
  hotkey_dialog_.Open(GetModuleHandleW(nullptr), settings_.hotkey, &Engine::SaveRecordedHotkey, this);
}

void Engine::CloseRecord() { hotkey_dialog_.Close(); }

bool Engine::SaveRecordedHotkey(void* self, Hotkey next) {
  auto* engine = static_cast<Engine*>(self);
  Settings updated = engine->settings_;
  updated.hotkey = next;
  if (!SaveSettings(engine->config_path_, updated)) return false;
  engine->settings_ = updated;
  return true;
}
void Engine::UpdateBox() {
  UiDpiScope coordinates;
  if (session_.phase() == Phase::Idle) {
    box_.Hide();
    return;
  }
#ifndef VOICEIME_TESTING
  DWORD foreground_process=0;
  GetWindowThreadProcessId(GetForegroundWindow(),&foreground_process);
  if (caret_known_ && caret_process_!=0 && caret_process_!=foreground_process) { box_.Hide(); return; }
  if (!caret_known_ && GetTickCount64()-listening_started_<180) return;
#endif
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
  box_.ShowText(text, caret, window, session_.phase(), !mic_error_.empty(), insert_uncertain_);
}

bool Engine::WriteClient(const Message& message, ClientId target) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (write_event_ == nullptr || std::none_of(clients_.begin(), clients_.end(),
      [target](const auto& client) { return client.id == target; })) return false;
  auto bytes = Encode(message);
  if (bytes.empty()) return false;
  outgoing_.emplace_back(target, std::move(bytes));
  SetEvent(write_event_);
  return true;
}

void Engine::Broadcast(const Message& message) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (write_event_ == nullptr) return;
  const std::string bytes = Encode(message);
  if (bytes.empty()) return;
  for (const auto& client : clients_) outgoing_.emplace_back(client.id, bytes);
  if (!clients_.empty()) SetEvent(write_event_);
}

void Engine::PublishPhase() {
  if (session_.phase()==Phase::Idle) KillTimer(box_.hwnd(),kAnchorTimer);
  else SetTimer(box_.hwnd(),kAnchorTimer,80,nullptr);
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
  try {
  std::vector<Token> tokens;
  if (segmenter_) {
    segmenter_->Remember(stored);
    tokens = segmenter_->Cut(stored);
    for (const Token& token : tokens) segmenter_->Remember(token.text);
  }
  lexicon_->Commit(stored, tokens, static_cast<std::int64_t>(std::time(nullptr)));
  lexicon_warning_.clear();
  } catch (const std::exception& e) { LexiconWarning(e.what()); }
}

void Engine::LexiconWarning(const char* diagnostic) {
  std::fprintf(stderr, "lexicon unavailable: %s\n", diagnostic);
  const bool first = lexicon_warning_.empty();
  lexicon_warning_ = "词库暂不可用，已写入的文字不受影响";
  if (!first || !tray_added_) return;
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon); icon.hWnd = box_.hwnd(); icon.uID = 1;
  icon.uFlags = NIF_INFO; icon.dwInfoFlags = NIIF_WARNING;
  wcscpy_s(icon.szInfoTitle, L"语音词库未更新");
  wcscpy_s(icon.szInfo, L"已写入的文字不受影响。请稍后重试，或从托盘查看词库状态。");
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void Engine::PublishCommit() {
  std::string commit;
  if (!session_.TakeCommit(&commit)) return;
  pending_insert_ = commit;
  insert_id_ = ++next_insert_id_;
  insert_client_ = commit_client_;
  insert_uncertain_ = false;
  Message done;
  done.type = MsgType::InsertText;
  done.request_id = insert_id_;
  done.text = commit;
  if (!WriteClient(done, insert_client_)) {
    session_.RestorePending(pending_insert_);
    pending_insert_.clear();
    insert_id_ = 0;
    PublishPhase();
  } else if (!SetTimer(box_.hwnd(), kInsertTimer, 5000, nullptr)) LoseInsertConfirmation();
}

void Engine::LoseInsertConfirmation() {
  if (insert_id_ == 0) return;
  Message cancel; cancel.type = MsgType::CancelInsert; cancel.request_id = insert_id_;
  WriteClient(cancel, insert_client_);
  KillTimer(box_.hwnd(), kInsertTimer);
  session_.RestorePending(pending_insert_);
  pending_insert_.clear(); insert_id_ = 0;
  insert_uncertain_ = true;
  UpdateBox(); PublishPhase();
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
  if (recognizer_ && !recognizer_->TakeWarning().empty()) LexiconWarning("recognition retained without lexicon bias");
  if (session_.phase() != Phase::Idle) RequestCaret();
  UpdateBox();
  PublishPhase();
}

void Engine::StartCapture() {
  StopCapture();
  mic_error_.clear();
  vad_.Reset();
  partial_posted_ = false;
  stop_capture_ = false;
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const int generation = ++capture_generation_;
  capture_thread_ = std::thread([this, generation] { CaptureMain(generation); });
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
  session_.OnDraft(recognizer_->Partial());
  UpdateBox();
}

void Engine::OnEndpoint() {
  StopCapture();
  if (session_.phase() != Phase::Listening) return;
  BeginFinalize();
  UpdateBox();
}

void Engine::ShowMenu(HWND hwnd) {
  HMENU menu = CreatePopupMenu();
  HMENU recent = CreatePopupMenu();
  recent_menu_.clear();
  if (lexicon_) {
    try {
    for (const Entry& entry : lexicon_->Recent(20)) {
      if (recent_menu_.size() >= 20) break;
      recent_menu_.push_back(entry.text);
      AppendMenuW(recent, MF_STRING, kRecentBase + static_cast<UINT>(recent_menu_.size() - 1),
                  Utf8ToWide("删除「" + entry.text + "」").c_str());
    }
    } catch (const std::exception& e) { LexiconWarning(e.what()); }
  }
  if (recent_menu_.empty()) {
    AppendMenuW(recent, MF_STRING | MF_GRAYED, kRecentBase, Utf8ToWide("（空）").c_str());
  }
  AppendMenuW(menu, MF_STRING, kRecordCommand, Utf8ToWide("设置快捷键").c_str());
  if (const auto skins=UiSkinMenu(box_.skin(),kSkinBase))
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(skins),L"皮肤");
  if (!lexicon_warning_.empty()) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, Utf8ToWide(lexicon_warning_).c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(recent), Utf8ToWide("最近记住的词（点击删除）").c_str());
  AppendMenuW(menu, MF_STRING, kQuitCommand, Utf8ToWide("退出").c_str());
  POINT point;
  GetCursorPos(&point);
  SetForegroundWindow(hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
  DestroyMenu(menu);
}

void Engine::OnPipe(ClientId sender, const Message& message) {
  if (message.type == MsgType::ShowSettings) {
    ShowMenu(box_.hwnd());
    return;
  }
  if (message.type == MsgType::InsertResult) {
    if (insert_id_ == 0 || sender != insert_client_ || message.request_id != insert_id_) return;
    KillTimer(box_.hwnd(), kInsertTimer);
    if (message.text == "ok") {
      session_.ConfirmInserted();
      StoreLexicon();
    } else if (!pending_insert_.empty()) {
      session_.RestorePending(pending_insert_);
    }
    pending_insert_.clear();
    insert_id_ = 0;
    insert_uncertain_ = false;
    UpdateBox();
    PublishPhase();
    return;
  }
  if (message.type == MsgType::Caret) {
    if (message.caret_w<=0 || message.caret_h<=0) return;
#ifndef VOICEIME_TESTING
    DWORD foreground_process=0,client_process=0;
    GetWindowThreadProcessId(GetForegroundWindow(),&foreground_process);
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      for (const auto& client:clients_) if(client.id==sender) client_process=client.process;
    }
    if (!client_process || client_process!=foreground_process) return;
    caret_process_=client_process;
#endif
    caret_known_ = true;
    caret_ = Rect{message.caret_x, message.caret_y, message.caret_w, message.caret_h};
    window_rect_ = Rect{message.window_x, message.window_y, message.window_w, message.window_h};
    UpdateBox();
    return;
  }
  if (message.type == MsgType::Enter) {
    if (!mic_error_.empty()) return;
    if (session_.phase() == Phase::Committing || session_.phase() == Phase::Finalizing) return;
    if (session_.phase() == Phase::Listening) {
      session_.Enter();
      StopCapture();
      BeginFinalize();
      UpdateBox();
      PublishPhase();
      return;
    }
    commit_client_ = sender;
    session_.Enter();
    PublishCommit();
    UpdateBox();
    PublishPhase();
    return;
  }
  if (message.type == MsgType::Cancel) {
    if (session_.phase() == Phase::Committing) return;
    ++finalize_generation_;
    StopCapture();
    if (finalize_thread_.joinable()) finalize_thread_.join();
    session_.Escape();
    pending_insert_.clear();
    insert_id_ = 0;
    insert_uncertain_ = false;
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
  if (session_.phase() == Phase::Finalizing || session_.phase() == Phase::Committing) return;
  if (finalize_thread_.joinable()) finalize_thread_.join();
  recognizer_->Reset();
  caret_known_ = false;
  caret_process_ = 0;
  listening_started_=GetTickCount64();
  insert_uncertain_ = false;
  session_.Hotkey();
  StartCapture();
  RequestCaret();
  UpdateBox();
  PublishPhase();
}

LRESULT Engine::OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kClientGoneMessage) {
    if (insert_id_ != 0 && static_cast<ClientId>(wparam) == insert_client_) LoseInsertConfirmation();
    return 0;
  }
  if (message == WM_TIMER && wparam == kInsertTimer) { LoseInsertConfirmation(); return 0; }
  if (message == WM_TIMER && wparam == kAnchorTimer) { UpdateBox(); return 0; }
  if (message == kPipeMessage) {
    std::deque<std::pair<ClientId, Message>> local;
    {
      std::lock_guard<std::mutex> lock(inbox_mu_);
      local.swap(inbox_);
    }
    for (const auto& item : local) {
      OnPipe(item.first, item.second);
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
    if (static_cast<int>(wparam) == capture_generation_) OnPartial();
    return 0;
  }
  if (message == kEndpointMessage) {
    if (static_cast<int>(wparam) == capture_generation_) OnEndpoint();
    return 0;
  }
  if (message == kCaptureErrorMessage) {
    if (static_cast<int>(wparam) == capture_generation_ && session_.phase() == Phase::Listening) {
      StopCapture();
      // The capture thread posts a static diagnostic; UI state has one owner.
      mic_error_ = reinterpret_cast<const char*>(lparam);
      session_.RestorePending("");
      UpdateBox();
      PublishPhase();
    }
    return 0;
  }
  if (message == kTrayMessage && (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP)) {
    ShowMenu(hwnd);
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
    if (id>=kSkinBase && id<kSkinBase+3) {
      const auto skin=static_cast<UiSkin>(id-kSkinBase);
      if (SaveSkin(skin_path_,skin)) {
        box_.SetSkin(skin);
#ifndef VOICEIME_TESTING
        // Existing candidate windows in other applications reload the saved
        // preference; this does not send keystrokes or change editor focus.
        SendNotifyMessageW(HWND_BROADCAST,UiSkinChangedMessage(),0,0);
#endif
      } else {
        std::fprintf(stderr,"appearance preference could not be saved\n");
        if (tray_added_) {
          NOTIFYICONDATAW icon{}; icon.cbSize=sizeof(icon); icon.hWnd=box_.hwnd(); icon.uID=1;
          icon.uFlags=NIF_INFO; icon.dwInfoFlags=NIIF_WARNING;
          wcscpy_s(icon.szInfoTitle,L"皮肤未切换");
          wcscpy_s(icon.szInfo,L"未能保存皮肤选择，仍使用原来的皮肤。请稍后重试。");
          Shell_NotifyIconW(NIM_MODIFY,&icon);
        }
      }
      return 0;
    }
    if (lexicon_ != nullptr && id >= kRecentBase && id < kRecentBase + recent_menu_.size()) {
      try { lexicon_->Delete(recent_menu_[id - kRecentBase]); lexicon_warning_.clear(); }
      catch (const std::exception& e) { LexiconWarning(e.what()); }
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

void Engine::ClientLoop(ClientId id, HANDLE pipe) {
  PostMessageW(box_.hwnd(), kAnnounceMessage, 0, 0);
  std::string buffer;
  auto take_outgoing = [this, id] {
    std::vector<std::string> mine;
    std::lock_guard<std::mutex> lock(write_mu_);
    for (auto it = outgoing_.begin(); it != outgoing_.end();) {
      if (it->first == id) {
        mine.push_back(std::move(it->second));
        it = outgoing_.erase(it);
      } else {
        ++it;
      }
    }
    if (outgoing_.empty() && write_event_ != nullptr) ResetEvent(write_event_);
    return mine;
  };
  auto deliver = [this, id](std::string& pending) {
    for (;;) {
      Message decoded;
      std::size_t used = 0;
      const auto status = DecodeFrame(pending, &decoded, &used);
      if (status == DecodeStatus::Invalid) return false;
      if (status == DecodeStatus::Incomplete) return true;
      pending.erase(0, used);
      {
        std::lock_guard<std::mutex> lock(inbox_mu_);
        inbox_.emplace_back(id, std::move(decoded));
      }
      PostMessageW(box_.hwnd(), kPipeMessage, 0, 0);
    }
  };
  while (!stop_pipe_) {
    const std::vector<std::string> mine = take_outgoing();
    if (!mine.empty()) {
      bool failed = false;
      for (const std::string& bytes : mine) {
        if (!WritePipeBytes(pipe, bytes, stop_event_)) { failed = true; break; }
      }
      if (failed || stop_pipe_) break;
      continue;
    }
    const PipeReadResult received = ReadPipeChunk(pipe, write_event_, stop_event_);
    buffer += received.bytes;
    if (!deliver(buffer)) break;
    if (received.status == PipeReadStatus::Closed) break;
  }
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
        [id](const auto& client) { return client.id == id; }), clients_.end());
    outgoing_.erase(std::remove_if(outgoing_.begin(), outgoing_.end(),
        [id](const auto& item) { return item.first == id; }), outgoing_.end());
    if (outgoing_.empty() && write_event_ != nullptr) ResetEvent(write_event_);
  }
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);
  PostMessageW(box_.hwnd(), kClientGoneMessage, id, 0);
}

void Engine::ReapClients() {
  for (auto it = client_threads_.begin(); it != client_threads_.end();) {
    if (!it->finished->load()) { ++it; continue; }
    it->thread.join();
    it = client_threads_.erase(it);
  }
}

void Engine::PipeMain() {
  ClientId next_client = 0;
  while (!stop_pipe_) {
    ReapClients();
    HANDLE pipe = CreateNamedPipeW(PipeName().c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
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
            ReapClients();
          }
          DWORD unused = 0;
          if (stop_pipe_) {
            CancelIoEx(pipe, &connect);
            GetOverlappedResult(pipe, &connect, &unused, TRUE);
          } else {
            connected = GetOverlappedResult(pipe, &connect, &unused, FALSE);
          }
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
    const ClientId id = ++next_client;
    DWORD process=0;
    using QueryClient=BOOL(WINAPI*)(HANDLE,PULONG);
    static auto query_client=reinterpret_cast<QueryClient>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"GetNamedPipeClientProcessId"));
    if (query_client) query_client(pipe,&process);
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      clients_.push_back({id, pipe, process});
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    client_threads_.push_back({std::thread([this, id, pipe, finished] {
      ClientLoop(id, pipe); finished->store(true);
    }), finished});
  }
}

void Engine::CaptureMain(int generation) {
#ifdef VOICEIME_TESTING
  const auto fixture = TestEnvironment(L"VOICEIME_TEST_CAPTURE");
  if (fixture == L"error") {
    PostMessageW(box_.hwnd(), kCaptureErrorMessage, static_cast<WPARAM>(generation),
                 reinterpret_cast<LPARAM>("测试：麦克风采集已中断"));
    return;
  }
  std::vector<std::int16_t> fixture_samples(fixture == L"silence" ? 16000 * 6 : 320, 0);
  int fixture_frames=0;
  while (!stop_capture_) {
    if(fixture==L"speech-end") std::fill(fixture_samples.begin(),fixture_samples.end(),fixture_frames++<5 ? 1200 : 0);
    recognizer_->AcceptAudio(fixture_samples.data(), fixture_samples.size());
    if (fixture == L"silence" || fixture == L"speech-end") {
      const auto event=vad_.Push(fixture_samples.data(),fixture_samples.size());
      if(event==VadEvent::Endpoint || event==VadEvent::ForcedEndpoint) {
        PostMessageW(box_.hwnd(), kEndpointMessage, static_cast<WPARAM>(generation), 0);
        return;
      }
    }
    if (!partial_posted_.exchange(true)) PostMessageW(box_.hwnd(), kPartialMessage, static_cast<WPARAM>(generation), 0);
    if (WaitForSingleObject(wake_, 20) == WAIT_OBJECT_0) break;
  }
  return;
#endif
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* enumerator = nullptr;
  IMMDevice* device = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* capture = nullptr;
  WAVEFORMATEX* format = nullptr;
  bool started = false;
  auto fail = [&](const char* reason) {
    stop_capture_ = true;
    PostMessageW(box_.hwnd(), kCaptureErrorMessage, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(reason));
  };
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator))) ||
      enumerator == nullptr) {
    fail("未能打开麦克风。");
  } else if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device)) || device == nullptr) {
    fail("未能打开麦克风。");
  } else if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
             client == nullptr || FAILED(client->GetMixFormat(&format)) || format == nullptr) {
    fail("未能打开麦克风。");
  } else if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, format,
                                        nullptr)) ||
             FAILED(client->SetEventHandle(wake_)) ||
             FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) ||
             capture == nullptr || FAILED(client->Start())) {
    fail("未能打开麦克风。");
  } else {
    started = true;
  }

  std::unique_ptr<Resampler16k> resampler;
  if (started) {
    if (format->nSamplesPerSec == 0 || format->nSamplesPerSec > 384000) {
      fail("麦克风采样格式暂不支持，请更换输入设备。");
    } else resampler = std::make_unique<Resampler16k>(format->nSamplesPerSec);
  }
  while (started && !stop_capture_) {
    const DWORD waited = WaitForSingleObject(wake_, 200);
    if (stop_capture_) break;
    if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT) { fail("麦克风采集已中断"); break; }
    UINT32 packet = 0;
    if (FAILED(capture->GetNextPacketSize(&packet))) { fail("麦克风采集已中断"); break; }
    while (packet > 0 && !stop_capture_) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
        fail("麦克风采集已中断");
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
      std::vector<std::int16_t> samples = resampler->Process(mono.data(), mono.size());
      recognizer_->AcceptAudio(samples.data(), samples.size());
      const VadEvent event = vad_.Push(samples.data(), samples.size());
      if (event == VadEvent::Endpoint || event == VadEvent::ForcedEndpoint) {
        if (!stop_capture_) PostMessageW(box_.hwnd(), kEndpointMessage, static_cast<WPARAM>(generation), 0);
        stop_capture_ = true;
        break;
      }
      if (!partial_posted_.exchange(true)) PostMessageW(box_.hwnd(), kPartialMessage, static_cast<WPARAM>(generation), 0);
      if (FAILED(capture->GetNextPacketSize(&packet))) { fail("麦克风采集已中断"); packet = 0; }
    }
  }

  if (resampler) {
    const auto tail = resampler->Flush();
    if (!tail.empty()) recognizer_->AcceptAudio(tail.data(), tail.size());
  }
  if (started && client != nullptr) client->Stop();
  if (format != nullptr) CoTaskMemFree(format);
  if (capture != nullptr) capture->Release();
  if (client != nullptr) client->Release();
  if (device != nullptr) device->Release();
  if (enumerator != nullptr) enumerator->Release();
  if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace

int main(int argc, char** argv) {
  bool fake = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--version") == 0) {
      std::printf("%s %s-preview\n", kProductName, SIMPLE_VOICE_VERSION);
      return 0;
    }
    if (std::strcmp(argv[i], "--check") == 0) return CheckInstallation();
    if (std::strcmp(argv[i], "--fake") == 0) fake = true;
  }
  Engine engine;
  return engine.Run(fake);
}
