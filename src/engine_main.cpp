#include "confirm_box.hpp"
#include "product.hpp"
#include "brand_icon.hpp"
#include "ui_theme.hpp"
#include "lexicon.hpp"
#include "recognizer.hpp"
#include "resample.hpp"
#include "segmenter.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "hotkey_dialog.hpp"
#include "preferences_window.hpp"
#include "recognition_text.hpp"
#include "sherpa_recognizer.hpp"
#include "vad.hpp"
#include "capture_health.hpp"

#include "ipc.hpp"
#include "pipe_io.hpp"
#include "ipc_security.hpp"

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
constexpr UINT kPreparedMessage = WM_APP + 9;
constexpr UINT kCaptureReadyMessage = WM_APP + 10;
constexpr UINT_PTR kInsertTimer = 1;
constexpr UINT_PTR kAnchorTimer = 2;
using ClientId = std::uintptr_t;
struct ClientConnection { ClientId id; HANDLE pipe; std::shared_ptr<IpcPeer> peer; HWND window = nullptr; };
struct ClientWorker { std::thread thread; std::shared_ptr<std::atomic<bool>> finished; };
constexpr UINT kTrayMessage = WM_APP + 4;
constexpr UINT kQuitCommand = 1;
constexpr UINT kRecordCommand = 2;
constexpr UINT kSkinBase = 30;
constexpr UINT kSettingsCommand=3, kTermsCommand=4, kEditCommand=5, kCopyCommand=6, kRestoreCommand=7, kGuideCommand=8;
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
  return VoicePipeName();
#endif
}
std::wstring MutexName() {
#ifdef VOICEIME_TESTING
  return L"Local\\VoiceImeQA_" + TestEnvironment(L"VOICEIME_TEST_ID");
#else
  return VoiceMutexName();
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
std::string ModelDir() { return ResourceDir(L"models", L"streaming-bilingual\\encoder-epoch-99-avg-1.int8.onnx"); }

int CheckInstallation(bool chinese_only=false) {
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
  auto recognizer = MakeSherpaRecognizer(ModelDir(), chinese_only);
  if (!recognizer) return 2;
  std::printf("%s %s: dictionaries and %s speech models loaded.\n", kProductName, SIMPLE_VOICE_VERSION, chinese_only?"Chinese-preferred":"bilingual");
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
    const auto transcript=TestEnvironment(L"VOICEIME_TEST_TRANSCRIPT");
    if(!transcript.empty())return WideToUtf8(transcript);
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
    PipeSecurity security;
    const auto name = MutexName();
    if (name.empty() || !security.attributes()) return;
    handle_ = CreateMutexW(security.attributes(), FALSE, name.c_str());
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
  void OnPipe(ClientId sender, const Message& message, bool resumed = false);
  bool ForegroundClientAllowed(ClientId sender) const;
  void ResumeQueuedStart();
  Message StateMessage();
  void ShowMenu(HWND hwnd);
  void OpenPreferences(int page=0);
  void BeginPrepare();
  bool ExportTerms();
  std::wstring Diagnostics() const;
  void EditPreview();
  void OnPartial();
  void OnEndpoint();
  void StartCapture();
  void StopCapture();
  void UpdateBox();
  void CaptureMain(int generation,std::wstring microphone);
  void PipeMain();
  void ClientLoop(ClientId id, HANDLE pipe, std::shared_ptr<IpcPeer> peer);
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
  std::thread prepare_thread_;
  std::unique_ptr<Recognizer> prepared_;
  std::string final_result_, final_error_, original_text_;
  bool preparing_=false, fake_=false, recognizer_chinese_=false, preparing_chinese_=false;
#ifdef VOICEIME_TESTING
  int prepare_attempts_=0;
  DWORD test_foreground_ = 0;
#endif
  std::string model_error_;
  ClientId queued_start_=0;
  std::atomic<bool> capture_ready_{false};
  std::atomic<int> input_level_{0};
  InputPreferences preferences_;
  std::wstring data_path_, preferences_path_;
  PreferencesWindow preferences_window_;
  PreviewEditor preview_editor_;
  int finalize_generation_ = 0;
  int capture_generation_ = 0;
  std::string pending_insert_;
  std::uint64_t insert_id_ = 0, next_insert_id_ = 0;
  ClientId insert_client_ = 0;
  ClientId commit_client_ = 0;
  bool insert_uncertain_ = false;
  std::uint64_t utterance_generation_ = 0;
  ClientId start_client_ = 0;
  std::uint64_t start_from_state_ = 0;
  std::uint64_t utterance_state_floor_ = 0;
  std::uint64_t state_id_ = 1, published_utterance_ = 0;
  Phase published_phase_ = Phase::Idle;
  std::string published_preview_;
  std::string lexicon_warning_;
  std::atomic<bool> stop_capture_{false};
  std::atomic<bool> stop_pipe_{false};
  HANDLE wake_ = nullptr;
  HANDLE listen_pipe_ = INVALID_HANDLE_VALUE;
  std::vector<ClientConnection> clients_;
  std::vector<ClientWorker> client_threads_;
  mutable std::mutex write_mu_;
  HANDLE write_event_ = nullptr;
  HANDLE stop_event_ = nullptr;
  std::vector<std::pair<ClientId, std::string>> outgoing_;
  std::mutex inbox_mu_;
  std::deque<std::pair<ClientId, Message>> inbox_;
  bool tray_added_ = false;
  Settings settings_{DefaultHotkey()};
  std::wstring skin_path_;
  std::string config_path_;
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
  test_foreground_ = std::wcstoul(TestEnvironment(L"VOICEIME_TEST_FOREGROUND").c_str(), nullptr, 10);
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
  data_path_=data; preferences_path_=data+L"\\preferences.ini";
  preferences_=LoadInputPreferences(preferences_path_);
  ExportTerms();
  config_path_ = WideToUtf8(data + L"\\config.json");
  settings_ = LoadSettings(config_path_);
  skin_path_ = data + L"\\appearance.json";
  box_.SetSkin(LoadSkin(skin_path_));
  const std::string dict = DictDir();
  if (!dict.empty()) segmenter_ = std::make_unique<Segmenter>(dict);
  fake_=fake;
  if(fake) recognizer_=std::make_unique<FakeRecognizer>();
#ifdef VOICEIME_TESTING
  if(TestEnvironment(L"VOICEIME_TEST_CAPTURE").find(L"model-")==0) recognizer_.reset();
#endif

  if (!box_.Create(GetModuleHandleW(nullptr), &Engine::Hook, this)) return 1;
#ifndef VOICEIME_TESTING
  AddTray();
#endif
  write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  pipe_thread_ = std::thread([this] { PipeMain(); });

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (hotkey_dialog_.Translate(&message) || preview_editor_.Translate(&message) || preferences_window_.Translate(&message)) continue;
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
  if (prepare_thread_.joinable()) prepare_thread_.join();
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
  icon.hIcon = LoadProductIcon(GetModuleHandleW(nullptr),
      ShellUsesLightIcons() ? IDI_SOUND_CORE_DARK : IDI_SOUND_CORE_LIGHT,ShellIconPixels());
  const std::wstring tip = std::wstring(kProductDisplayName) + L" · Simple Voice";
  wcsncpy(icon.szTip, tip.c_str(), 127);
  const bool updated = Shell_NotifyIconW(tray_added_ ? NIM_MODIFY : NIM_ADD, &icon) == TRUE;
  tray_added_ = tray_added_ || updated;
  if(icon.hIcon)DestroyIcon(icon.hIcon);
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
#ifndef VOICEIME_TESTING
  const auto message = HotkeyChangedMessage();
  if (message) SendNotifyMessageW(HWND_BROADCAST, message, 0, 0);
#endif
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
  box_.SetActivity(preparing_?L"正在准备语音模型":queued_start_?L"正在结束上一段识别":
      session_.phase()==Phase::Listening && !capture_ready_?L"正在连接麦克风":L"");
  const std::string text = mic_error_.empty() ?
      (session_.phase()==Phase::Listening && !capture_ready_?"收到采样后即可开始说话":session_.box_text()) : mic_error_;
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
      [target](const auto& client) { return client.id == target && client.window; })) return false;
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
  for (const auto& client : clients_) if (client.window) outgoing_.emplace_back(client.id, bytes);
  if (!clients_.empty()) SetEvent(write_event_);
}

Message Engine::StateMessage() {
  const auto preview = session_.phase() == Phase::Pending ? session_.box_text() : std::string();
  if (published_phase_ != session_.phase() || published_utterance_ != utterance_generation_ || published_preview_ != preview) {
    ++state_id_;
    published_phase_ = session_.phase();
    published_utterance_ = utterance_generation_;
    published_preview_ = preview;
  }
  Message phase;
  phase.type = MsgType::Phase;
  phase.text = std::to_string(static_cast<int>(session_.phase()));
  phase.state_id = state_id_;
  return phase;
}
void Engine::PublishPhase() {
  if (session_.phase()==Phase::Idle) KillTimer(box_.hwnd(),kAnchorTimer);
  else SetTimer(box_.hwnd(),kAnchorTimer,80,nullptr);
  Broadcast(StateMessage());
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
  lexicon_->Commit(stored, static_cast<std::int64_t>(std::time(nullptr)));
  ExportTerms();
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
  if(session_.phase()==Phase::Listening) session_.OnSpeechEnded();
  if(session_.phase()!=Phase::Finalizing || finalize_thread_.joinable()) return;
  const int generation=++finalize_generation_; PublishPhase();
  final_result_.clear(); final_error_.clear();
  finalize_thread_=std::thread([this,generation] {
    try { final_result_=recognizer_?recognizer_->Finalize():std::string(); }
    catch(...) { final_error_="语音校准失败。可编辑已有草稿，或取消后重新录音。"; }
    PostMessageW(box_.hwnd(),kFinalMessage,static_cast<WPARAM>(generation),0);
  });
}

void Engine::PublishFinal(const std::string& final_text) {
  original_text_=recognizer_?recognizer_->Original():final_text;
  if(original_text_.empty()) original_text_=final_text;
  session_.OnFinal(final_text);
  if(recognizer_) { const auto warning=recognizer_->TakeWarning(); if(!warning.empty()) mic_error_=warning; }
  if(final_text.empty()) mic_error_="没有识别到文字。检查麦克风后重新录音。";
  else if(HasUnknownSpeech(final_text)) mic_error_="有内容无法识别。右键打开编辑预览，补全后再写入。";
  if(session_.phase()!=Phase::Idle) RequestCaret(); UpdateBox(); PublishPhase();
}

void Engine::StartCapture() {
  StopCapture();
  mic_error_.clear();
  vad_=Vad(VadConfig{16000,preferences_.pause_ms,60000,800});
  capture_ready_=false; input_level_=0;
  partial_posted_ = false;
  stop_capture_ = false;
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const int generation = ++capture_generation_;
  const auto device=preferences_.microphone;
  capture_thread_ = std::thread([this, generation,device] { CaptureMain(generation,device); });
}

void Engine::StopCapture() {
  stop_capture_ = true;
  if (wake_ != nullptr) SetEvent(wake_);
  if (capture_thread_.joinable()) capture_thread_.join();
  capture_ready_=false; input_level_=0;
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
  HMENU menu=CreatePopupMenu();
  if(session_.phase()==Phase::Pending && !session_.box_text().empty()) {
    AppendMenuW(menu,MF_STRING,kEditCommand,L"编辑预览…");
    AppendMenuW(menu,MF_STRING,kCopyCommand,L"复制预览");
    AppendMenuW(menu,MF_STRING,kRestoreCommand,L"恢复识别原文");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
  }
  AppendMenuW(menu,MF_STRING,kSettingsCommand,L"设置…");
  AppendMenuW(menu,MF_STRING,kTermsCommand,L"我的词语…");
  AppendMenuW(menu,MF_STRING,kGuideCommand,L"使用与诊断…");
  AppendMenuW(menu,MF_STRING,kRecordCommand,L"语音快捷键…");
  if(const auto skins=UiSkinMenu(box_.skin(),kSkinBase)) AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(skins),L"外观");
  if(!lexicon_warning_.empty()) AppendMenuW(menu,MF_STRING|MF_GRAYED,0,Utf8ToWide(lexicon_warning_).c_str());
  AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
  AppendMenuW(menu,MF_STRING,kQuitCommand,L"退出轻音输入法");
  POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
  TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,hwnd,nullptr); DestroyMenu(menu);
}

bool Engine::ExportTerms() {
  try { return lexicon_ && lexicon_->ExportTerms(data_path_+L"\\rime\\voiceime_personal.txt"); }
  catch(...) { return false; }
}
std::wstring Engine::Diagnostics() const {
  return L"Simple Voice "+Utf8ToWide(SIMPLE_VOICE_VERSION)+L" · Windows x64\r\n"+
    (preparing_?L"语音模型：正在准备":recognizer_?L"语音模型：就绪":L"语音模型：未就绪，请重试或修复安装")+
    L" · "+(recognizer_chinese_?L"中文优先":L"中英通用")+
    L"\r\n麦克风："+(capture_ready_?L"已收到采样":L"未录音 / 等待采样")+
    L"  输入电平："+std::to_wstring(input_level_.load())+L"%\r\n拼音学习："+(preferences_.learning?L"开启":L"暂停")+
    L"\r\n音频只在内存中处理；诊断不包含输入正文。";
}
void Engine::OpenPreferences(int page) {
  if(page==0 && !preferences_.guide_dismissed) page=3;
  PreferencesActions actions;
  actions.save=[this](const InputPreferences& next) {
    if(!SaveInputPreferences(preferences_path_,next)) return false;
    const bool model_changed=preferences_.chinese_only!=next.chinese_only;
    preferences_=next; if(model_changed) BeginPrepare(); return true;
  };
  actions.skin=[this](UiSkin skin) {
    if(!SaveSkin(skin_path_,skin)) return false;
    box_.SetSkin(skin);
#ifndef VOICEIME_TESTING
    SendNotifyMessageW(HWND_BROADCAST,UiSkinChangedMessage(),0,0);
#endif
    return true;
  };
  actions.hotkey=[this]{BeginRecordHotkey();};
  actions.retry_models=[this]{BeginPrepare();};
  actions.diagnostics=[this]{return Diagnostics();};
  actions.terms_changed=[this]{return ExportTerms();};
  actions.input_code=[this](const std::string& text) {
    std::string code;
    if(segmenter_) for(const auto& token:segmenter_->Cut(text)) code+=token.pinyin;
    if(code.empty()) code=text;
    std::string clean;
    for(unsigned char c:code) if(c>='A'&&c<='Z') clean+=char(c+32); else if(c>='a'&&c<='z') clean+=char(c);
    return clean;
  };
  preferences_window_.Open(lexicon_.get(),preferences_,box_.skin(),std::move(actions),page);
}
void Engine::EditPreview() {
  if(session_.phase()!=Phase::Pending || queued_start_) return;
  const auto generation=utterance_generation_;
  preview_editor_.Open(session_.box_text(),original_text_,[this,generation](std::string text){
    if(session_.phase()!=Phase::Pending || queued_start_ || generation!=utterance_generation_) return;
    // Editing cannot prove whether a previous insertion reached its document.
    session_.RestorePending(text); mic_error_.clear(); UpdateBox(); PublishPhase();
  },insert_uncertain_);
}
void Engine::BeginPrepare() {
  if(preparing_ || capture_thread_.joinable() || finalize_thread_.joinable() || session_.phase()==Phase::Listening || session_.phase()==Phase::Finalizing || session_.phase()==Phase::Committing) return;
  if(prepare_thread_.joinable()) prepare_thread_.join();
  preparing_=true; model_error_.clear(); preparing_chinese_=preferences_.chinese_only;
  recognizer_.reset();
  prepare_thread_=std::thread([this] {
#ifdef VOICEIME_TESTING
    const auto fixture=TestEnvironment(L"VOICEIME_TEST_CAPTURE");
    ++prepare_attempts_;
    if(fixture==L"model-slow" || fixture==L"model-fail-once") Sleep(500);
    if(fixture==L"model-fail-once" && prepare_attempts_==1) {
      prepared_.reset(); PostMessageW(box_.hwnd(),kPreparedMessage,0,0); return;
    }
#endif
    try { prepared_=fake_?std::make_unique<FakeRecognizer>():MakeSherpaRecognizer(ModelDir(),preparing_chinese_); }
    catch(...) { prepared_.reset(); }
    PostMessageW(box_.hwnd(),kPreparedMessage,0,0);
  });
}
bool Engine::ForegroundClientAllowed(ClientId sender) const {
  DWORD foreground = ForegroundProcessId();
#ifdef VOICEIME_TESTING
  foreground = test_foreground_;  // Fixture input; identity/window checks are unchanged.
#endif
  std::lock_guard<std::mutex> lock(write_mu_);
  for (const auto& client : clients_) {
    if (client.id != sender || !client.window || !client.peer->alive() || client.peer->id() != foreground) continue;
    DWORD owner = 0;
    return IsWindow(client.window) && GetWindowThreadProcessId(client.window, &owner) && owner == foreground;
  }
  return false;
}
void Engine::ResumeQueuedStart() {
  const auto pending = queued_start_;
  queued_start_ = 0;
  if (!pending) return;
  if (!ForegroundClientAllowed(pending)) {
    session_.Escape(); mic_error_.clear(); UpdateBox(); PublishPhase();
    return;
  }
  Message start; start.type = MsgType::StartListen; start.state_id = StateMessage().state_id;
  OnPipe(pending, start, true);
}
void Engine::OnPipe(ClientId sender, const Message& message, bool resumed) {
  // Receipts belong to the original insertion client even after focus moves.
  // New actions require the verified foreground host and the current preview.
  if (message.type != MsgType::InsertResult && !ForegroundClientAllowed(sender)) return;
  if (IsVoiceControl(message.type) && message.state_id != StateMessage().state_id) {
    // The pipe preserves command order. A quick Esc/stop after Start may
    // precede its phase reply, but must still control that same accepted Start.
    // An old Enter can never use this exception to confirm a final preview.
    const bool follows_start = sender == start_client_ && message.state_id == start_from_state_ &&
        (message.type == MsgType::Cancel ||
         ((message.type == MsgType::Enter || message.type == MsgType::StartListen) && session_.phase() == Phase::Listening));
    const bool cancels_same_utterance = message.type == MsgType::Cancel && utterance_state_floor_ &&
        message.state_id >= utterance_state_floor_ && message.state_id < state_id_;
    if (!follows_start && !cancels_same_utterance) { WriteClient(StateMessage(), sender); return; }
  }
  if(message.type==MsgType::PersonalTermUsed) {
    if(lexicon_) try { lexicon_->Commit(message.text,std::time(nullptr)); ExportTerms(); }
      catch(const std::exception& error) { LexiconWarning(error.what()); }
    return;
  }
  if (message.type == MsgType::ShowSettings) {
    OpenPreferences();
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
      for (const auto& client:clients_) if(client.id==sender) client_process=client.peer->id();
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
    queued_start_=0; ++utterance_generation_; preview_editor_.Close();
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
  // A changed preference applies to the next utterance. Handle active audio
  // before examining model selection, so a stop hotkey cannot unload a model
  // still owned by the capture/finalization thread.
  if(session_.phase()==Phase::Listening) {
    StopCapture(); session_.Hotkey(); BeginFinalize(); UpdateBox(); return;
  }
  if(session_.phase()==Phase::Finalizing || session_.phase()==Phase::Committing) return;
  // Invalidate the previous editor as soon as a new utterance is accepted,
  // including paths that wait for preparation or a cancelled finalizer.
  if (!resumed) {
    start_client_ = sender; start_from_state_ = message.state_id; utterance_state_floor_ = state_id_ + 1;
  }
  ++utterance_generation_;
  preview_editor_.Close(); original_text_.clear();
  if(preparing_ || (finalize_thread_.joinable() && session_.phase()!=Phase::Finalizing)) {
    queued_start_=sender; session_.RestorePending("");
    mic_error_=preparing_?"正在准备语音模型，请稍候。Esc 取消。":"正在结束上一段识别，请稍候。Esc 取消。";
    listening_started_=GetTickCount64(); UpdateBox(); PublishPhase(); return;
  }
  if(!recognizer_ || recognizer_chinese_!=preferences_.chinese_only) {
    queued_start_=sender; session_.RestorePending(""); mic_error_="正在准备语音模型，请稍候。Esc 取消。";
    BeginPrepare(); listening_started_=GetTickCount64(); UpdateBox(); PublishPhase(); return;
  }
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
#ifdef VOICEIME_TESTING
  if (message == WM_APP + 100) { test_foreground_ = static_cast<DWORD>(wparam); return 0; }
#endif
  static const UINT taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
  if(taskbar_created && message==taskbar_created) { tray_added_=false;AddTray();return 0; }
  if(message==WM_SETTINGCHANGE || message==WM_THEMECHANGED || message==WM_DPICHANGED || message==WM_DISPLAYCHANGE) {
    AddTray();return 0;
  }
  if (message == kClientGoneMessage) {
    if (insert_id_ != 0 && static_cast<ClientId>(wparam) == insert_client_) LoseInsertConfirmation();
    if (queued_start_ == static_cast<ClientId>(wparam)) {
      queued_start_ = 0; session_.Escape(); mic_error_.clear(); UpdateBox(); PublishPhase();
    }
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
    // Publish the handshake and its initial state together, before this peer
    // becomes eligible for broadcasts (including QueryCaret during a start).
    Message ready; ready.type = MsgType::ServerReady;
    const auto phase = StateMessage();
    const auto id = static_cast<ClientId>(wparam);
    std::lock_guard<std::mutex> lock(write_mu_);
    for (auto& client : clients_) {
      if (client.id != id || client.window) continue;
      outgoing_.emplace_back(id, Encode(ready));
      outgoing_.emplace_back(id, Encode(phase));
      client.window = reinterpret_cast<HWND>(lparam);
      SetEvent(write_event_);
      break;
    }
    return 0;
  }
  if(message==kPreparedMessage) {
    if(prepare_thread_.joinable()) prepare_thread_.join();
    preparing_=false;
    if(prepared_) { recognizer_=std::move(prepared_); recognizer_chinese_=preparing_chinese_; }
    else { model_error_="语音模型未能准备。请在设置中重试，或重新安装完整模型。"; mic_error_=model_error_; }
    if(queued_start_ && recognizer_) ResumeQueuedStart();
    else { queued_start_=0; UpdateBox(); PublishPhase(); }
    return 0;
  }
  if(message==kCaptureReadyMessage) { if(static_cast<int>(wparam)==capture_generation_) UpdateBox(); return 0; }
  if (message == kFinalMessage) {
    if(finalize_thread_.joinable()) finalize_thread_.join();
    if(static_cast<int>(wparam)==finalize_generation_ && session_.phase()==Phase::Finalizing) {
      if(final_error_.empty()) PublishFinal(final_result_);
      else { mic_error_=final_error_; session_.RestorePending(session_.box_text()); UpdateBox(); PublishPhase(); }
    }
    ResumeQueuedStart();
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
      session_.RestorePending(session_.box_text());
      UpdateBox();
      PublishPhase();
    }
    return 0;
  }
  if (message == WM_CONTEXTMENU || (message == kTrayMessage && (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONUP))) {
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
    if(id==kSettingsCommand || id==kTermsCommand || id==kGuideCommand) { OpenPreferences(id==kTermsCommand?1:id==kGuideCommand?3:0); return 0; }
    if(id==kEditCommand) { EditPreview(); return 0; }
    if(id==kCopyCommand) { CopyUserText(hwnd,Utf8ToWide(session_.box_text())); return 0; }
    if(id==kRestoreCommand && session_.phase()==Phase::Pending && !original_text_.empty()) {
      session_.RestorePending(original_text_); mic_error_=HasUnknownSpeech(original_text_)?"请编辑无法识别的内容，再确认写入。":"";
      UpdateBox(); PublishPhase(); return 0;
    }

  }
  if (message == WM_DESTROY) {
    RemoveTray();
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Engine::ClientLoop(ClientId id, HANDLE pipe, std::shared_ptr<IpcPeer> peer) {
  bool authenticated = false;
  const auto handshake_deadline = GetTickCount64() + 3000;
  PipeDeadline deadline;
#ifdef VOICEIME_TESTING
  const auto module = ExecutableDirectory() + L"\\voiceime_engine_integration.exe";
#else
  const auto module = ExecutableDirectory() + L"\\voiceime_tsf.dll";
#endif
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
  auto deliver = [this, id, &authenticated, &peer, &module](std::string& pending) {
    for (;;) {
      Message decoded;
      std::size_t used = 0;
      const auto status = DecodeFrame(pending, &decoded, &used);
      if (status == DecodeStatus::Invalid) return false;
      if (status == DecodeStatus::Incomplete) return true;
      pending.erase(0, used);
      if (!authenticated) {
        if (decoded.type != MsgType::ClientHello) return false;
        const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(decoded.client_window));
        if (!peer->owns_window(window, module)) return false;
        authenticated = true;
        PostMessageW(box_.hwnd(), kAnnounceMessage, id, reinterpret_cast<LPARAM>(window));
        continue;
      }
      if (!IsClientMessage(decoded.type)) return false;
      {
        std::lock_guard<std::mutex> lock(inbox_mu_);
        inbox_.emplace_back(id, std::move(decoded));
      }
      PostMessageW(box_.hwnd(), kPipeMessage, 0, 0);
    }
  };
  while (!stop_pipe_) {
    if (!authenticated && (!deadline.event() || GetTickCount64() >= handshake_deadline)) break;
    const std::vector<std::string> mine = take_outgoing();
    if (!mine.empty()) {
      bool failed = false;
      for (const std::string& bytes : mine) {
        if (!WritePipeBytes(pipe, bytes, stop_event_)) { failed = true; break; }
      }
      if (failed || stop_pipe_) break;
      continue;
    }
    const PipeReadResult received = ReadPipeChunk(pipe, authenticated ? write_event_ : deadline.event(), stop_event_);
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
  PipeSecurity security;
  const auto name = PipeName();
  if (!security.attributes() || name.empty()) return;
  bool first_instance = true;
  while (!stop_pipe_) {
    ReapClients();
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                    (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, security.attributes());
    if (pipe == INVALID_HANDLE_VALUE) {
      if (stop_pipe_) return;
      Sleep(100);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      listen_pipe_ = pipe;
    }
    first_instance = false;
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
    auto peer = PipeClientPeer(pipe);
    if (!peer) { DisconnectNamedPipe(pipe); CloseHandle(pipe); continue; }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      clients_.push_back({id, pipe, peer, nullptr});
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    client_threads_.push_back({std::thread([this, id, pipe, finished, peer] {
      ClientLoop(id, pipe, peer); finished->store(true);
    }), finished});
  }
}

void Engine::CaptureMain(int generation,std::wstring microphone) {
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
    if(!capture_ready_.exchange(true)) PostMessageW(box_.hwnd(),kCaptureReadyMessage,generation,0);
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
  } else if (FAILED(microphone.empty()?enumerator->GetDefaultAudioEndpoint(eCapture,eConsole,&device):
      enumerator->GetDevice(microphone.c_str(),&device)) || device == nullptr) {
    fail("麦克风不可用。请在设置中选择可用设备，或重新连接后重试。");
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
    if (format->nSamplesPerSec == 0 || format->nSamplesPerSec > 384000 ||
        (IsFloatFormat(format)?format->wBitsPerSample!=32:(format->wBitsPerSample!=16 && format->wBitsPerSample!=32))) {
      fail("麦克风采样格式暂不支持，请更换输入设备。");
    } else resampler = std::make_unique<Resampler16k>(format->nSamplesPerSec);
  }
  CaptureHealth health(GetTickCount64());
  while (started && !stop_capture_) {
    const DWORD waited = WaitForSingleObject(wake_, 200);
    if (stop_capture_) break;
    if(const char* error=health.Error(GetTickCount64())) { fail(error); break; }
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
      health.Push(samples.data(),samples.size(),GetTickCount64());
      input_level_=health.level();
      if(health.received() && !capture_ready_.exchange(true)) PostMessageW(box_.hwnd(),kCaptureReadyMessage,generation,0);
      if(const char* error=health.Error(GetTickCount64())) { fail(error); break; }
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
      std::printf("%s %s\n", kProductName, SIMPLE_VOICE_VERSION);
      return 0;
    }
    if (std::strcmp(argv[i], "--check") == 0) return CheckInstallation();
    if (std::strcmp(argv[i], "--check-chinese") == 0) return CheckInstallation(true);
    if (std::strcmp(argv[i], "--fake") == 0) fake = true;
  }
  Engine engine;
  return engine.Run(fake);
}
