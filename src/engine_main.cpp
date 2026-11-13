#include "confirm_box.hpp"
#include "lexicon.hpp"
#include "recognizer.hpp"
#include "resample.hpp"
#include "segmenter.hpp"
#include "session.hpp"
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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
constexpr UINT kTrayMessage = WM_APP + 4;
constexpr UINT kQuitCommand = 1;
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

bool ReadExact(HANDLE pipe, void* data, DWORD size) {
  auto* bytes = static_cast<char*>(data);
  DWORD got_total = 0;
  while (got_total < size) {
    DWORD got = 0;
    if (!ReadFile(pipe, bytes + got_total, size - got_total, &got, nullptr) || got == 0) return false;
    got_total += got;
  }
  return true;
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
  void AddTray();
  void RemoveTray();

  ConfirmBox box_;
  Session session_;
  Vad vad_{VadConfig{16000, 500, 60000, 800}};
  std::unique_ptr<Recognizer> recognizer_;
  std::unique_ptr<Lexicon> lexicon_;
  std::unique_ptr<Segmenter> segmenter_;
  std::thread capture_thread_;
  std::thread pipe_thread_;
  std::atomic<bool> stop_capture_{false};
  std::atomic<bool> stop_pipe_{false};
  HANDLE wake_ = nullptr;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  std::mutex inbox_mu_;
  std::deque<Message> inbox_;
  bool tray_added_ = false;
  std::string mic_error_;
  std::atomic<bool> partial_posted_{false};

  static LRESULT Hook(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* self) {
    return static_cast<Engine*>(self)->OnMessage(hwnd, message, wparam, lparam);
  }
};

int Engine::Run(bool fake) {
  const std::wstring data = [] {
    wchar_t buffer[MAX_PATH];
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::wstring dir = count > 0 ? std::wstring(buffer, count) : L".";
    dir += L"\\VoiceIme";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
  }();
  lexicon_ = std::make_unique<Lexicon>(WideToUtf8(data + L"\\lexicon.sqlite"));
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
  pipe_thread_ = std::thread([this] { PipeMain(); });

  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  stop_pipe_ = true;
  if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, nullptr);
  StopCapture();
  if (pipe_thread_.joinable()) pipe_thread_.join();
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

void Engine::UpdateBox() {
  if (session_.phase() == Phase::Idle) {
    box_.Hide();
    return;
  }
  const std::string text = mic_error_.empty() ? session_.box_text() : mic_error_;
  box_.ShowText(text, Rect{0, 0, 0, 0}, ForegroundClient());
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
  if (capture_thread_.joinable()) capture_thread_.join();
  if (session_.phase() != Phase::Listening) return;
  session_.OnSpeechEnded();
  const std::string final_text = recognizer_->Finalize();
  session_.OnFinal(final_text);
  std::fprintf(stderr, "final: %s\n", final_text.c_str());
  UpdateBox();
}

void Engine::OnPipe(const Message& message) {
  if (message.type == MsgType::Cancel) {
    StopCapture();
    session_.Escape();
    mic_error_.clear();
    UpdateBox();
    return;
  }
  if (message.type != MsgType::StartListen) return;
  if (session_.phase() == Phase::Listening) {
    StopCapture();
    session_.Hotkey();
    const std::string final_text = recognizer_->Finalize();
    session_.OnFinal(final_text);
    std::fprintf(stderr, "final: %s\n", final_text.c_str());
    UpdateBox();
    return;
  }
  if (session_.phase() == Phase::Finalizing) return;
  recognizer_->Reset();
  session_.Hotkey();
  StartCapture();
  UpdateBox();
}

LRESULT Engine::OnMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kPipeMessage) {
    std::deque<Message> local;
    {
      std::lock_guard<std::mutex> lock(inbox_mu_);
      local.swap(inbox_);
    }
    for (const Message& item : local) OnPipe(item);
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
    const std::wstring quit = Utf8ToWide("退出");
    AppendMenuW(menu, MF_STRING, kQuitCommand, quit.c_str());
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    return 0;
  }
  if (message == WM_COMMAND && LOWORD(wparam) == kQuitCommand) {
    DestroyWindow(hwnd);
    return 0;
  }
  if (message == WM_DESTROY) {
    RemoveTray();
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void Engine::PipeMain() {
  while (!stop_pipe_) {
    HANDLE pipe = CreateNamedPipeW(L"\\\\.\\pipe\\VoiceIme", PIPE_ACCESS_DUPLEX,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                    65536, 65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return;
    pipe_ = pipe;
    const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
    if (!connected || stop_pipe_) {
      CloseHandle(pipe);
      pipe_ = INVALID_HANDLE_VALUE;
      if (stop_pipe_) return;
      continue;
    }
    while (!stop_pipe_) {
      unsigned char header[4];
      if (!ReadExact(pipe, header, 4)) break;
      const std::uint32_t length = static_cast<std::uint32_t>(header[0]) | (static_cast<std::uint32_t>(header[1]) << 8) |
                                   (static_cast<std::uint32_t>(header[2]) << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
      if (length < 1 || length > (1u << 20)) break;
      std::string frame(4u + length, '\0');
      std::memcpy(frame.data(), header, 4);
      if (!ReadExact(pipe, frame.data() + 4, length)) break;
      Message decoded;
      std::size_t used = 0;
      if (!Decode(frame, &decoded, &used)) continue;
      {
        std::lock_guard<std::mutex> lock(inbox_mu_);
        inbox_.push_back(std::move(decoded));
      }
      PostMessageW(box_.hwnd(), kPipeMessage, 0, 0);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    pipe_ = INVALID_HANDLE_VALUE;
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
