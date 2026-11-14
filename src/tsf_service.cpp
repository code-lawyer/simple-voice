#include <initguid.h>

#include "tsf_service.hpp"

#include "candidate_bar.hpp"
#include "hotkey.hpp"
#include "ipc.hpp"
#include "register_status.hpp"
#include "rime_session.hpp"

#include <msctf.h>
#include <oleauto.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr UINT kCommitMessage = WM_APP + 1;
constexpr UINT kCaretMessage = WM_APP + 2;

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  return wide;
}

int NumberAfter(const std::string& json, const char* key, bool* ok) {
  const auto at = json.find(std::string("\"") + key + "\"");
  if (at == std::string::npos) {
    *ok = false;
    return 0;
  }
  auto cursor = json.find(':', at);
  if (cursor == std::string::npos) {
    *ok = false;
    return 0;
  }
  ++cursor;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
  if (cursor >= json.size() || json[cursor] < '0' || json[cursor] > '9') {
    *ok = false;
    return 0;
  }
  int value = 0;
  while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
    value = value * 10 + (json[cursor] - '0');
    ++cursor;
  }
  *ok = true;
  return value;
}

// Local reader so Task 10 can still introduce LoadSettings without this copy
// already satisfying that test. Missing or unreadable files stay on the default.
Hotkey ReadConfiguredHotkey() {
  wchar_t local[MAX_PATH];
  const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (count == 0 || count >= MAX_PATH) return DefaultHotkey();
  std::wstring path(local, count);
  path += L"\\VoiceIme\\config.json";
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return DefaultHotkey();
  char buffer[4096];
  DWORD got = 0;
  const BOOL read = ReadFile(file, buffer, sizeof(buffer), &got, nullptr);
  CloseHandle(file);
  if (!read || got == 0) return DefaultHotkey();
  const std::string json(buffer, buffer + got);
  bool mod_ok = false;
  bool vk_ok = false;
  const int modifiers = NumberAfter(json, "modifiers", &mod_ok);
  const int vk = NumberAfter(json, "vk", &vk_ok);
  if (!mod_ok || !vk_ok || vk <= 0 || vk > 255) return DefaultHotkey();
  return Hotkey{modifiers, vk};
}

enum class CompositionEdit { SetPreedit, Commit, Cancel };

std::wstring ModuleDirectory() {
  wchar_t module_path[MAX_PATH];
  const DWORD count = GetModuleFileNameW(VoiceImeModule(), module_path, MAX_PATH);
  if (count == 0 || count >= MAX_PATH) return L"";
  std::wstring path(module_path, count);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return L"";
  return path.substr(0, slash);
}

std::wstring RimeUserDirectory() {
  wchar_t local[MAX_PATH];
  const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (count == 0 || count >= MAX_PATH) return L"";
  return std::wstring(local, count) + L"\\VoiceIme\\rime";
}

bool EatsKey(KeyAction action) {
  return action != KeyAction::Pass && action != KeyAction::PassAfterClear;
}

bool NeedsVoicePipe(KeyAction action) {
  return action == KeyAction::OpenVoice || action == KeyAction::VoiceEnter || action == KeyAction::VoiceEscape;
}

void MoveCaretToEnd(TfEditCookie cookie, ITfContext* context, ITfRange* range) {
  if (context == nullptr || range == nullptr) return;
  ITfRange* end = nullptr;
  if (FAILED(range->Clone(&end)) || end == nullptr) return;
  end->Collapse(cookie, TF_ANCHOR_END);
  TF_SELECTION selection{};
  selection.range = end;
  selection.style.ase = TF_AE_NONE;
  selection.style.fInterimChar = FALSE;
  context->SetSelection(cookie, 1, &selection);
  end->Release();
}

class VoiceService : public ITfTextInputProcessor {
 public:
  VoiceService() : key_sink_(this), thread_sink_(this), composition_sink_(this), pinyin_(&rime_) {}
  ~VoiceService() { Deactivate(); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfTextInputProcessor)) {
      *out = static_cast<ITfTextInputProcessor*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return static_cast<ULONG>(left);
  }

  HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* manager, TfClientId client_id) override;
  HRESULT STDMETHODCALLTYPE Deactivate() override;

  HRESULT OnKey(ITfContext* context, WPARAM vk, LPARAM lparam, WINBOOL* eaten, bool down);
  HRESULT OnKeyUp(WPARAM vk, WINBOOL* eaten, bool down);
  void OnForeground(bool foreground) { foreground_ = foreground; }

  void ReadLoop();
  void HandleCommit(const std::wstring& text);
  void HandleCaret();

 private:
  class KeySink : public ITfKeyEventSink {
   public:
    explicit KeySink(VoiceService* owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
      if (out == nullptr) return E_POINTER;
      if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfKeyEventSink)) {
        *out = static_cast<ITfKeyEventSink*>(this);
        AddRef();
        return S_OK;
      }
      return owner_->QueryInterface(iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return owner_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner_->Release(); }
    HRESULT STDMETHODCALLTYPE OnSetFocus(WINBOOL foreground) override {
      owner_->OnForeground(foreground == TRUE);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM vk, LPARAM lparam, WINBOOL* eaten) override {
      return owner_->OnKey(context, vk, lparam, eaten, false);
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext*, WPARAM vk, LPARAM, WINBOOL* eaten) override {
      return owner_->OnKeyUp(vk, eaten, false);
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM vk, LPARAM lparam, WINBOOL* eaten) override {
      return owner_->OnKey(context, vk, lparam, eaten, true);
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext*, WPARAM vk, LPARAM, WINBOOL* eaten) override {
      return owner_->OnKeyUp(vk, eaten, true);
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext*, REFGUID, WINBOOL* eaten) override {
      if (eaten != nullptr) *eaten = FALSE;
      return S_OK;
    }

   private:
    VoiceService* owner_;
  };

  class ThreadSink : public ITfThreadMgrEventSink {
   public:
    explicit ThreadSink(VoiceService* owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
      if (out == nullptr) return E_POINTER;
      if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfThreadMgrEventSink)) {
        *out = static_cast<ITfThreadMgrEventSink*>(this);
        AddRef();
        return S_OK;
      }
      return owner_->QueryInterface(iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return owner_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner_->Release(); }
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr*, ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext*) override { return S_OK; }

   private:
    VoiceService* owner_;
  };

  class CompositionSink : public ITfCompositionSink {
   public:
    explicit CompositionSink(VoiceService* owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
      if (out == nullptr) return E_POINTER;
      if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfCompositionSink)) {
        *out = static_cast<ITfCompositionSink*>(this);
        AddRef();
        return S_OK;
      }
      return owner_->QueryInterface(iid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return owner_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner_->Release(); }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie cookie, ITfComposition* composition) override;

   private:
    VoiceService* owner_;
  };

  struct StackSession : ITfEditSession {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
      if (out == nullptr) return E_POINTER;
      if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfEditSession)) {
        *out = static_cast<ITfEditSession*>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override { return static_cast<ULONG>(InterlockedDecrement(&refs)); }
  };

  struct InsertSession : StackSession {
    ITfContext* context = nullptr;
    std::wstring text;
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
      if (text.empty() || context == nullptr) return S_OK;
      ITfInsertAtSelection* insert = nullptr;
      if (FAILED(context->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert))) ||
          insert == nullptr) {
        return E_NOINTERFACE;
      }
      const HRESULT hr = insert->InsertTextAtSelection(cookie, TF_IAS_NOQUERY, text.c_str(),
                                                        static_cast<LONG>(text.size()), nullptr);
      insert->Release();
      return hr;
    }
  };

  struct CaretSession : StackSession {
    ITfContext* context = nullptr;
    int caret_x = 0;
    int caret_y = 0;
    int caret_w = 0;
    int caret_h = 0;
    int window_x = 0;
    int window_y = 0;
    int window_w = 0;
    int window_h = 0;
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
      if (context == nullptr) return S_OK;
      TF_SELECTION selection{};
      ULONG fetched = 0;
      const HRESULT selected = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
      ITfContextView* view = nullptr;
      if (FAILED(context->GetActiveView(&view)) || view == nullptr) {
        if (SUCCEEDED(selected) && selection.range != nullptr) selection.range->Release();
        return S_OK;
      }
      if (SUCCEEDED(selected) && fetched > 0 && selection.range != nullptr) {
        RECT rect{};
        WINBOOL clipped = FALSE;
        if (SUCCEEDED(view->GetTextExt(cookie, selection.range, &rect, &clipped))) {
          int width = rect.right - rect.left;
          int height = rect.bottom - rect.top;
          // A caret is often 0 wide. PlaceBox only anchors when both sides are positive.
          if (width <= 0 && height > 0) width = 1;
          if (height <= 0 && width > 0) height = 1;
          if (width > 0 && height > 0) {
            caret_x = rect.left;
            caret_y = rect.top;
            caret_w = width;
            caret_h = height;
          }
        }
        selection.range->Release();
      }
      HWND window = nullptr;
      RECT screen{};
      if (SUCCEEDED(view->GetWnd(&window)) && window != nullptr) {
        RECT client{};
        POINT origin{0, 0};
        if (GetClientRect(window, &client) && ClientToScreen(window, &origin)) {
          window_x = origin.x;
          window_y = origin.y;
          window_w = client.right;
          window_h = client.bottom;
        }
      } else if (SUCCEEDED(view->GetScreenExt(&screen)) && screen.right > screen.left && screen.bottom > screen.top) {
        window_x = screen.left;
        window_y = screen.top;
        window_w = screen.right - screen.left;
        window_h = screen.bottom - screen.top;
      }
      view->Release();
      return S_OK;
    }
  };

  struct CompSession : StackSession {
    VoiceService* owner = nullptr;
    ITfContext* context = nullptr;
    CompositionEdit op = CompositionEdit::Cancel;
    std::wstring text;
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override;
  };

  bool DocumentAcceptsText(ITfContext* context) const;
  bool WriteClient(const Message& message);
  bool EnsureRime();
  bool PipeConnected();
  KeyState MakeKeyState(ITfContext* context, WPARAM vk, LPARAM lparam);
  void SendVoice(MsgType type, WPARAM vk, WINBOOL* eaten);
  void RunComposition(ITfContext* context, CompositionEdit op, const std::wstring& text);
  void EditComposition(TfEditCookie cookie, ITfContext* context, CompositionEdit op, const std::wstring& text);
  bool StartComposition(TfEditCookie cookie, ITfContext* context);
  void SetCompositionText(TfEditCookie cookie, ITfContext* context, const std::wstring& text);
  void CommitComposition(TfEditCookie cookie, ITfContext* context, const std::wstring& text);
  void CancelComposition(TfEditCookie cookie);
  void ReleaseCompositionPointer();
  void OnCompositionTerminated();
  void ShowCandidates(ITfContext* context);
  void SyncAfterRime(ITfContext* context);
  void CancelFocusComposition();
  void LaunchEngine();
  void EnsureWindow();
  ITfContext* FocusContext() const;
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  KeySink key_sink_;
  ThreadSink thread_sink_;
  CompositionSink composition_sink_;
  RimeSession rime_;
  PinyinController pinyin_;
  CandidateBar bar_;
  ITfComposition* composition_ = nullptr;
  bool ending_composition_ = false;
  bool rime_ready_ = false;
  bool rime_failed_ = false;
  LONG refs_ = 1;
  ITfThreadMgr* thread_mgr_ = nullptr;
  TfClientId client_id_ = 0;
  DWORD thread_cookie_ = 0;
  bool advised_keys_ = false;
  bool foreground_ = true;
  bool active_ = false;
  int eaten_vk_ = 0;
  HWND host_ = nullptr;
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE reader_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE write_event_ = nullptr;
  std::mutex write_mu_;
  std::vector<std::string> outgoing_;
  bool stop_reader_ = false;
};

bool VoiceService::DocumentAcceptsText(ITfContext* context) const {
  if (context == nullptr) return false;
  TF_STATUS status{};
  if (FAILED(context->GetStatus(&status))) return false;
  if ((status.dwStaticFlags & TF_SD_READONLY) != 0) return false;
  ITfCompartmentMgr* manager = nullptr;
  if (FAILED(context->QueryInterface(IID_ITfCompartmentMgr, reinterpret_cast<void**>(&manager))) || manager == nullptr) {
    return true;
  }
  ITfCompartment* compartment = nullptr;
  bool disabled = false;
  if (SUCCEEDED(manager->GetCompartment(kKeyboardDisabled, &compartment)) && compartment != nullptr) {
    VARIANT value{};
    VariantInit(&value);
    if (SUCCEEDED(compartment->GetValue(&value)) && value.vt == VT_I4 && value.lVal != 0) disabled = true;
    VariantClear(&value);
    compartment->Release();
  }
  manager->Release();
  return !disabled;
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

bool VoiceService::WriteClient(const Message& message) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (pipe_ == INVALID_HANDLE_VALUE || write_event_ == nullptr) return false;
  outgoing_.push_back(Encode(message));
  SetEvent(write_event_);
  return true;
}

void VoiceService::LaunchEngine() {
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\VoiceImeEngine");
  const bool running = mutex != nullptr;
  if (mutex != nullptr) CloseHandle(mutex);
  if (!ShouldLaunchEngine(running)) return;
  wchar_t module_path[MAX_PATH];
  const DWORD count = GetModuleFileNameW(VoiceImeModule(), module_path, MAX_PATH);
  if (count == 0 || count >= MAX_PATH) return;
  std::wstring path(module_path, count);
  const auto slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return;
  const std::wstring dir = path.substr(0, slash);
  const std::wstring exe = dir + L"\\voiceime_engine.exe";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &startup,
                       &process)) {
    return;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

void VoiceService::EnsureWindow() {
  if (host_ != nullptr) return;
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = VoiceImeModule();
  window_class.lpszClassName = L"VoiceImeTsfHost";
  RegisterClassW(&window_class);
  host_ = CreateWindowExW(0, L"VoiceImeTsfHost", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, VoiceImeModule(), this);
}

LRESULT CALLBACK VoiceService::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto* self = reinterpret_cast<VoiceService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
  if (message == kCommitMessage) {
    auto* text = reinterpret_cast<std::wstring*>(lparam);
    if (text != nullptr) {
      self->HandleCommit(*text);
      delete text;
    }
    return 0;
  }
  if (message == kCaretMessage) {
    self->HandleCaret();
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

ITfContext* VoiceService::FocusContext() const {
  if (thread_mgr_ == nullptr) return nullptr;
  ITfDocumentMgr* document = nullptr;
  if (FAILED(thread_mgr_->GetFocus(&document)) || document == nullptr) return nullptr;
  ITfContext* context = nullptr;
  const HRESULT hr = document->GetTop(&context);
  document->Release();
  if (FAILED(hr)) return nullptr;
  return context;
}

void VoiceService::HandleCommit(const std::wstring& text) {
  bool inserted = false;
  ITfContext* context = FocusContext();
  if (context != nullptr) {
    InsertSession session;
    session.context = context;
    session.text = text;
    HRESULT session_hr = E_FAIL;
    context->RequestEditSession(client_id_, &session, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
    inserted = SUCCEEDED(session_hr);
    context->Release();
  }
  if (inserted) active_ = false;
  Message ack;
  ack.type = MsgType::InsertAck;
  ack.text = inserted ? "ok" : "fail";
  WriteClient(ack);
}

void VoiceService::HandleCaret() {
  Message reply;
  reply.type = MsgType::Caret;
  ITfContext* context = FocusContext();
  if (context != nullptr) {
    CaretSession session;
    session.context = context;
    HRESULT session_hr = E_FAIL;
    context->RequestEditSession(client_id_, &session, TF_ES_SYNC | TF_ES_READ, &session_hr);
    if (SUCCEEDED(session_hr)) {
      reply.caret_x = session.caret_x;
      reply.caret_y = session.caret_y;
      reply.caret_w = session.caret_w;
      reply.caret_h = session.caret_h;
      reply.window_x = session.window_x;
      reply.window_y = session.window_y;
      reply.window_w = session.window_w;
      reply.window_h = session.window_h;
    }
    context->Release();
  }
  WriteClient(reply);
}

void VoiceService::ReadLoop() {
  auto deliver = [this](std::string& pending) {
    for (;;) {
      Message message;
      std::size_t used = 0;
      if (!Decode(pending, &message, &used)) break;
      pending.erase(0, used);
      if (message.type == MsgType::CommitDone && host_ != nullptr) {
        PostMessageW(host_, kCommitMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(Utf8ToWide(message.text))));
      } else if (message.type == MsgType::Phase) {
        active_ = !message.text.empty() && message.text != "0";
      } else if (message.type == MsgType::QueryCaret && host_ != nullptr && foreground_) {
        PostMessageW(host_, kCaretMessage, 0, 0);
      }
    }
  };
  while (stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    LaunchEngine();
    // One thread owns the handle. A synchronous read and write on the same
    // pipe deadlock, so the UI thread only queues bytes and this loop writes them.
    HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\VoiceIme", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(stop_event_, 100) == WAIT_OBJECT_0) return;
      continue;
    }
    OVERLAPPED overlap{};
    overlap.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlap.hEvent == nullptr) {
      CloseHandle(pipe);
      if (WaitForSingleObject(stop_event_, 100) == WAIT_OBJECT_0) return;
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      pipe_ = pipe;
    }
    std::string buffer;
    char chunk[4096];
    bool failed = false;
    while (!failed && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
      std::vector<std::string> mine;
      {
        std::lock_guard<std::mutex> lock(write_mu_);
        mine.swap(outgoing_);
        if (outgoing_.empty() && write_event_ != nullptr) ResetEvent(write_event_);
      }
      if (!mine.empty()) {
        for (const std::string& bytes : mine) {
          if (!WriteAll(pipe, bytes)) failed = true;
        }
        continue;
      }
      ResetEvent(overlap.hEvent);
      DWORD got = 0;
      const BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), &got, &overlap);
      if (!ok && GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[3] = {overlap.hEvent, nullptr, nullptr};
        DWORD count = 1;
        if (write_event_ != nullptr) waits[count++] = write_event_;
        if (stop_event_ != nullptr) waits[count++] = stop_event_;
        const DWORD which = WaitForMultipleObjects(count, waits, FALSE, INFINITE);
        if (which != WAIT_OBJECT_0) {
          CancelIoEx(pipe, &overlap);
          DWORD partial = 0;
          if (GetOverlappedResult(pipe, &overlap, &partial, TRUE) && partial > 0) {
            buffer.append(chunk, chunk + partial);
            deliver(buffer);
          }
          if (which == WAIT_FAILED) failed = true;
          continue;
        }
        if (!GetOverlappedResult(pipe, &overlap, &got, FALSE) || got == 0) {
          failed = true;
          continue;
        }
      } else if (!ok || got == 0) {
        failed = true;
        continue;
      }
      buffer.append(chunk, chunk + got);
      deliver(buffer);
    }
    const bool stopping = stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      outgoing_.clear();
      if (!stopping && pipe_ == pipe) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
      }
    }
    CloseHandle(overlap.hEvent);
    if (stopping) return;
  }
}

DWORD WINAPI VoiceReader(void* param) {
  static_cast<VoiceService*>(param)->ReadLoop();
  return 0;
}

HRESULT VoiceService::Activate(ITfThreadMgr* manager, TfClientId client_id) {
  if (manager == nullptr) return E_INVALIDARG;
  thread_mgr_ = manager;
  thread_mgr_->AddRef();
  client_id_ = client_id;
  EnsureWindow();
  ITfKeystrokeMgr* keys = nullptr;
  if (SUCCEEDED(manager->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keys))) && keys != nullptr) {
    advised_keys_ = SUCCEEDED(keys->AdviseKeyEventSink(client_id_, &key_sink_, TRUE));
    keys->Release();
  }
  ITfSource* source = nullptr;
  if (SUCCEEDED(manager->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) && source != nullptr) {
    source->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<IUnknown*>(&thread_sink_), &thread_cookie_);
    source->Release();
  }
  stop_reader_ = false;
  if (stop_event_ == nullptr) stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  else ResetEvent(stop_event_);
  if (write_event_ == nullptr) write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  else ResetEvent(write_event_);
  reader_ = CreateThread(nullptr, 0, VoiceReader, this, 0, nullptr);
  return S_OK;
}

HRESULT VoiceService::Deactivate() {
  CancelFocusComposition();
  bar_.Hide();
  rime_.Shutdown();
  rime_ready_ = false;
  rime_failed_ = false;
  stop_reader_ = true;
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  if (write_event_ != nullptr) SetEvent(write_event_);
  HANDLE reader = nullptr;
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    reader = reader_;
  }
  for (int attempt = 0; reader != nullptr && attempt < 50; ++attempt) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      pipe = pipe_;
    }
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    if (WaitForSingleObject(reader, 200) == WAIT_OBJECT_0) break;
  }
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    if (reader_ != nullptr) {
      CloseHandle(reader_);
      reader_ = nullptr;
    }
    if (pipe_ != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
    if (stop_event_ != nullptr) {
      CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
    if (write_event_ != nullptr) {
      CloseHandle(write_event_);
      write_event_ = nullptr;
    }
    outgoing_.clear();
  }
  if (thread_mgr_ != nullptr && advised_keys_) {
    ITfKeystrokeMgr* keys = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keys))) && keys != nullptr) {
      keys->UnadviseKeyEventSink(client_id_);
      keys->Release();
    }
    advised_keys_ = false;
  }
  if (thread_mgr_ != nullptr && thread_cookie_ != 0) {
    ITfSource* source = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source))) && source != nullptr) {
      source->UnadviseSink(thread_cookie_);
      source->Release();
    }
    thread_cookie_ = 0;
  }
  if (host_ != nullptr) {
    DestroyWindow(host_);
    host_ = nullptr;
  }
  if (thread_mgr_ != nullptr) {
    thread_mgr_->Release();
    thread_mgr_ = nullptr;
  }
  active_ = false;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE VoiceService::CompositionSink::OnCompositionTerminated(TfEditCookie, ITfComposition*) {
  owner_->OnCompositionTerminated();
  return S_OK;
}

HRESULT STDMETHODCALLTYPE VoiceService::CompSession::DoEditSession(TfEditCookie cookie) {
  if (owner != nullptr) owner->EditComposition(cookie, context, op, text);
  return S_OK;
}

bool VoiceService::EnsureRime() {
  if (rime_ready_) return true;
  if (rime_failed_) return false;
  const std::wstring shared = ModuleDirectory();
  const std::wstring user = RimeUserDirectory();
  // Maintenance is slow. Activate must return without this call.
  if (shared.empty() || user.empty() || !rime_.Start(shared + L"\\rime-data", user)) {
    rime_failed_ = true;
    return false;
  }
  rime_ready_ = true;
  return true;
}

bool VoiceService::PipeConnected() {
  std::lock_guard<std::mutex> lock(write_mu_);
  return pipe_ != INVALID_HANDLE_VALUE;
}

KeyState VoiceService::MakeKeyState(ITfContext* context, WPARAM vk, LPARAM lparam) {
  const RimeView& view = pinyin_.view();
  KeyState state{};
  state.mode = pinyin_.mode();
  state.accepts = DocumentAcceptsText(context);
  state.voice_active = active_;
  state.composing = view.composing;
  state.candidate_count = static_cast<int>(view.page.size());
  state.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  state.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  state.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
  // Caps Lock is a toggle. The low bit is the locked state, not key-down.
  state.caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
  state.repeat = IsKeyRepeat(static_cast<unsigned int>(lparam));
  state.vk = static_cast<int>(vk);
  return state;
}

void VoiceService::SendVoice(MsgType type, WPARAM vk, WINBOOL* eaten) {
  Message message;
  message.type = type;
  if (!WriteClient(message)) {
    if (type == MsgType::Enter || type == MsgType::Cancel) active_ = false;
    return;
  }
  if (type == MsgType::StartListen) active_ = true;
  if (type == MsgType::Cancel) active_ = false;
  eaten_vk_ = static_cast<int>(vk);
  if (eaten != nullptr) *eaten = TRUE;
}

void VoiceService::RunComposition(ITfContext* context, CompositionEdit op, const std::wstring& text) {
  if (op == CompositionEdit::Cancel && composition_ == nullptr) return;
  if (context == nullptr || client_id_ == 0) {
    if (op == CompositionEdit::Cancel) ReleaseCompositionPointer();
    return;
  }
  CompSession session;
  session.owner = this;
  session.context = context;
  session.op = op;
  session.text = text;
  HRESULT session_hr = E_FAIL;
  context->RequestEditSession(client_id_, &session, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
  if (FAILED(session_hr) && op == CompositionEdit::Cancel) ReleaseCompositionPointer();
}

void VoiceService::EditComposition(TfEditCookie cookie, ITfContext* context, CompositionEdit op, const std::wstring& text) {
  if (op == CompositionEdit::Commit) CommitComposition(cookie, context, text);
  else if (op == CompositionEdit::SetPreedit) SetCompositionText(cookie, context, text);
  else CancelComposition(cookie);
}

bool VoiceService::StartComposition(TfEditCookie cookie, ITfContext* context) {
  if (composition_ != nullptr) return true;
  if (context == nullptr) return false;
  ITfInsertAtSelection* insert = nullptr;
  if (FAILED(context->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&insert))) || insert == nullptr) {
    return false;
  }
  ITfRange* range = nullptr;
  const HRESULT inserted = insert->InsertTextAtSelection(cookie, TF_IAS_QUERYONLY, nullptr, 0, &range);
  insert->Release();
  if (FAILED(inserted) || range == nullptr) return false;
  ITfContextComposition* compositions = nullptr;
  if (FAILED(context->QueryInterface(IID_ITfContextComposition, reinterpret_cast<void**>(&compositions))) ||
      compositions == nullptr) {
    range->Release();
    return false;
  }
  ITfComposition* created = nullptr;
  const HRESULT started = compositions->StartComposition(cookie, range, &composition_sink_, &created);
  compositions->Release();
  range->Release();
  if (FAILED(started) || created == nullptr) return false;
  composition_ = created;
  return true;
}

void VoiceService::SetCompositionText(TfEditCookie cookie, ITfContext* context, const std::wstring& text) {
  if (text.empty()) {
    CancelComposition(cookie);
    return;
  }
  if (composition_ == nullptr && !StartComposition(cookie, context)) return;
  ITfRange* range = nullptr;
  if (composition_ == nullptr || FAILED(composition_->GetRange(&range)) || range == nullptr) return;
  range->SetText(cookie, TF_ST_CORRECTION, text.c_str(), static_cast<LONG>(text.size()));
  MoveCaretToEnd(cookie, context, range);
  range->Release();
}

void VoiceService::CommitComposition(TfEditCookie cookie, ITfContext* context, const std::wstring& text) {
  if (composition_ == nullptr && !text.empty() && !StartComposition(cookie, context)) return;
  if (composition_ == nullptr) return;
  ITfRange* range = nullptr;
  if (SUCCEEDED(composition_->GetRange(&range)) && range != nullptr) {
    range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size()));
    MoveCaretToEnd(cookie, context, range);
    range->Release();
  }
  ITfComposition* held = composition_;
  ending_composition_ = true;
  held->EndComposition(cookie);
  ending_composition_ = false;
  if (composition_ == held) ReleaseCompositionPointer();
}

void VoiceService::CancelComposition(TfEditCookie cookie) {
  if (composition_ == nullptr) return;
  ITfRange* range = nullptr;
  if (SUCCEEDED(composition_->GetRange(&range)) && range != nullptr) {
    range->SetText(cookie, TF_ST_CORRECTION, L"", 0);
    range->Release();
  }
  ITfComposition* held = composition_;
  ending_composition_ = true;
  held->EndComposition(cookie);
  ending_composition_ = false;
  if (composition_ == held) ReleaseCompositionPointer();
}

void VoiceService::ReleaseCompositionPointer() {
  if (composition_ == nullptr) return;
  ITfComposition* held = composition_;
  composition_ = nullptr;
  held->Release();
}

void VoiceService::OnCompositionTerminated() {
  ReleaseCompositionPointer();
  if (ending_composition_) return;
  rime_.Clear();
  bar_.Hide();
}

void VoiceService::ShowCandidates(ITfContext* context) {
  const RimeView& view = pinyin_.view();
  Rect caret{0, 0, 0, 0};
  Rect window{0, 0, 0, 0};
  if (context != nullptr && client_id_ != 0) {
    CaretSession session;
    session.context = context;
    HRESULT session_hr = E_FAIL;
    context->RequestEditSession(client_id_, &session, TF_ES_SYNC | TF_ES_READ, &session_hr);
    if (SUCCEEDED(session_hr)) {
      caret = Rect{session.caret_x, session.caret_y, session.caret_w, session.caret_h};
      window = Rect{session.window_x, session.window_y, session.window_w, session.window_h};
    }
  }
  bar_.Show(caret, window, view, pinyin_.mode());
}

void VoiceService::SyncAfterRime(ITfContext* context) {
  const RimeView& view = pinyin_.view();
  if (!view.commit.empty()) {
    RunComposition(context, CompositionEdit::Commit, Utf8ToWide(view.commit));
    if (view.composing && !view.preedit.empty()) {
      RunComposition(context, CompositionEdit::SetPreedit, Utf8ToWide(view.preedit));
      ShowCandidates(context);
    } else {
      bar_.Hide();
    }
    return;
  }
  if (view.preedit.empty()) {
    RunComposition(context, CompositionEdit::Cancel, L"");
    bar_.Hide();
    return;
  }
  RunComposition(context, CompositionEdit::SetPreedit, Utf8ToWide(view.preedit));
  ShowCandidates(context);
}

void VoiceService::CancelFocusComposition() {
  ITfContext* context = FocusContext();
  if (context != nullptr) {
    RunComposition(context, CompositionEdit::Cancel, L"");
    context->Release();
  }
  ReleaseCompositionPointer();
}

HRESULT VoiceService::OnKey(ITfContext* context, WPARAM vk, LPARAM lparam, WINBOOL* eaten, bool down) {
  if (eaten != nullptr) *eaten = FALSE;
  if (!foreground_) return S_OK;
  const KeyState state = MakeKeyState(context, vk, lparam);
  const Hotkey hotkey = ReadConfiguredHotkey();
  const KeyAction action = RouteKey(state, hotkey).action;
  // RouteKey reports ToggleMode for a held Shift. A repeat must not flip mode.
  const bool shift_repeat = state.repeat && action == KeyAction::ToggleMode;
  // Enter and Esc repeats used to be swallowed with no second voice message.
  const bool voice_repeat = state.repeat && (action == KeyAction::VoiceEnter || action == KeyAction::VoiceEscape);

  if (!down) {
    if (shift_repeat || voice_repeat) {
      if (eaten != nullptr) *eaten = TRUE;
      return S_OK;
    }
    if (action == KeyAction::ToRime && rime_failed_) return S_OK;
    if (NeedsVoicePipe(action) && !PipeConnected()) return S_OK;
    if (eaten != nullptr) *eaten = EatsKey(action) ? TRUE : FALSE;
    return S_OK;
  }

  if (shift_repeat || voice_repeat) {
    eaten_vk_ = static_cast<int>(vk);
    if (eaten != nullptr) *eaten = TRUE;
    return S_OK;
  }
  // OpenVoice clears the composition inside Apply. Skip that when the pipe is down
  // so the pinyin stays and the hotkey falls through, matching the old voice path.
  if (action == KeyAction::OpenVoice && !PipeConnected()) return S_OK;
  if (action == KeyAction::ToRime && !EnsureRime()) return S_OK;

  const KeyAction decided = pinyin_.Apply(state, hotkey).action;
  switch (decided) {
    case KeyAction::Pass:
      break;
    case KeyAction::PassAfterClear:
      RunComposition(context, CompositionEdit::Cancel, L"");
      bar_.Hide();
      break;
    case KeyAction::ToRime:
      SyncAfterRime(context);
      eaten_vk_ = static_cast<int>(vk);
      if (eaten != nullptr) *eaten = TRUE;
      break;
    case KeyAction::ToggleMode:
      RunComposition(context, CompositionEdit::Cancel, L"");
      bar_.Hide();
      eaten_vk_ = static_cast<int>(vk);
      if (eaten != nullptr) *eaten = TRUE;
      break;
    case KeyAction::OpenVoice:
      RunComposition(context, CompositionEdit::Cancel, L"");
      bar_.Hide();
      SendVoice(MsgType::StartListen, vk, eaten);
      break;
    case KeyAction::VoiceEnter:
      SendVoice(MsgType::Enter, vk, eaten);
      break;
    case KeyAction::VoiceEscape:
      SendVoice(MsgType::Cancel, vk, eaten);
      break;
    case KeyAction::Consume:
      eaten_vk_ = static_cast<int>(vk);
      if (eaten != nullptr) *eaten = TRUE;
      break;
  }
  return S_OK;
}

HRESULT VoiceService::OnKeyUp(WPARAM vk, WINBOOL* eaten, bool down) {
  const bool eat = eaten_vk_ != 0 && eaten_vk_ == static_cast<int>(vk);
  if (eat && down) eaten_vk_ = 0;
  if (eaten != nullptr) *eaten = eat ? TRUE : FALSE;
  return S_OK;
}

class Factory : public IClassFactory {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IClassFactory)) {
      *out = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return static_cast<ULONG>(left);
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    auto* service = new VoiceService();
    const HRESULT hr = service->QueryInterface(iid, out);
    service->Release();
    return hr;
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }

 private:
  LONG refs_ = 1;
};

}  // namespace

HRESULT CreateVoiceImeFactory(const GUID& iid, void** object) {
  auto* factory = new Factory();
  const HRESULT hr = factory->QueryInterface(iid, object);
  factory->Release();
  return hr;
}
