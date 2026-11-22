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
#include "worker_recognizer.hpp"
#include "model_download.hpp"
#include "data_backup.hpp"
#include "recording.hpp"
#include "capture_health.hpp"
#include "capture_buffer.hpp"

#include "ipc.hpp"
#include "pinyin_client.hpp"
#include "rime_session.hpp"
#include "pipe_io.hpp"
#include "ipc_security.hpp"
#include "pinyin_handover.hpp"

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
#include <filesystem>
#include <optional>
#ifdef VOICEIME_TESTING
#include <fstream>
#endif

namespace {

constexpr UINT kPipeMessage = WM_APP + 1;
constexpr UINT kPartialMessage = WM_APP + 2;
constexpr UINT kCaptureLimitMessage = WM_APP + 3;
constexpr UINT kFinalMessage = WM_APP + 5;
constexpr UINT kAnnounceMessage = WM_APP + 6;
constexpr UINT kCaptureErrorMessage = WM_APP + 7;
constexpr UINT kClientGoneMessage = WM_APP + 8;
constexpr UINT kPreparedMessage = WM_APP + 9;
constexpr UINT kCaptureReadyMessage = WM_APP + 10;
constexpr UINT kDownloadedMessage = WM_APP + 11;
constexpr UINT kDeviceErrorMessage = WM_APP + 12;
constexpr UINT_PTR kInsertTimer = 1;
constexpr UINT_PTR kAnchorTimer = 2;
constexpr UINT_PTR kModelIdleTimer = 3;
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

bool PinyinWaitingForRestart() {
#ifdef VOICEIME_TESTING
  const auto path = TestEnvironment(L"VOICEIME_TEST_PINYIN_HANDOVER");
  return !path.empty() && PinyinHandoverPending(HKEY_CURRENT_USER, path.c_str());
#else
  return PinyinHandoverPending(HKEY_LOCAL_MACHINE, kPinyinHandoverKey);
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
  ModelSettings choice;if(chinese_only)choice.model=SpeechModel::ChineseLegacy;
  std::string error;auto recognizer = MakeWorkerRecognizer(ModelDir(),choice,&error);
  if (!recognizer) {std::fprintf(stderr,"%s\n",error.c_str());return 2;}
  std::printf("%s %s: dictionaries and %s speech models loaded.\n", kProductName, SIMPLE_VOICE_VERSION, chinese_only?"Chinese-preferred":"bilingual");
  std::puts("No microphone, input-method registration or personal data was accessed.");
  return 0;
}

class FakeRecognizer : public Recognizer {
 public:
  bool AcceptAudio(const std::int16_t*, std::size_t count) override {
#ifdef VOICEIME_TESTING
    if(TestEnvironment(L"VOICEIME_TEST_CAPTURE")==L"audio-hang" && audio_calls_++>0)
      while(!cancelled_)Sleep(5);
#endif
    std::lock_guard<std::mutex> lock(mu_);
    samples_ += count;
    return count!=0;
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
#ifdef VOICEIME_TESTING
    if(TestEnvironment(L"VOICEIME_TEST_CAPTURE")==L"reset-error")throw std::runtime_error("测试：模型重置失败");
    if(TestEnvironment(L"VOICEIME_TEST_CAPTURE")==L"reset-hang") {
      while(!cancelled_)Sleep(5);
      throw std::runtime_error("测试：模型重置已取消");
    }
#endif
    std::lock_guard<std::mutex> lock(mu_);
    samples_ = 0;
  }
  void Cancel() override {cancelled_=true;}

 private:
  mutable std::mutex mu_;
  std::size_t samples_ = 0;
  std::atomic<bool> cancelled_{false};
#ifdef VOICEIME_TESTING
  int audio_calls_=0;
#endif
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
  DWORD ForegroundInputProcess() const;
  bool ForegroundClientAllowed(ClientId sender) const;
  void ResumeQueuedStart();
  Message StateMessage();
  void ShowMenu(HWND hwnd);
  void OpenPreferences(int page=0);
  void BeginPrepare(const ModelSettings* requested=nullptr,bool activate=false,bool for_capture=false);
  void PreloadModelIfIdle();
  void ReleaseIdleModel(ULONGLONG now);
  std::string ModelRoot(const ModelSettings& settings) const;
  ModelSettings ModelChoice(ModelSettings settings) const;
  ModelActionResult UseModel(const ModelSettings& settings,bool allow_download=true);
  std::wstring DownloadChoice(const ModelSettings& settings);
  std::wstring ModelStatus(SpeechModel model) const;
  bool ExportTerms();
  std::wstring Diagnostics() const;
  std::wstring Practice(int action);
  std::wstring PracticeStatus() const;
  std::wstring RestoreBackup(const std::wstring& folder);
  void EditPreview();
  void OnPartial();
  void OnCaptureLimit();
  void StartRecording(ClientId origin);
  void StartBufferedDecode();
  bool ResetDecoder(int generation);
  bool FeedDecoder(const std::int16_t* samples,std::size_t count,int generation);
  void DiscardCapture();
  void CancelRecording();
  void StopCapture();
  void UpdateBox();
  void CaptureMain(int generation,std::wstring microphone,HANDLE wake,CaptureBuffer& buffer);
  bool AcceptCaptureAudio(const std::int16_t* samples,std::size_t count,int generation,
                          CaptureHealth& health,RecordingBudget& budget,CaptureBuffer& buffer,bool tail=false);
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
  void StartFinalizer();
  void PublishFinal(const std::string& final_text);

  ConfirmBox box_;
  Session session_;
  std::unique_ptr<Recognizer> recognizer_;
  std::unique_ptr<Lexicon> lexicon_;
  std::thread capture_thread_;
  std::thread buffered_decode_thread_;
  std::shared_ptr<CaptureBuffer> buffered_audio_;
  bool waiting_for_model_=false, retry_cancelled_load_=false;
  std::optional<ModelSettings> requested_capture_model_;
  ClientId buffering_client_=0;
  std::wstring stopped_notice_;
  std::thread pipe_thread_;
  std::thread finalize_thread_;
  std::thread prepare_thread_;
  std::thread download_thread_;
  std::unique_ptr<Recognizer> prepared_;
  std::string final_result_, final_error_, original_text_;
  bool preparing_=false, fake_=false;
  bool preload_on_activate_=true, preloading_=false;
  bool activating_model_=false,downloading_=false,download_ok_=false;
  ModelSettings models_,active_models_,loading_models_,downloading_models_;
  std::optional<ModelSettings> deferred_models_;
  SpeechModel notice_model_=SpeechModel::Qwen3;
  std::atomic<bool> prepare_cancel_{false},download_cancel_{false};
  std::string prepared_error_;
  std::wstring download_result_,model_notice_;
  ULONGLONG prepare_started_=0,model_last_used_=0,final_started_=0,last_load_ms_=0,last_final_ms_=0;
#ifdef VOICEIME_TESTING
  int prepare_attempts_=0;
  std::atomic<DWORD> test_foreground_{0};
#endif
  std::string model_error_;
  ClientId queued_start_=0;
  std::atomic<bool> capture_ready_{false};
  std::atomic<int> input_level_{0};
  std::atomic<std::size_t> recorded_samples_{0};
  std::atomic<bool> quiet_capture_{false};
  InputPreferences preferences_;
  std::wstring data_path_, preferences_path_;
  std::wstring model_settings_path_, speech_startup_path_;
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
  std::atomic<bool> finish_capture_{false};
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
  bool practice_=false;
  std::wstring practice_result_;

  static LRESULT Hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* self) {
    return static_cast<Engine*>(self)->OnMessage(hwnd, message, wparam, lparam);
  }

};

int Engine::Run(bool fake) {
#ifdef VOICEIME_TESTING
  if (TestEnvironment(L"VOICEIME_TEST_ID").empty() || TestEnvironment(L"VOICEIME_TEST_DATA").empty()) return 3;
  fake = true;
  test_foreground_ = std::wcstoul(TestEnvironment(L"VOICEIME_TEST_FOREGROUND").c_str(), nullptr, 10);
#else
  const auto registered=RegisteredVoiceDll();
  if(!registered.empty() && !SameFile(registered,ExecutableDirectory()+L"\\voiceime_tsf.dll")) {
    MessageBoxW(nullptr,L"此文件夹不是当前安装版本。请从当前安装目录启动，或运行新版本的 install.cmd 完成升级后注销并重新登录。",L"轻音输入法 · 版本已更换",MB_OK|MB_ICONINFORMATION);
    return 5;
  }
  if(LegacyVoiceEngineRunning()) {
    MessageBoxW(nullptr,L"旧版本仍在本次 Windows 会话中运行。请保存工作，注销 Windows 后重新登录，以完成升级并避免重复托盘图标。",L"轻音输入法 · 需要重新登录",MB_OK|MB_ICONINFORMATION);
    return 6;
  }
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
  try {if(lexicon_)RecoverDataRestore(*lexicon_,data);}
  catch(const std::exception& e) {std::fprintf(stderr,"restore recovery: %s\n",e.what());return 4;}
  data_path_=data; preferences_path_=data+L"\\preferences.ini";
  preferences_=LoadInputPreferences(preferences_path_);
  model_settings_path_=data+L"\\speech-models.ini";
  models_=LoadModelSettings(model_settings_path_,preferences_.chinese_only);
  // The existing model file is also the strict worker wire format used by rc.9.
  speech_startup_path_=data+L"\\speech-startup.ini";
  preload_on_activate_=GetPrivateProfileIntW(L"speech",L"preload_on_activate",1,speech_startup_path_.c_str())!=0;
  ExportTerms();
  config_path_ = WideToUtf8(data + L"\\config.json");
  settings_ = LoadSettings(config_path_);
  skin_path_ = data + L"\\appearance.json";
  box_.SetSkin(LoadSkin(skin_path_));
  fake_=fake;
  if(fake) recognizer_=std::make_unique<FakeRecognizer>();
#ifdef VOICEIME_TESTING
  if(TestEnvironment(L"VOICEIME_TEST_CAPTURE").find(L"model-")==0) recognizer_.reset();
#endif

  if (!box_.Create(GetModuleHandleW(nullptr), &Engine::Hook, this)) return 1;
  SetTimer(box_.hwnd(),kModelIdleTimer,10000,nullptr);
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
  prepare_cancel_=true;download_cancel_=true;
  if(recognizer_)recognizer_->Cancel();
  CloseRecord();
  stop_pipe_ = true;
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  if (write_event_ != nullptr) SetEvent(write_event_);
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    if (listen_pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(listen_pipe_, nullptr);
    for (const auto& client : clients_) CancelIoEx(client.pipe, nullptr);
  }
  preferences_window_.Close();
  DiscardCapture();
  if (finalize_thread_.joinable()) finalize_thread_.join();
  if (prepare_thread_.joinable()) prepare_thread_.join();
  if (download_thread_.joinable()) download_thread_.join();
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
  std::wstring activity;
  if (queued_start_) activity=L"正在结束上一段识别";
  else if (session_.phase()==Phase::Stopped) activity=stopped_notice_;
  else if (session_.phase()==Phase::Listening && !capture_ready_) activity=L"正在连接麦克风";
  else if (waiting_for_model_) activity=session_.phase()==Phase::Finalizing?
      L"已停录 · 等待语音模型":L"正在收音 · 模型准备中";
  box_.SetActivity(std::move(activity));
  box_.SetRecordingProgress(static_cast<int>(recorded_samples_/kRecordingSampleRate),quiet_capture_);
  std::string text=mic_error_.empty()?session_.box_text():mic_error_;
  if (text.empty() && session_.phase()==Phase::Listening) {
    if (!capture_ready_) text="收到采样后即可开始说话";
    else if (waiting_for_model_) text="可以继续说话，模型就绪后会识别已收到的语音。";
  }
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
  if(session_.can_finalize()) session_.OnSpeechEnded();
  if(session_.phase()!=Phase::Finalizing || finalize_thread_.joinable()) return;
  buffering_client_=0; // An explicitly stopped recording may be confirmed from another app.
  finish_capture_=true;
  stop_capture_=true;
  capture_ready_=false;input_level_=0;
  if(wake_)SetEvent(wake_);
  final_started_=GetTickCount64();
  PublishPhase();
  // Capture exits and seals the FIFO even when the model is still loading.
  // The prepare completion resumes this finalization without reopening the mic.
  if(!waiting_for_model_) StartFinalizer();
}

void Engine::StartFinalizer() {
  // The finalizer owns the capture join and wake handle. A slow model request
  // must not prevent the UI thread from receiving Esc and terminating it.
  auto capture=std::move(capture_thread_);const auto wake=wake_;wake_=nullptr;
  const int generation=++finalize_generation_;
  final_result_.clear(); final_error_.clear();
  finalize_thread_=std::thread([this,generation,wake,buffer=buffered_audio_,capture=std::move(capture),
                                decoder=std::move(buffered_decode_thread_)]() mutable {
    if(capture.joinable())capture.join();
    if(wake)CloseHandle(wake);
    if(decoder.joinable())decoder.join();
    capture_ready_=false;input_level_=0;
    try { if(recognizer_ && (!buffer || !buffer->discarded())) final_result_=recognizer_->Finalize(); }
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

void Engine::StartRecording(ClientId origin) {
  StopCapture();
  const bool need_model=preparing_ || deferred_models_ || !recognizer_ || !recognizer_->Healthy() ||
      !SameModelRuntime(active_models_,models_);
  requested_capture_model_=deferred_models_;
  waiting_for_model_=need_model;
  retry_cancelled_load_=preparing_ && prepare_cancel_;
  buffering_client_=need_model?origin:0;
  buffered_audio_=std::make_shared<CaptureBuffer>();
  stopped_notice_.clear();
  model_last_used_=listening_started_=GetTickCount64();
  caret_known_=false;caret_process_=0;insert_uncertain_=false;
  session_.Hotkey();
  mic_error_.clear();
  recorded_samples_=0; quiet_capture_=false;
  capture_ready_=false; input_level_=0;
  partial_posted_ = false;
  finish_capture_=false;
  stop_capture_ = false;
  wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  const int generation = ++capture_generation_;
  const auto device=preferences_.microphone;
  const auto wake=wake_;
  capture_thread_ = std::thread([this,generation,device,wake,buffer=buffered_audio_] {
    CaptureMain(generation,device,wake,*buffer);
    buffer->Close();
  });
  if(!need_model) {
    StartBufferedDecode();
  } else if(!preparing_) {
    const auto requested=std::move(requested_capture_model_);
    requested_capture_model_.reset();
    BeginPrepare(requested?&*requested:nullptr,requested.has_value(),true);
  }
}

void Engine::StartBufferedDecode() {
  waiting_for_model_=false;
  buffering_client_=0;
  buffered_decode_thread_=std::thread([this,generation=capture_generation_,buffer=buffered_audio_] {
    if(!ResetDecoder(generation)) {buffer->Discard(); return;}
    std::vector<std::int16_t> block;
    while(buffer->Read(block)) {
      if(!FeedDecoder(block.data(),block.size(),generation)) {buffer->Discard();return;}
    }
    if(!buffer->discarded()) PostMessageW(box_.hwnd(),kPartialMessage,generation,1);
  });
  if(session_.phase()==Phase::Finalizing) StartFinalizer();
}

void Engine::DiscardCapture() {
  finish_capture_=false;
  if(buffered_audio_) buffered_audio_->Discard();
  if(!waiting_for_model_ && recognizer_ && (session_.can_finalize() || session_.phase()==Phase::Finalizing))
    recognizer_->Cancel();
  StopCapture();
  if(buffered_decode_thread_.joinable()) buffered_decode_thread_.join();
  buffered_audio_.reset();
  requested_capture_model_.reset();
  buffering_client_=0;
  waiting_for_model_=retry_cancelled_load_=false;
}

void Engine::CancelRecording() {
  ++capture_generation_; ++finalize_generation_; ++utterance_generation_;
  if(waiting_for_model_ && !preloading_ && !activating_model_) prepare_cancel_=true;
  DiscardCapture();
  queued_start_=0; preview_editor_.Close();
  session_.Escape();
  pending_insert_.clear();insert_id_=0;insert_uncertain_=false;
  mic_error_.clear();caret_known_=false;
  UpdateBox();PublishPhase();
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
}

void Engine::OnPartial() {
  partial_posted_ = false;
  if (waiting_for_model_ || session_.phase()!=Phase::Listening || !recognizer_) return;
  session_.OnDraft(recognizer_->Partial());
  UpdateBox();
}

void Engine::OnCaptureLimit() {
  if (session_.phase() != Phase::Listening) return;
  StopCapture();
  if(!waiting_for_model_ && recognizer_) session_.OnDraft(recognizer_->Partial());
  session_.OnCaptureLimit();
  UpdateBox(); PublishPhase();
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
    (preloading_?L"语音模型：正在后台预加载":preparing_?L"语音模型：正在准备":
     recognizer_ && recognizer_->FinalReady()?L"语音模型：草稿和定稿就绪":
     recognizer_ && recognizer_->Healthy()?L"语音模型：草稿就绪，正在准备定稿":L"语音模型：尚未就绪")+
    L" · "+SpeechModelLabel(models_.model)+
    L"\r\n麦克风："+(capture_ready_?L"已收到采样":L"未录音 / 等待采样")+
    L"  输入电平："+std::to_wstring(input_level_.load())+L"%\r\n拼音学习："+(preferences_.learning?L"开启":L"暂停")+
    L"\r\n上次模型可用耗时："+std::to_wstring(last_load_ms_)+L" ms；停录后校准："+std::to_wstring(last_final_ms_)+L" ms"+
    L"\r\n"+model_notice_+
    L"\r\n音频只在内存中处理；诊断不包含输入正文或提示词。";
}
void Engine::OpenPreferences(int page) {
  if(page==0 && !preferences_.guide_dismissed) page=3;
  PreferencesActions actions;
  actions.save=[this](const InputPreferences& next) {
    if(!SaveInputPreferences(preferences_path_,next)) return false;
    const bool model_changed=preferences_.chinese_only!=next.chinese_only;
    preferences_=next;
    if(model_changed) {
      auto choice=models_;choice.model=next.chinese_only?SpeechModel::ChineseLegacy:SpeechModel::Qwen3;
      BeginPrepare(&choice,true);
    }
    return true;
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
  actions.hotkey_label=[this]{return HotkeyLabel(settings_.hotkey);};
  actions.retry_models=[this]{BeginPrepare();};
  actions.models=[this]{return models_;};
  actions.preload_on_activate=preload_on_activate_;
  actions.save_preload_on_activate=[this](bool enabled) {
    if(!WritePrivateProfileStringW(L"speech",L"preload_on_activate",enabled?L"1":L"0",speech_startup_path_.c_str())) return false;
    preload_on_activate_=enabled;
    return true;
  };
  actions.save_model_idle_release=[this](bool enabled) {
    auto next=models_;
    next.idle_minutes=enabled?ModelSettings::kDefaultIdleMinutes:0;
    if(!SaveModelSettings(model_settings_path_,next))return false;
    models_.idle_minutes=next.idle_minutes;
    return true;
  };
  actions.model_status=[this](SpeechModel model){return ModelStatus(model);};
  actions.model_task=[this]{return preparing_ || downloading_?ModelTaskState::Running:
      deferred_models_?ModelTaskState::Queued:ModelTaskState::Idle;};
  actions.use_model=[this](const ModelSettings& settings){return UseModel(settings);};
  actions.cancel_model_operation=[this]{
    prepare_cancel_=true;download_cancel_=true;deferred_models_.reset();
    if(waiting_for_model_ || queued_start_) {
      if(practice_) Practice(2);
      else CancelRecording();
    }
    model_notice_=L"已取消模型操作，原方案保留。";
  };
  actions.diagnostics=[this]{return Diagnostics();};
  actions.practice=[this](int action){return Practice(action);};
  actions.practice_status=[this]{return PracticeStatus();};
  actions.backup=[this](const std::wstring& folder)->std::wstring {
    if(!lexicon_)return L"个人词库不可用，暂时无法创建备份。";
    try {return L"备份已保存："+CreateDataBackup(*lexicon_,data_path_,folder);}
    catch(const std::exception& e){return Utf8ToWide(e.what());}
  };
  actions.restore=[this](const std::wstring& folder){return RestoreBackup(folder);};
  actions.terms_changed=[this]{return ExportTerms();};
  // Reuse Jieba while editing terms. The settings window owns this callback.
  actions.input_code=[segmenter=std::shared_ptr<Segmenter>{}](const std::string& text) mutable {
    if (!segmenter && !text.empty()) {
      const auto dict = DictDir();
      if (!dict.empty()) segmenter = std::make_shared<Segmenter>(dict);
    }
    std::string code;
    if(segmenter) for(const auto& token:segmenter->Cut(text)) code+=token.pinyin;
    if(code.empty()) code=text;
    std::string clean;
    for(unsigned char c:code) if(c>='A'&&c<='Z') clean+=char(c+32); else if(c>='a'&&c<='z') clean+=char(c);
    return clean;
  };
  preferences_window_.Open(lexicon_.get(),preferences_,box_.skin(),std::move(actions),page);
}
std::wstring Engine::RestoreBackup(const std::wstring& folder) {
    if(!lexicon_)return L"个人词库不可用，暂时无法恢复。";
    if(session_.phase()!=Phase::Idle || preparing_)return L"请先确认或取消当前语音，再恢复备份。";
    try {
      // Old preferences contain chinese_only. Pin the current model before
      // restoring those bytes so a restart cannot re-import a legacy choice.
      if(!SaveModelSettings(model_settings_path_,models_))return L"无法保存当前模型选择；未恢复数据，请检查数据目录后重试。";
      RestoreDataBackup(*lexicon_,data_path_,folder);
      preferences_=LoadInputPreferences(preferences_path_);settings_=LoadSettings(config_path_);
      const auto skin=LoadSkin(skin_path_);box_.SetSkin(skin);
      preferences_window_.ReloadInputs(preferences_,skin);
#ifndef VOICEIME_TESTING
      SendNotifyMessageW(HWND_BROADCAST,HotkeyChangedMessage(),0,0);
      SendNotifyMessageW(HWND_BROADCAST,UiSkinChangedMessage(),0,0);
#endif
      if(!ExportTerms())return L"数据已恢复；个人词语同步失败，请重启后重试。旧数据保存在数据目录的 backups 文件夹。";
      return L"词语与设置已恢复。原数据保存在数据目录的 backups 文件夹。";
    } catch(const std::exception& e){return Utf8ToWide(e.what());}
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
std::string Engine::ModelRoot(const ModelSettings& settings) const {
#ifdef VOICEIME_TESTING
  if(!TestEnvironment(L"VOICEIME_TEST_DOWNLOAD_PROGRAM").empty())return WideToUtf8(data_path_+L"\\models");
#endif
  const auto packaged=ModelDir();
  for(const auto& root:{packaged,WideToUtf8(data_path_+L"\\models")}) {
    ModelFiles files;std::string error;
    if(!root.empty() && ResolveModelFiles(root,settings,files,error))return root;
  }
  return packaged;
}
ModelSettings Engine::ModelChoice(ModelSettings settings) const {
  const auto i=static_cast<int>(settings.model);
  if(settings.directories[i].empty()) {
    const auto managed=data_path_+L"\\models\\"+Utf8ToWide(SpeechModelFolder(settings.model));
    ModelFiles files;std::string error;auto candidate=settings;candidate.directories[i]=managed;
    if(ResolveModelFiles(ModelRoot(candidate),candidate,files,error))settings=std::move(candidate);
  }
  return settings;
}
std::wstring Engine::Practice(int action) {
  if(action==2) {
    if(!practice_)return L"试用已结束。";
    practice_=false;practice_result_=L"已取消试用。";
    CancelRecording();return practice_result_;
  }
  if(action==1) {
    if(!practice_ || session_.phase()!=Phase::Pending || session_.box_text().empty())return L"请等待最终预览后再确认。";
    if(!mic_error_.empty())return L"请先编辑并核对预览中的异常内容，或取消后重新试用。";
    session_.Enter();std::string text;
    if(!session_.TakeCommit(&text))return L"文字尚未准备好。";
    session_.ConfirmInserted();std::string ignored;session_.TakeLexiconWrite(&ignored);
    practice_=false;practice_result_=L"已确认（仅本页）：\r\n"+Utf8ToWide(text);mic_error_.clear();
    UpdateBox();PublishPhase();return L"试用完成。日常输入时，确认文字会写入当前应用。";
  }
  if(practice_ && session_.can_finalize()) {BeginFinalize();UpdateBox();return L"正在校准，请等待最终预览。";}
  if(session_.phase()!=Phase::Idle || finalize_thread_.joinable())return L"请先完成或取消当前语音。";
  practice_=true;practice_result_.clear();++utterance_generation_;
  StartRecording(0);UpdateBox();PublishPhase();return L"收到麦克风采样后开始说话。再次点击可停录预览。";
}
std::wstring Engine::PracticeStatus() const {
  if(!practice_)return practice_result_.empty()?L"点击开始后才会打开麦克风；试用结果仅显示在本页。":practice_result_;
  const auto text=Utf8ToWide(session_.box_text());
  if(!mic_error_.empty())return Utf8ToWide(mic_error_)+L"\r\n"+text;
  if(session_.phase()==Phase::Listening) return (!capture_ready_?L"正在连接麦克风…\r\n":
      waiting_for_model_?L"正在收音 · 模型准备中\r\n":L"正在听…\r\n")+text;
  if(session_.phase()==Phase::Finalizing)return (waiting_for_model_?L"已停录 · 等待语音模型\r\n":L"正在校准，请稍候…\r\n")+text;
  if(session_.phase()==Phase::Stopped)return (stopped_notice_.empty()?L"已达到 10 分钟上限":stopped_notice_)+L"。点击“开始 / 停止试用”后校准。\r\n"+text;
  return L"最终预览；确认后只显示在本页：\r\n"+text;
}
ModelActionResult Engine::UseModel(const ModelSettings& choice,bool allow_download) {
  if(preparing_ || downloading_)return {L"已有模型任务正在进行，请等待或取消任务。"};
  auto next=ModelChoice(choice);
  next.idle_minutes=models_.idle_minutes;
  ModelFiles files;std::string error;
  bool require_files=!fake_;
#ifdef VOICEIME_TESTING
  require_files|=!TestEnvironment(L"VOICEIME_TEST_DOWNLOAD_PROGRAM").empty();
#endif
  if(require_files && !ResolveModelFiles(ModelRoot(next),next,files,error)) {
    const auto index=static_cast<int>(choice.model);
    const auto managed=data_path_+L"\\models\\"+Utf8ToWide(SpeechModelFolder(choice.model));
    const bool managed_path=CompareStringOrdinal(choice.directories[index].c_str(),-1,managed.c_str(),-1,TRUE)==CSTR_EQUAL;
    if(allow_download && (choice.directories[index].empty() || managed_path)) {
      auto download=choice; download.directories[index].clear();
      auto message=DownloadChoice(download);
      return {std::move(message),downloading_};
    }
    return {Utf8ToWide(error)};
  }
  if(session_.phase()!=Phase::Idle || capture_thread_.joinable() || finalize_thread_.joinable()) {
    deferred_models_=next;notice_model_=next.model;model_notice_=L"已选择；下一次录音前验证并切换。当前录音保持原模型。";
    return {model_notice_,true};
  }
  if(recognizer_ && recognizer_->Healthy() && SameModelRuntime(active_models_,next)) {
    if(!recognizer_->FinalReady())return {L"草稿已就绪，正在准备定稿模型。"};
    if(!SaveModelSettings(model_settings_path_,next))return {L"设置保存失败，原方案保留。"};
    models_=next;deferred_models_.reset();return {L"已保存，此模型已准备。"};
  }
  BeginPrepare(&next,true);
  if(!preparing_) return {L"当前无法开始模型验证，请稍后重试。"};
  return {L"正在验证模型。成功后启用；失败保留原方案。",true};
}
std::wstring Engine::ModelStatus(SpeechModel model) const {
  if(deferred_models_ && deferred_models_->model==model)return L"已选择 · 下一次录音前验证并切换。";
  if(downloading_ && downloading_models_.model==model)return L"下载中\r\n"+ModelDownloadStatus(data_path_);
  if(preparing_ && loading_models_.model==model)return preloading_?L"正在后台预加载语音模型。":L"正在验证模型，请稍候。原有方案保留。";
  const std::wstring notice=model==notice_model_ && !model_notice_.empty()?L"\r\n"+model_notice_:L"";
  if(model==models_.model) {
    return std::wstring(L"使用中 · ")+(recognizer_ && recognizer_->FinalReady()?L"已准备":
        recognizer_ && recognizer_->Healthy()?L"草稿就绪，正在准备定稿模型":
        preload_on_activate_?L"切入轻音或录音时准备":L"下次录音时准备")+
        notice;
  }
  auto candidate=models_;candidate.model=model;candidate=ModelChoice(candidate);
  ModelFiles files;std::string error;
  return (ResolveModelFiles(ModelRoot(candidate),candidate,files,error)?std::wstring(L"可用 · 点击“启用所选模型”完成验证并切换。"):
      std::wstring(L"未安装或文件不完整。点击“启用所选模型”会自动下载，也可选择已有文件。"))+notice;
}
std::wstring Engine::DownloadChoice(const ModelSettings& settings) {
  if(downloading_)return L"已有下载正在进行，请等待或取消任务。";
  if(download_thread_.joinable())download_thread_.join();
  downloading_=true;downloading_models_=settings;download_cancel_=false;deferred_models_.reset();
  download_result_.clear();download_ok_=false;
  auto program=ExecutableDirectory();const auto data=data_path_;const auto model=settings.model;
#ifdef VOICEIME_TESTING
  const auto fixture=TestEnvironment(L"VOICEIME_TEST_DOWNLOAD_PROGRAM");if(!fixture.empty())program=fixture;
#endif
  download_thread_=std::thread([this,program,data,model]{
    try {download_ok_=DownloadModel(program,data,model,download_cancel_,download_result_);}
    catch(...) {download_result_=L"下载未能完成，请重试。";}
    PostMessageW(box_.hwnd(),kDownloadedMessage,0,0);
  });
  return L"下载已开始。已有模型仍可使用。";
}
void Engine::PreloadModelIfIdle() {
  if (!preload_on_activate_ || preparing_ || downloading_ || deferred_models_ ||
      session_.phase()!=Phase::Idle || (recognizer_ && recognizer_->Healthy())) return;
  // Switching hosts repeatedly should not continually retry a missing model.
  // An explicit voice hotkey can always retry immediately.
  if (!model_error_.empty() && GetTickCount64()-prepare_started_<30000) return;
  BeginPrepare();
  preloading_=preparing_;
}

void Engine::BeginPrepare(const ModelSettings* requested,bool activate,bool for_capture) {
  if(preparing_ || finalize_thread_.joinable() || session_.phase()==Phase::Committing) return;
  if(!for_capture && (capture_thread_.joinable() || session_.can_finalize() || session_.phase()==Phase::Finalizing)) return;
  if(prepare_thread_.joinable()) prepare_thread_.join();
  preparing_=true; model_error_.clear();prepared_error_.clear();prepare_cancel_=false;
  loading_models_=ModelChoice(requested?*requested:models_);activating_model_=activate;
  notice_model_=loading_models_.model;deferred_models_.reset();
  prepare_started_=GetTickCount64();
  if(recognizer_ && !recognizer_->Healthy())recognizer_.reset();
#ifdef VOICEIME_TESTING
  ++prepare_attempts_;
#endif
  const auto startup=activate?ModelStartup::Full:ModelStartup::DraftFirst;
  prepare_thread_=std::thread([this,startup] {
    bool use_fake=fake_;
#ifdef VOICEIME_TESTING
    const auto fixture=TestEnvironment(L"VOICEIME_TEST_CAPTURE");
    if(fixture==L"model-real" || fixture==L"model-cold-pcm") use_fake=false;
    if(fixture==L"model-buffer-limit" || fixture==L"model-buffer-disconnect" || fixture==L"model-buffer-gate") {
      const auto gate=std::filesystem::path(data_path_)/L"allow-model";
      const auto deadline=GetTickCount64()+15000;
      while(!prepare_cancel_ && !std::filesystem::exists(gate) && GetTickCount64()<deadline) Sleep(5);
    }
    if(fixture==L"model-slow" || fixture==L"model-fail-once") Sleep(500);
    if(fixture==L"model-fail-once" && prepare_attempts_==1) {
      prepared_.reset(); PostMessageW(box_.hwnd(),kPreparedMessage,0,0); return;
    }
#endif
    try { prepared_=use_fake?std::make_unique<FakeRecognizer>():
      MakeWorkerRecognizer(ModelRoot(loading_models_),loading_models_,&prepared_error_,L"",120000,&prepare_cancel_,startup); }
    catch(...) { prepared_.reset(); }
    PostMessageW(box_.hwnd(),kPreparedMessage,0,0);
  });
}
void Engine::ReleaseIdleModel(ULONGLONG now) {
  if(models_.idle_minutes>0 && recognizer_ && !preparing_ &&
     !capture_thread_.joinable() && !finalize_thread_.joinable() && session_.phase()==Phase::Idle &&
     now-model_last_used_>=static_cast<ULONGLONG>(models_.idle_minutes)*60000) {
    recognizer_.reset();model_notice_=preload_on_activate_?L"闲置模型已释放，下次切入轻音或录音时准备。":L"闲置模型已释放，下次录音时准备。";
  }
}
DWORD Engine::ForegroundInputProcess() const {
#ifdef VOICEIME_TESTING
  return test_foreground_;  // Fixture input; identity/window checks are unchanged.
#else
  return ForegroundProcessId();
#endif
}

bool IsForegroundPeer(const IpcPeer& peer, HWND window, DWORD foreground) {
  DWORD owner = 0;
  return window && peer.id() == foreground && peer.alive() &&
      GetWindowThreadProcessId(window, &owner) && owner == foreground;
}

bool Engine::ForegroundClientAllowed(ClientId sender) const {
  const DWORD foreground = ForegroundInputProcess();
  std::lock_guard<std::mutex> lock(write_mu_);
  for (const auto& client : clients_) {
    if (client.id == sender) return IsForegroundPeer(*client.peer, client.window, foreground);
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
  if(practice_ && IsVoiceControl(message.type))return;
  // Receipts belong to the original insertion client even after focus moves.
  // New actions require the verified foreground host and the current preview.
  // A pinyin receipt is bound to this connection's last generated commit in
  // ClientLoop. The user can change applications immediately after SetText.
  if (message.type != MsgType::InsertResult && message.type != MsgType::PinyinInserted &&
      !ForegroundClientAllowed(sender)) return;
  if (message.type == MsgType::PreloadModel) {
    PreloadModelIfIdle();
    return;
  }
  if (IsVoiceControl(message.type) && message.state_id != StateMessage().state_id) {
    // The pipe preserves command order. A quick Esc/stop after Start may
    // precede its phase reply, but must still control that same accepted Start.
    // An old Enter can never use this exception to confirm a final preview.
    const bool follows_start = sender == start_client_ && message.state_id == start_from_state_ &&
        (message.type == MsgType::Cancel ||
         ((message.type == MsgType::Enter || message.type == MsgType::StartListen) && session_.can_finalize()));
    const bool cancels_same_utterance = message.type == MsgType::Cancel && utterance_state_floor_ &&
        message.state_id >= utterance_state_floor_ && message.state_id < state_id_;
    // A stop pressed just as the cap is reached still means "finalize", never
    // "insert". Keep stale Enter blocked once a calibrated preview exists.
    const bool stops_at_limit = session_.phase()==Phase::Stopped && utterance_state_floor_ &&
        message.state_id>=utterance_state_floor_ && message.state_id<state_id_ &&
        (message.type==MsgType::Enter || message.type==MsgType::StartListen);
    if (!follows_start && !cancels_same_utterance && !stops_at_limit) { WriteClient(StateMessage(), sender); return; }
  }
  if(message.type==MsgType::PersonalTermUsed || message.type==MsgType::PinyinInserted) {
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
    if (session_.can_finalize()) {
      session_.Enter();
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
    CancelRecording();
    return;
  }
  if (message.type != MsgType::StartListen) return;
  // A changed preference applies to the next utterance. Handle active audio
  // before examining model selection, so a stop hotkey cannot unload a model
  // still owned by the capture/finalization thread.
  if(session_.can_finalize()) {
    session_.Hotkey(); BeginFinalize(); UpdateBox(); return;
  }
  if(session_.phase()==Phase::Finalizing || session_.phase()==Phase::Committing) return;
  // Invalidate the previous editor as soon as a new utterance is accepted,
  // including paths that wait for preparation or a cancelled finalizer.
  if (!resumed) {
    start_client_ = sender; start_from_state_ = message.state_id; utterance_state_floor_ = state_id_ + 1;
  }
  ++utterance_generation_;
  preview_editor_.Close(); original_text_.clear();
  if(finalize_thread_.joinable()) {
    queued_start_=sender; session_.RestorePending("");
    mic_error_="正在结束上一段识别，请稍候。Esc 取消。";
    listening_started_=GetTickCount64(); UpdateBox(); PublishPhase(); return;
  }
  StartRecording(sender);
  RequestCaret();
  UpdateBox();
  PublishPhase();
}

LRESULT Engine::OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
#ifdef VOICEIME_TESTING
  if (message == WM_APP + 100) { test_foreground_ = static_cast<DWORD>(wparam); return 0; }
  if (message == WM_APP + 101) return RestoreBackup(data_path_+L"\\restore-fixture").find(L"词语与设置已恢复")==0;
  if (message == WM_APP + 102) { ReleaseIdleModel(model_last_used_+wparam); return 0; }
  if (message == WM_APP + 103) return recognizer_ && recognizer_->FinalReady();
  if (message == WM_APP + 104) return prepare_attempts_;
  if (message == WM_APP + 105) return static_cast<LRESULT>(recorded_samples_.load());
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
    if (buffering_client_==static_cast<ClientId>(wparam) && buffering_client_) CancelRecording();
    return 0;
  }
  if (message == WM_TIMER && wparam == kInsertTimer) { LoseInsertConfirmation(); return 0; }
  if (message == WM_TIMER && wparam == kAnchorTimer) {
    if(buffering_client_ && !ForegroundClientAllowed(buffering_client_)) CancelRecording();
    else UpdateBox();
    return 0;
  }
  if(message==WM_TIMER && wparam==kModelIdleTimer) {
    if(!fake_) ReleaseIdleModel(GetTickCount64());
    return 0;
  }
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
    const bool quiet_preload=preloading_ && !waiting_for_model_;
    preloading_=false;
    last_load_ms_=GetTickCount64()-prepare_started_;
    if(prepare_cancel_) {prepared_.reset();prepared_error_="模型准备已取消，原方案保留。";}
    // Model preparation can finish after the idle preference has changed.
    loading_models_.idle_minutes=models_.idle_minutes;
    if(prepared_ && activating_model_ && !SaveModelSettings(model_settings_path_,loading_models_)) {
      prepared_.reset();prepared_error_="模型已验证，但设置保存失败。原方案保留，请检查数据目录后重试。";
    }
    if(prepared_) {
      recognizer_=std::move(prepared_);
      active_models_=loading_models_;models_=loading_models_;model_last_used_=GetTickCount64();
      if(recognizer_->FinalReady())
        model_notice_=quiet_preload?L"语音模型已预加载。":L"模型已验证并启用。";
      else model_notice_.clear();
      if(session_.phase()==Phase::Idle || waiting_for_model_)mic_error_.clear();
    } else {
      model_error_=prepared_error_.empty()?"语音模型未能准备，请重试或重新下载。":prepared_error_;
      model_notice_=Utf8ToWide(model_error_);
      if(!quiet_preload && (waiting_for_model_ || session_.phase()==Phase::Idle) &&
         (!recognizer_ || !recognizer_->Healthy()))mic_error_=model_error_;
    }
    activating_model_=false;
    if(waiting_for_model_) {
      if(buffering_client_ && !ForegroundClientAllowed(buffering_client_)) {
        CancelRecording();
        return 0;
      }
      if(requested_capture_model_ || (retry_cancelled_load_ && prepare_cancel_)) {
        const auto requested=std::move(requested_capture_model_);
        requested_capture_model_.reset();
        retry_cancelled_load_=false;
        BeginPrepare(requested?&*requested:nullptr,requested.has_value(),true);
        return 0;
      }
      if(recognizer_ && recognizer_->Healthy()) StartBufferedDecode();
      else {
        ++capture_generation_;
        DiscardCapture();
        session_.RestorePending("");
        mic_error_=model_error_;
        PublishPhase();
      }
    }
    if(!quiet_preload) UpdateBox();
    return 0;
  }
  if(message==kDownloadedMessage) {
    if(download_thread_.joinable())download_thread_.join();downloading_=false;
    model_notice_=download_result_;notice_model_=downloading_models_.model;
    if(download_ok_ && !download_cancel_) {
      if(preparing_) {
        deferred_models_=downloading_models_;
        model_notice_=L"下载已完成；当前准备结束后，下次录音前验证所选模型。";
      } else {
        // A successful helper exit is not proof that every required model file
        // exists. Validate once and report any failure instead of re-downloading.
        model_notice_=UseModel(downloading_models_,false).message;
      }
    }
    return 0;
  }
  if(message==kCaptureReadyMessage) { if(static_cast<int>(wparam)==capture_generation_) UpdateBox(); return 0; }
  if (message == kFinalMessage) {
    if(finalize_thread_.joinable()) finalize_thread_.join();
    buffered_audio_.reset();
    last_final_ms_=GetTickCount64()-final_started_;model_last_used_=GetTickCount64();
    if(static_cast<int>(wparam)==finalize_generation_ && session_.phase()==Phase::Finalizing) {
      if(final_error_.empty()) PublishFinal(final_result_);
      else { mic_error_=final_error_; session_.RestorePending(session_.box_text()); UpdateBox(); PublishPhase(); }
    }
    ResumeQueuedStart();
    return 0;
  }
  if (message == kPartialMessage) {
    if (static_cast<int>(wparam) == capture_generation_) {
      if(lparam && recognizer_ && session_.phase()==Phase::Stopped) {
        session_.OnCaptureDrained(recognizer_->Partial());UpdateBox();
      } else OnPartial();
    }
    return 0;
  }
  if (message == kCaptureLimitMessage) {
    if (static_cast<int>(wparam) == capture_generation_) {
      if(lparam) stopped_notice_=L"识别缓冲已满 · 已保留 "+std::to_wstring(recorded_samples_.load()/kRecordingSampleRate)+L" 秒语音";
      OnCaptureLimit();
    }
    return 0;
  }
  if (message == kCaptureErrorMessage || message == kDeviceErrorMessage) {
    if (static_cast<int>(wparam) == capture_generation_ && (session_.can_finalize() || session_.phase()==Phase::Finalizing)) {
      if(message==kDeviceErrorMessage && recorded_samples_>0) {
        stopped_notice_=L"麦克风已中断 · 已保留本次采音";
        OnCaptureLimit();
        return 0;
      }
      ++capture_generation_;++finalize_generation_;
      DiscardCapture();
      mic_error_=reinterpret_cast<const char*>(lparam);
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
  std::unique_ptr<RimeSession> pinyin;
  HWND pinyin_window = nullptr;
  std::string awaiting_insert;
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
  auto deliver = [&](std::string& pending) {
    for (;;) {
      Message decoded;
      std::size_t used = 0;
      const auto status = DecodeFrame(pending, &decoded, &used);
      if (status == DecodeStatus::Invalid) return false;
      if (status == DecodeStatus::Incomplete) return true;
      pending.erase(0, used);
      if (!authenticated) {
        const bool wants_pinyin = decoded.type == MsgType::PinyinHello;
        if (!wants_pinyin && decoded.type != MsgType::ClientHello) return false;
        const auto window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(decoded.client_window));
        auto expected = module;
#ifdef VOICEIME_TESTING
        if (wants_pinyin) expected = ExecutableDirectory() + L"\\voiceime_tsf_integration.exe";
#endif
        if (!peer->owns_window(window, expected)) return false;
        authenticated = true;
        if (wants_pinyin) {
          if (PinyinWaitingForRestart()) return false;
          pinyin_window = window;
          pinyin = std::make_unique<RimeSession>();
          if (!pinyin->Start(ExecutableDirectory() + L"\\rime-data", data_path_ + L"\\rime")) return false;
          if (!WritePipeBytes(pipe, Encode({MsgType::PinyinView, EncodePinyinView(pinyin->view())}), stop_event_)) return false;
          continue;
        }
        PostMessageW(box_.hwnd(), kAnnounceMessage, id, reinterpret_cast<LPARAM>(window));
        continue;
      }
      if (pinyin) {
        if (decoded.type == MsgType::PinyinInserted) {
          if (!awaiting_insert.empty()) {
            decoded.text.swap(awaiting_insert);
            {
              std::lock_guard<std::mutex> lock(inbox_mu_);
              inbox_.emplace_back(id, std::move(decoded));
            }
            PostMessageW(box_.hwnd(), kPipeMessage, 0, 0);
          }
          continue;
        }
        if (decoded.type == MsgType::PinyinClear) pinyin->Clear();
        else if (decoded.type == MsgType::PinyinKey) {
          if (!IsForegroundPeer(*peer, pinyin_window, ForegroundInputProcess())) return false;
          const auto vk = static_cast<unsigned char>(decoded.text[0]);
          const auto modifiers = static_cast<unsigned char>(decoded.text[1]);
          if (!vk || modifiers > 7) return false;
          pinyin->ProcessKeyWithModifiers(vk, modifiers & 1, modifiers & 2, modifiers & 4);
        } else return false;
        awaiting_insert = pinyin->view().commit;
        const auto frame = Encode({MsgType::PinyinView, EncodePinyinView(pinyin->view())});
        if (frame.empty() || !WritePipeBytes(pipe, frame, stop_event_)) return false;
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
    const auto wake = pinyin ? nullptr : authenticated ? write_event_ : deadline.event();
    const PipeReadResult received = ReadPipeChunk(pipe, wake, stop_event_);
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

bool Engine::AcceptCaptureAudio(const std::int16_t* samples,std::size_t count,int generation,
                                CaptureHealth& health,RecordingBudget& budget,CaptureBuffer& buffer,bool tail) {
  if(stop_capture_ && !tail) return false;
  const auto available=std::min(count,kMaxRecordingSamples-budget.samples());
  if(!available) return !budget.full() && (!stop_capture_ || tail);
  const auto written=buffer.Write(samples,available);
  const auto accepted=budget.Take(written.samples);
  if(!accepted) return false;
  health.Push(samples,accepted,GetTickCount64());
  input_level_=health.level(); quiet_capture_=health.quiet();
  recorded_samples_=budget.samples();
  if(!tail && !capture_ready_.exchange(true)) PostMessageW(box_.hwnd(),kCaptureReadyMessage,generation,0);
  if(budget.full() || written.full) {
    stop_capture_=true;
    PostMessageW(box_.hwnd(),kCaptureLimitMessage,generation,written.full && !budget.full());
    return false;
  }
  return !stop_capture_ || tail;
}

bool Engine::ResetDecoder(int generation) {
  try {recognizer_->Reset();return true;}
  catch(...) {
    recognizer_->Cancel();
    PostMessageW(box_.hwnd(),kCaptureErrorMessage,generation,
        reinterpret_cast<LPARAM>("模型未能开始本次识别。请重新准备模型或更换模型。"));
    return false;
  }
}

bool Engine::FeedDecoder(const std::int16_t* samples,std::size_t count,int generation) {
  const bool changed=recognizer_->AcceptAudio(samples,count);
  if(!recognizer_->Healthy()) {
    stop_capture_=true;
    PostMessageW(box_.hwnd(),kCaptureErrorMessage,generation,
        reinterpret_cast<LPARAM>("识别模型已中断，草稿会保留。请重新录音或更换模型。"));
    return false;
  }
  if(changed && !partial_posted_.exchange(true)) PostMessageW(box_.hwnd(),kPartialMessage,generation,0);
  return true;
}

void Engine::CaptureMain(int generation,std::wstring microphone,HANDLE wake,CaptureBuffer& buffer) {
  if(stop_capture_)return;
  CaptureHealth health(GetTickCount64());
  RecordingBudget budget;
#ifdef VOICEIME_TESTING
  const auto fixture = TestEnvironment(L"VOICEIME_TEST_CAPTURE");
  if (fixture == L"error") {
    PostMessageW(box_.hwnd(), kDeviceErrorMessage, static_cast<WPARAM>(generation),
                 reinterpret_cast<LPARAM>("测试：麦克风采集已中断"));
    return;
  }
  if(fixture==L"model-cold-pcm") {
    std::ifstream input(std::filesystem::path(data_path_)/L"capture.pcm",std::ios::binary);
    std::int16_t block[320];
    const auto start=GetTickCount64();
    while(!stop_capture_) {
      input.read(reinterpret_cast<char*>(block),sizeof(block));
      const auto count=static_cast<std::size_t>(input.gcount())/sizeof(*block);
      if(!count || !AcceptCaptureAudio(block,count,generation,health,budget,buffer)) break;
      const auto due=start+budget.samples()*1000/kRecordingSampleRate;
      const auto now=GetTickCount64();
      if(due>now && WaitForSingleObject(wake,static_cast<DWORD>(due-now))==WAIT_OBJECT_0) break;
    }
    return;
  }
  std::vector<std::int16_t> fixture_samples(fixture==L"recording-limit" ? 16000*4 :
      fixture==L"model-buffer-limit" ? 16000*5 :
      fixture == L"silence" || fixture==L"long-speech" ? 16000 * 6 : 320, 0);
  int fixture_frames=0;
  while (!stop_capture_) {
    if(fixture==L"speech-end") std::fill(fixture_samples.begin(),fixture_samples.end(),fixture_frames++<5 ? 1200 : 0);
    if(fixture==L"soft-speech") std::fill(fixture_samples.begin(),fixture_samples.end(),fixture_frames++<5 ? 1200 : 250);
    if(fixture==L"long-speech" || fixture==L"recording-limit") std::fill(fixture_samples.begin(),fixture_samples.end(),1200);
    if(!AcceptCaptureAudio(fixture_samples.data(),fixture_samples.size(),generation,health,budget,buffer))return;
    if(fixture==L"model-buffer-disconnect") {
      PostMessageW(box_.hwnd(),kDeviceErrorMessage,generation,reinterpret_cast<LPARAM>("测试：采样设备已中断"));
      return;
    }
    if (WaitForSingleObject(wake,fixture==L"recording-limit" ? 8 : 20) == WAIT_OBJECT_0) break;
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
    PostMessageW(box_.hwnd(), kDeviceErrorMessage, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(reason));
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
             FAILED(client->SetEventHandle(wake)) ||
             FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture))) ||
             capture == nullptr) {
    fail("未能打开麦克风。");
  } else if(!stop_capture_) {
    if(FAILED(client->Start())) fail("未能打开麦克风。");
    else started=true;
  }

  std::unique_ptr<Resampler16k> resampler;
  if (started) {
    if (format->nSamplesPerSec == 0 || format->nSamplesPerSec > 384000 ||
        (IsFloatFormat(format)?format->wBitsPerSample!=32:(format->wBitsPerSample!=16 && format->wBitsPerSample!=32))) {
      fail("麦克风采样格式暂不支持，请更换输入设备。");
    } else resampler = std::make_unique<Resampler16k>(format->nSamplesPerSec);
  }
  auto read_packets=[&](bool draining) {
    while(draining?finish_capture_.load():!stop_capture_) {
      UINT32 packet=0;
      if(FAILED(capture->GetNextPacketSize(&packet))) {fail("麦克风采集已中断");break;}
      if(!packet || budget.full()) break;
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      const HRESULT result=capture->GetBuffer(&data,&frames,&flags,nullptr,nullptr);
      if(result==AUDCLNT_S_BUFFER_EMPTY) break;
      if(FAILED(result)) {fail("麦克风采集已中断");break;}
      std::vector<std::int16_t> mono;
      if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
        mono.assign(frames, 0);
      } else {
        mono = ToMono16(data, frames, format);
      }
      capture->ReleaseBuffer(frames);
      std::vector<std::int16_t> samples = resampler->Process(mono.data(), mono.size());
      const bool final_packet=draining || (stop_capture_ && finish_capture_);
      if(!AcceptCaptureAudio(samples.data(),samples.size(),generation,health,budget,buffer,final_packet)) break;
    }
  };
  while (started && !stop_capture_) {
    const DWORD waited=WaitForSingleObject(wake,200);
    if(stop_capture_) break;
    if(const char* error=health.Error(GetTickCount64())) {fail(error);break;}
    if(waited!=WAIT_OBJECT_0 && waited!=WAIT_TIMEOUT) {fail("麦克风采集已中断");break;}
    read_packets(false);
  }

  // Freeze the endpoint before draining samples already delivered by Windows.
  if(started && SUCCEEDED(client->Stop()) && resampler && finish_capture_) read_packets(true);
  if (resampler) {
    const auto tail = resampler->Flush();
    AcceptCaptureAudio(tail.data(),tail.size(),generation,health,budget,buffer,true);
  }
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
