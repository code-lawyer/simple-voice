#include <initguid.h>

#include "tsf_service.hpp"

#include "candidate_bar.hpp"
#include "input_indicator.hpp"
#include "ui_theme.hpp"
#include "hotkey.hpp"
#include "settings.hpp"
#include "ipc.hpp"
#include "pipe_io.hpp"
#include "ipc_security.hpp"
#include "register_status.hpp"
#include "rime_session.hpp"
#include "utf8.hpp"

#include <msctf.h>
#include <oleauto.h>

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>

// MinGW msctf.h declares the info and enum interfaces and omits the provider.
// This IID is the Windows SDK msctf.idl uuid, not a new identifier.
DEFINE_GUID(IID_ITfDisplayAttributeProvider, 0xfee47777, 0x163c, 0x4769, 0x99, 0x6a, 0x6e, 0x9c, 0x50, 0xad, 0x8f, 0x54);
#ifndef __ITfDisplayAttributeProvider_INTERFACE_DEFINED__
#define __ITfDisplayAttributeProvider_INTERFACE_DEFINED__
MIDL_INTERFACE("fee47777-163c-4769-996a-6e9c50ad8f54")
ITfDisplayAttributeProvider : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) = 0;
};
#endif

// MinGW's msctf.h omits this Windows SDK macro. The SDK value is 0.
#ifndef TF_INVALID_EDIT_COOKIE
#define TF_INVALID_EDIT_COOKIE ((TfEditCookie)0)
#endif

namespace {

constexpr UINT kCommitMessage = WM_APP + 1;
constexpr UINT kCaretMessage = WM_APP + 2;
constexpr UINT kTrackingMessage = WM_APP + 3;
constexpr UINT_PTR kTrackingTimer = 1;
struct IncomingCommit {
  std::wstring text;
  std::uint64_t id = 0, connection = 0;
};

std::wstring ConfiguredHotkeyPath() {
  wchar_t local[MAX_PATH];
  const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (count == 0 || count >= MAX_PATH) return {};
  return std::wstring(local, count) + L"\\VoiceIme\\config.json";
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

bool IsInputMethodEditor(HWND focus) {
  HWND root=focus?GetAncestor(focus,GA_ROOT):nullptr;
  if(!root) return false;
  wchar_t name[80]{}; GetClassNameW(root,name,80);
  return lstrcmpW(name,L"SimpleVoicePreviewEditor")==0 || lstrcmpW(name,L"SimpleVoicePreferences")==0;
}

bool CanUseDesktopRuntime() {
  // Packaged desktop hosts such as Windows Terminal can use the desktop
  // runtime. AppContainers need a separate data and IPC integration.
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD app_container = 0;
  DWORD written = 0;
  const BOOL queried = GetTokenInformation(token, TokenIsAppContainer,
      &app_container, sizeof(app_container), &written);
  CloseHandle(token);
  return queried && app_container == 0;
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

TF_DISPLAYATTRIBUTE SolidInputUnderline() {
  TF_DISPLAYATTRIBUTE attr{};
  attr.crText.type = TF_CT_NONE;
  attr.crBk.type = TF_CT_NONE;
  attr.lsStyle = TF_LS_SOLID;
  attr.fBoldLine = FALSE;
  attr.crLine.type = TF_CT_SYSCOLOR;
  attr.crLine.nIndex = COLOR_WINDOWTEXT;
  attr.bAttr = TF_ATTR_INPUT;
  return attr;
}

// No SDK constant names an input underline. CLSID_VoiceIme is the one attribute this service publishes.
TfGuidAtom InputAttributeAtom() {
  static TfGuidAtom cached = 0;
  if (cached != 0) return cached;
  ITfCategoryMgr* categories = nullptr;
  const HRESULT created = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                           reinterpret_cast<void**>(&categories));
  if (FAILED(created) || categories == nullptr) return 0;
  TfGuidAtom atom = 0;
  const HRESULT registered = categories->RegisterGUID(CLSID_VoiceIme, &atom);
  categories->Release();
  if (FAILED(registered) || atom == 0) return 0;
  cached = atom;
  return cached;
}

void SetInputAttribute(TfEditCookie cookie, ITfContext* context, ITfRange* range, bool underline) {
  if (context == nullptr || range == nullptr) return;
  ITfProperty* prop = nullptr;
  if (FAILED(context->GetProperty(GUID_PROP_ATTRIBUTE, &prop)) || prop == nullptr) return;
  if (!underline) {
    prop->Clear(cookie, range);
    prop->Release();
    return;
  }
  const TfGuidAtom atom = InputAttributeAtom();
  if (atom == 0) {
    prop->Release();
    return;
  }
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_I4;
  value.lVal = static_cast<LONG>(atom);
  prop->SetValue(cookie, range, &value);
  prop->Release();
}

class InputAttribute : public ITfDisplayAttributeInfo {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfDisplayAttributeInfo)) {
      *out = static_cast<ITfDisplayAttributeInfo*>(this);
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
  HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override {
    if (guid == nullptr) return E_INVALIDARG;
    *guid = CLSID_VoiceIme;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override {
    if (description == nullptr) return E_INVALIDARG;
    *description = SysAllocString(L"uncommitted pinyin");
    return *description == nullptr ? E_OUTOFMEMORY : S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetAttributeInfo(TF_DISPLAYATTRIBUTE* attr) override {
    if (attr == nullptr) return E_INVALIDARG;
    *attr = attr_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetAttributeInfo(const TF_DISPLAYATTRIBUTE* attr) override {
    if (attr == nullptr) return E_INVALIDARG;
    attr_ = *attr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    attr_ = SolidInputUnderline();
    return S_OK;
  }

 private:
  LONG refs_ = 1;
  TF_DISPLAYATTRIBUTE attr_ = SolidInputUnderline();
};

class InputAttributeEnum : public IEnumTfDisplayAttributeInfo {
 public:
  explicit InputAttributeEnum(ULONG index) : index_(index) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_IEnumTfDisplayAttributeInfo)) {
      *out = static_cast<IEnumTfDisplayAttributeInfo*>(this);
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
  HRESULT STDMETHODCALLTYPE Clone(IEnumTfDisplayAttributeInfo** out) override {
    if (out == nullptr) return E_INVALIDARG;
    *out = new InputAttributeEnum(index_);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, ITfDisplayAttributeInfo** info, ULONG* fetched) override {
    if (info == nullptr || (fetched == nullptr && count != 1)) return E_INVALIDARG;
    ULONG written = 0;
    if (index_ == 0 && count > 0) {
      info[0] = new InputAttribute();
      index_ = 1;
      written = 1;
    }
    if (fetched != nullptr) *fetched = written;
    return written == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    index_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    if (index_ >= 1 || count == 0) return index_ >= 1 && count > 0 ? S_FALSE : S_OK;
    index_ = 1;
    return count == 1 ? S_OK : S_FALSE;
  }

 private:
  LONG refs_ = 1;
  ULONG index_ = 0;
};

class VoiceService : public ITfTextInputProcessor {
 public:
  VoiceService()
      : key_sink_(this), thread_sink_(this), composition_sink_(this), attribute_provider_(this), pinyin_(&rime_) {
    bar_.SetSettingsHandler(&VoiceService::SettingsClicked, this);
    bar_.SetCandidateHandler(&VoiceService::CandidateClicked, this);
  }
  ~VoiceService() {
    bar_.SetSettingsHandler(nullptr, nullptr);
    bar_.SetCandidateHandler(nullptr, nullptr);
    Deactivate();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (IsEqualGUID(iid, IID_IUnknown) || IsEqualGUID(iid, IID_ITfTextInputProcessor)) {
      *out = static_cast<ITfTextInputProcessor*>(this);
      AddRef();
      return S_OK;
    }
    if (IsEqualGUID(iid, IID_ITfDisplayAttributeProvider)) {
      *out = static_cast<ITfDisplayAttributeProvider*>(&attribute_provider_);
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
#ifdef VOICEIME_TESTING
  void SetVoiceActiveForTest(bool active) { active_=active; }
  HWND IndicatorMessageWindowForTest() const { return host_; }
  Hotkey CachedHotkeyForTest() const { return hotkey_cache_.value(); }
#endif
  HRESULT OnKeyUp(ITfContext* context, WPARAM vk, WINBOOL* eaten, bool down);
  void OnForeground(bool foreground) {
    foreground_ = foreground;
    last_caret_sent_.clear();
    if (!foreground) bar_.Hide();
    UpdateTracking();
  }

  void ReadLoop();
  void HandleCommit(const IncomingCommit& request);
  void HandleCaret(bool force = false);

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
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext* context, WPARAM vk, LPARAM, WINBOOL* eaten) override {
      return owner_->OnKeyUp(context, vk, eaten, false);
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM vk, LPARAM lparam, WINBOOL* eaten) override {
      return owner_->OnKey(context, vk, lparam, eaten, true);
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM vk, LPARAM, WINBOOL* eaten) override {
      return owner_->OnKeyUp(context, vk, eaten, true);
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
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* current, ITfDocumentMgr* previous) override {
      owner_->OnDocumentFocus(current, previous);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext*) override { return S_OK; }

   private:
    VoiceService* owner_;
  };

  class AttributeProvider : public ITfDisplayAttributeProvider {
   public:
    explicit AttributeProvider(VoiceService* owner) : owner_(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override { return owner_->QueryInterface(iid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { return owner_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return owner_->Release(); }
    HRESULT STDMETHODCALLTYPE EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** out) override {
      if (out == nullptr) return E_INVALIDARG;
      *out = new InputAttributeEnum(0);
      return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** info) override {
      if (info == nullptr) return E_INVALIDARG;
      *info = nullptr;
      if (!IsEqualGUID(guid, CLSID_VoiceIme)) return E_INVALIDARG;
      *info = new InputAttribute();
      return S_OK;
    }

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
      ITfRange* inserted = nullptr;
      const HRESULT hr = insert->InsertTextAtSelection(cookie, 0, text.c_str(),
                                                        static_cast<LONG>(text.size()), &inserted);
      if (SUCCEEDED(hr) && inserted != nullptr) MoveCaretToEnd(cookie, context, inserted);
      if (inserted != nullptr) inserted->Release();
      insert->Release();
      return hr;
    }
  };

  struct CaretSession : StackSession {
    ITfContext* context = nullptr;
    ITfRange* anchor_range = nullptr;
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
      UiDpiScope coordinates;
      TF_SELECTION selection{};
      ULONG fetched = 0;
      HRESULT selected = E_FAIL;
      if (anchor_range) {
        selected = anchor_range->Clone(&selection.range);
        fetched = SUCCEEDED(selected) ? 1 : 0;
        selection.style.ase = TF_AE_START;
      } else selected = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &fetched);
      ITfContextView* view = nullptr;
      if (FAILED(context->GetActiveView(&view)) || view == nullptr) {
        if (SUCCEEDED(selected) && selection.range != nullptr) selection.range->Release();
        return S_OK;
      }
      if (SUCCEEDED(selected) && fetched > 0 && selection.range != nullptr) {
        selection.range->Collapse(cookie, selection.style.ase == TF_AE_START ? TF_ANCHOR_START : TF_ANCHOR_END);
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
  bool WriteClient(const Message& message, std::uint64_t connection = 0);
  void RequestSettings();
  void UpdateInputIndicator();
  static bool CanToggleInputMode(void* self);
  static bool ToggleInputMode(void* self);
  static void SettingsClicked(void* self);
  static void CandidateClicked(void* self, int index);
  bool EnsureRime();
  bool PipeConnected();
  KeyState MakeKeyState(ITfContext* context, WPARAM vk, LPARAM lparam);
  void SendVoice(MsgType type, WPARAM vk, WINBOOL* eaten);
  void RunComposition(ITfContext* context, CompositionEdit op, const std::wstring& text);
  void DropUncommitted(ITfContext* context, const KeyState& state, const Hotkey& hotkey);
  void EditComposition(TfEditCookie cookie, ITfContext* context, CompositionEdit op, const std::wstring& text);
  bool StartComposition(TfEditCookie cookie, ITfContext* context);
  void SetCompositionText(TfEditCookie cookie, ITfContext* context, const std::wstring& text);
  void CommitComposition(TfEditCookie cookie, ITfContext* context, const std::wstring& text);
  void CancelComposition(TfEditCookie cookie);
  void ReleaseCompositionPointer();
  void OnCompositionTerminated(TfEditCookie cookie, ITfComposition* composition);
  void ShowCandidates(ITfContext* context);
  void SyncAfterRime(ITfContext* context);
  void CancelFocusComposition();
  void OnDocumentFocus(ITfDocumentMgr* current, ITfDocumentMgr* previous);
  void LaunchEngine();
  void EnsureWindow();
  void UpdateTracking();
  void TrackCaret();
  bool OwnsForeground() const;
  ITfContext* FocusContext() const;
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

  KeySink key_sink_;
  ThreadSink thread_sink_;
  CompositionSink composition_sink_;
  AttributeProvider attribute_provider_;
  RimeSession rime_;
  PinyinController pinyin_;
  CandidateBar bar_;
  InputIndicator* input_indicator_ = nullptr;
  ITfComposition* composition_ = nullptr;
  ITfContext* composition_context_ = nullptr;
  bool ending_composition_ = false;
  bool rime_ready_ = false;
  bool rime_failed_ = false;
  LONG refs_ = 1;
  ITfThreadMgr* thread_mgr_ = nullptr;
  TfClientId client_id_ = 0;
  DWORD thread_cookie_ = 0;
  bool advised_keys_ = false;
  std::atomic<bool> foreground_{true};
  std::atomic<bool> active_{false};
  std::atomic<std::uint64_t> state_id_{0};
  HotkeyCache hotkey_cache_;
  std::atomic<std::uint64_t> connection_generation_{0}, cancelled_commit_{0};
  std::uint64_t handled_connection_ = ~std::uint64_t{0}, handled_id_ = 0;
  bool handled_success_ = false;
  bool pending_toggle_ = false;
  bool tracking_ = false;
  bool candidate_anchor_known_ = false;
  Rect candidate_anchor_{}, candidate_window_{};
  std::string last_caret_sent_;
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
  if (FAILED(context->GetStatus(&status))) return true;
  if ((status.dwDynamicFlags & TF_SD_READONLY) != 0) return false;
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

bool VoiceService::WriteClient(const Message& message, std::uint64_t connection) {
  std::lock_guard<std::mutex> lock(write_mu_);
  if (connection != 0 && connection != connection_generation_.load()) return false;
  if (pipe_ == INVALID_HANDLE_VALUE || write_event_ == nullptr) return false;
  Message current = message;
  if (IsVoiceControl(current.type)) current.state_id = state_id_.load();
  auto bytes = Encode(current);
  if (bytes.empty()) return false;
  outgoing_.push_back(std::move(bytes));
  SetEvent(write_event_);
  return true;
}

// The TSF target is Windows 7. That header set does not declare this Vista pipe query.
static BOOL QueryPipeServerProcessId(HANDLE pipe, ULONG* server) {
  using Fn = BOOL(WINAPI*)(HANDLE, ULONG*);
  static Fn fn = nullptr;
  static bool resolved = false;
  if (!resolved) {
    resolved = true;
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel != nullptr) fn = reinterpret_cast<Fn>(GetProcAddress(kernel, "GetNamedPipeServerProcessId"));
  }
  if (fn == nullptr || server == nullptr) return FALSE;
  return fn(pipe, server);
}

void VoiceService::RequestSettings() {
  ULONG server = 0;
  bool have_server = false;
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    if (pipe_ != INVALID_HANDLE_VALUE && QueryPipeServerProcessId(pipe_, &server)) have_server = server != 0;
  }
  if (have_server) AllowSetForegroundWindow(server);
  Message message;
  message.type = MsgType::ShowSettings;
  WriteClient(message);
}

void VoiceService::SettingsClicked(void* self) {
  static_cast<VoiceService*>(self)->RequestSettings();
}

void VoiceService::UpdateInputIndicator() {
  if(!input_indicator_)return;
  input_indicator_->SetMode(pinyin_.mode());
  ITfContext* context=FocusContext();
  input_indicator_->SetEnabled(DocumentAcceptsText(context) && !active_);
  if(context)context->Release();
}

bool VoiceService::CanToggleInputMode(void* self) {
  auto* service=static_cast<VoiceService*>(self);
  ITfContext* context=service->FocusContext();
  const bool allowed=service->DocumentAcceptsText(context) && !service->active_;
  if(context)context->Release();
  return allowed;
}

bool VoiceService::ToggleInputMode(void* self) {
  auto* service=static_cast<VoiceService*>(self);
  ITfContext* context=service->FocusContext();
  const bool allowed=service->DocumentAcceptsText(context) && !service->active_;
  if(allowed) {
    service->RunComposition(context,CompositionEdit::Cancel,L"");
    service->pinyin_.ToggleMode();
    service->pending_toggle_=false;
    service->bar_.Hide();
    service->UpdateInputIndicator();
  }
  if(context)context->Release();
  return allowed;
}

void VoiceService::CandidateClicked(void* self, int index) {
  auto* service = static_cast<VoiceService*>(self);
  if (index < 0 || index >= static_cast<int>(service->pinyin_.view().page.size())) return;
  ITfContext* context = service->FocusContext();
  if (context != nullptr && service->DocumentAcceptsText(context)) {
    service->rime_.ProcessKey(0x31 + index, false);
    service->SyncAfterRime(context);
  }
  if (context != nullptr) context->Release();
}

void VoiceService::LaunchEngine() {
#ifdef VOICEIME_TESTING
  return;  // Integration executable must never start the installed engine.
#endif
  const auto name = VoiceMutexName();
  if (name.empty()) return;
  HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
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
  // Hidden, nonactivating top-level window receives shell theme/display broadcasts;
  // HWND_MESSAGE windows do not. It is never shown and has no taskbar button.
  host_ = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE, L"VoiceImeTsfHost", L"", WS_POPUP,
      0,0,0,0,nullptr,nullptr,VoiceImeModule(),this);
}

LRESULT CALLBACK VoiceService::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* created = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  auto* self = reinterpret_cast<VoiceService*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
  const auto hotkey_changed = HotkeyChangedMessage();
  if (hotkey_changed && message == hotkey_changed) { self->hotkey_cache_.Reload(); return 0; }
  if (message == kCommitMessage) {
    auto* text = reinterpret_cast<IncomingCommit*>(lparam);
    if (text != nullptr) {
      self->HandleCommit(*text);
      delete text;
    }
    return 0;
  }
  if (message == kCaretMessage) {
    self->HandleCaret(true);
    return 0;
  }
  if (message == kTrackingMessage) {
    self->UpdateInputIndicator();self->UpdateTracking();self->TrackCaret();return 0;
  }
  if (message == WM_TIMER && wparam == kTrackingTimer) { self->TrackCaret(); return 0; }
  if(message==WM_SETTINGCHANGE || message==WM_THEMECHANGED || message==WM_DISPLAYCHANGE || message==WM_DPICHANGED) {
    if(self->input_indicator_)self->input_indicator_->RefreshIcon();
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

void VoiceService::HandleCommit(const IncomingCommit& request) {
  if (request.id == 0 || request.connection != connection_generation_.load()) return;
  if (handled_connection_ != request.connection) {
    handled_connection_ = request.connection; handled_id_ = 0;
  }
  if (request.id < handled_id_) return;
  if (request.id == handled_id_) {
    Message ack; ack.type = MsgType::InsertResult; ack.request_id = request.id;
    ack.text = handled_success_ ? "ok" : "fail";
    WriteClient(ack, request.connection); return;
  }
  bool inserted = false;
  ITfContext* context = request.id > cancelled_commit_.load() && !request.text.empty() ? FocusContext() : nullptr;
  if (context != nullptr) {
    if (DocumentAcceptsText(context)) {
    InsertSession session;
    session.context = context;
    session.text = request.text;
    HRESULT session_hr = E_FAIL;
    const auto request_hr = context->RequestEditSession(client_id_, &session, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
    inserted = SUCCEEDED(request_hr) && SUCCEEDED(session_hr);
    }
    context->Release();
  }
  handled_id_ = request.id; handled_success_ = inserted;
  if (inserted) {active_=false;UpdateInputIndicator();}
  Message ack;
  ack.type = MsgType::InsertResult;
  ack.request_id = request.id;
  ack.text = inserted ? "ok" : "fail";
  WriteClient(ack, request.connection);
}

bool VoiceService::OwnsForeground() const {
  if (!foreground_) return false;
#ifndef VOICEIME_TESTING
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  if (process != GetCurrentProcessId()) return false;
#endif
  return true;
}

void VoiceService::UpdateTracking() {
  if (!host_) return;
  const bool wanted = foreground_ && (active_ || pinyin_.view().composing);
  if (wanted && !tracking_) tracking_ = SetTimer(host_, kTrackingTimer, 60, nullptr) != 0;
  if (!wanted && tracking_) { KillTimer(host_, kTrackingTimer); tracking_ = false; }
}

void VoiceService::TrackCaret() {
  UpdateTracking();
  if (!OwnsForeground()) { bar_.Hide(); last_caret_sent_.clear(); return; }
  if (active_) HandleCaret();
  else if (composition_context_ && pinyin_.view().composing) ShowCandidates(composition_context_);
}

void VoiceService::HandleCaret(bool force) {
  if (!OwnsForeground()) return;
  Message reply;
  reply.type = MsgType::Caret;
  ITfContext* context = FocusContext();
  if (context != nullptr) {
    if (!DocumentAcceptsText(context)) { context->Release(); return; }
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
  // Chromium may briefly return TS_E_NOLAYOUT after an edit. Retain the last
  // valid location rather than moving a visible popup to the fallback corner.
  if (reply.caret_h <= 0 || reply.caret_w <= 0) return;
  const auto bytes = Encode(reply);
  if ((force || bytes != last_caret_sent_) && WriteClient(reply)) last_caret_sent_ = bytes;
}

void VoiceService::ReadLoop() {
  auto deliver = [this](std::string& pending) {
    for (;;) {
      Message message;
      std::size_t used = 0;
      const auto status = DecodeFrame(pending, &message, &used);
      if (status == DecodeStatus::Invalid) return false;
      if (status == DecodeStatus::Incomplete) return true;
      pending.erase(0, used);
      if (message.type == MsgType::InsertText && host_ != nullptr) {
        auto* text = new IncomingCommit{Utf8ToWide(message.text), message.request_id, connection_generation_.load()};
        if (!PostMessageW(host_, kCommitMessage, 0, reinterpret_cast<LPARAM>(text))) delete text;
      } else if (message.type == MsgType::CancelInsert) {
        cancelled_commit_.store(std::max(cancelled_commit_.load(), message.request_id));
      } else if (message.type == MsgType::Phase) {
        state_id_ = message.state_id;
        active_ = !message.text.empty() && message.text != "0";
        if (host_) PostMessageW(host_, kTrackingMessage, 0, 0);
      } else if (message.type == MsgType::QueryCaret && host_ != nullptr && foreground_) {
        PostMessageW(host_, kCaretMessage, 0, 0);
      }
    }
  };
  while (stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    LaunchEngine();
    // One thread owns the handle. A synchronous read and write on the same
    // pipe deadlock, so the UI thread only queues bytes and this loop writes them.
#ifdef VOICEIME_TESTING
    const auto pipe_name = L"\\\\.\\pipe\\VoiceImeTsfQA_" + std::to_wstring(GetCurrentProcessId());
#else
    const auto pipe_name = VoicePipeName();
#endif
    HANDLE pipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(stop_event_, 100) == WAIT_OBJECT_0) return;
      continue;
    }
    auto server = PipeServerPeer(pipe);
#ifdef VOICEIME_TESTING
    const auto expected_engine = ModuleDirectory() + L"\\voiceime_tsf_integration.exe";
#else
    const auto expected_engine = ModuleDirectory() + L"\\voiceime_engine.exe";
#endif
    Message initial;
    std::string buffer;
    if (!server || !server->is_image(expected_engine) || !ClientHandshake(pipe, host_, stop_event_, initial, buffer)) {
      CloseHandle(pipe);
      if (WaitForSingleObject(stop_event_, 100) == WAIT_OBJECT_0) return;
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      ++connection_generation_;
      cancelled_commit_ = 0;
      state_id_ = initial.state_id;
      active_ = initial.text != "0";
      pipe_ = pipe;
    }
    if (host_) PostMessageW(host_, kTrackingMessage, 0, 0);
    bool failed = !deliver(buffer);
    while (!failed && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
      std::vector<std::string> mine;
      {
        std::lock_guard<std::mutex> lock(write_mu_);
        mine.swap(outgoing_);
        if (outgoing_.empty() && write_event_ != nullptr) ResetEvent(write_event_);
      }
      if (!mine.empty()) {
        for (const std::string& bytes : mine) {
          if (!WritePipeBytes(pipe, bytes, stop_event_)) { failed = true; break; }
        }
        continue;
      }
      const PipeReadResult received = ReadPipeChunk(pipe, write_event_, stop_event_);
      buffer += received.bytes;
      failed = !deliver(buffer) || received.status == PipeReadStatus::Closed;
    }
    const bool stopping = stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0;
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      ++connection_generation_;
      outgoing_.clear();
      if (!stopping && pipe_ == pipe) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
      }
    }
    active_ = false;
    state_id_ = 0;
    if (host_) PostMessageW(host_, kTrackingMessage, 0, 0);
    if (stopping) return;
  }
}

DWORD WINAPI VoiceReader(void* param) {
  static_cast<VoiceService*>(param)->ReadLoop();
  return 0;
}

HRESULT VoiceService::Activate(ITfThreadMgr* manager, TfClientId client_id) {
  if (manager == nullptr) return E_INVALIDARG;
  if (thread_mgr_ != nullptr) return E_UNEXPECTED;
  if (!CanUseDesktopRuntime()) return E_ACCESSDENIED;
  hotkey_cache_.Load(ConfiguredHotkeyPath());
  thread_mgr_ = manager;
  thread_mgr_->AddRef();
  client_id_ = client_id;
  EnsureWindow();
  input_indicator_=new(std::nothrow) InputIndicator(VoiceImeModule(),&VoiceService::ToggleInputMode,this,pinyin_.mode(),&VoiceService::CanToggleInputMode);
  if(input_indicator_ && FAILED(input_indicator_->Attach(manager))) {
    input_indicator_->Release();input_indicator_=nullptr;
  }
  UpdateInputIndicator();
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
  if(input_indicator_) {
    input_indicator_->Detach();input_indicator_->Release();input_indicator_=nullptr;
  }
  if (host_) KillTimer(host_, kTrackingTimer);
  tracking_ = false;
  last_caret_sent_.clear();
  candidate_anchor_known_ = false;
  CancelFocusComposition();
  bar_.Hide();
  rime_.Shutdown();
  rime_ready_ = false;
  rime_failed_ = false;
  pending_toggle_ = false;
  stop_reader_ = true;
  if (stop_event_ != nullptr) SetEvent(stop_event_);
  if (write_event_ != nullptr) SetEvent(write_event_);
  HANDLE reader = nullptr;
  {
    std::lock_guard<std::mutex> lock(write_mu_);
    reader = reader_;
  }
  if (reader != nullptr) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    {
      std::lock_guard<std::mutex> lock(write_mu_);
      pipe = pipe_;
    }
    if (pipe != INVALID_HANDLE_VALUE) CancelIoEx(pipe, nullptr);
    // Every pending pipe operation observes stop_event_. Keep its storage alive
    // until the reader has drained cancellation and actually exited.
    WaitForSingleObject(reader, INFINITE);
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
    MSG pending{};
    while (PeekMessageW(&pending, host_, kCommitMessage, kCommitMessage, PM_REMOVE)) {
      delete reinterpret_cast<IncomingCommit*>(pending.lParam);
    }
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

HRESULT STDMETHODCALLTYPE VoiceService::CompositionSink::OnCompositionTerminated(TfEditCookie cookie,
                                                                                  ITfComposition* composition) {
  owner_->OnCompositionTerminated(cookie, composition);
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
  // The preview and term editor are themselves editable documents. A global
  // pending voice session must never steal their Enter/Esc or disable pinyin.
  state.voice_active = active_ && !IsInputMethodEditor(GetFocus());
  state.composing = view.composing;
  state.candidate_count = static_cast<int>(view.page.size());
  auto held = [](int key) {
    return (GetAsyncKeyState(key) & 0x8000) != 0 || (GetKeyState(key) & 0x8000) != 0;
  };
  state.shift = held(VK_SHIFT) || held(VK_LSHIFT) || held(VK_RSHIFT);
  state.ctrl = held(VK_CONTROL) || held(VK_LCONTROL) || held(VK_RCONTROL);
  state.alt = held(VK_MENU) || held(VK_LMENU) || held(VK_RMENU);
  state.win = held(VK_LWIN) || held(VK_RWIN);
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
    if (type == MsgType::Enter || type == MsgType::Cancel) {active_=false;UpdateInputIndicator();}
    return;
  }
  if (type == MsgType::StartListen) active_ = true;
  if (type == MsgType::Cancel) active_ = false;
  UpdateInputIndicator();
  UpdateTracking();
  if (active_) HandleCaret(true);
  eaten_vk_ = static_cast<int>(vk);
  if (eaten != nullptr) *eaten = TRUE;
}

void VoiceService::DropUncommitted(ITfContext* context, const KeyState& state, const Hotkey& hotkey) {
  // PassAfterClear and a pipe-down OpenVoice only Clear() inside Apply.
  pinyin_.Apply(state, hotkey);
  RunComposition(context, CompositionEdit::Cancel, L"");
  bar_.Hide();
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
  composition_context_ = context;
  composition_context_->AddRef();
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
  SetInputAttribute(cookie, context, range, true);
  ITfRange* caret=nullptr;
  if(SUCCEEDED(range->Clone(&caret)) && caret) {
    caret->Collapse(cookie,TF_ANCHOR_START);
    const auto& view=pinyin_.view();
    const auto prefix=Utf8ToWide(view.preedit.substr(0,std::clamp(view.caret,0,static_cast<int>(view.preedit.size()))));
    LONG shifted=0;
    caret->ShiftStart(cookie,static_cast<LONG>(prefix.size()),&shifted,nullptr);
    caret->Collapse(cookie,TF_ANCHOR_START);
    TF_SELECTION selection{}; selection.range=caret; selection.style.ase=TF_AE_NONE;
    context->SetSelection(cookie,1,&selection); caret->Release();
  }
  range->Release();
}

void VoiceService::CommitComposition(TfEditCookie cookie, ITfContext* context, const std::wstring& text) {
  if (composition_ == nullptr && !text.empty() && !StartComposition(cookie, context)) return;
  if (composition_ == nullptr) return;
  ITfRange* range = nullptr;
  if (SUCCEEDED(composition_->GetRange(&range)) && range != nullptr) {
    if(FAILED(range->SetText(cookie, 0, text.c_str(), static_cast<LONG>(text.size())))) { range->Release(); return; }
    SetInputAttribute(cookie, context, range, false);
    MoveCaretToEnd(cookie, context, range);
    range->Release();
    if(!text.empty()) { Message used; used.type=MsgType::PersonalTermUsed; used.text=WideToUtf8(text); WriteClient(used); }
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
  candidate_anchor_known_ = false;
  ITfComposition* held = composition_;
  composition_ = nullptr;
  ITfContext* context = composition_context_;
  composition_context_ = nullptr;
  if (held != nullptr) held->Release();
  if (context != nullptr) context->Release();
}

void VoiceService::OnCompositionTerminated(TfEditCookie cookie, ITfComposition* composition) {
  // Our EndComposition already wrote the commit or cleared the range.
  if (!ending_composition_ && cookie != TF_INVALID_EDIT_COOKIE && composition != nullptr) {
    ITfRange* range = nullptr;
    if (SUCCEEDED(composition->GetRange(&range)) && range != nullptr) {
      range->SetText(cookie, TF_ST_CORRECTION, L"", 0);
      range->Release();
    }
  }
  ReleaseCompositionPointer();
  if (ending_composition_) return;
  rime_.Clear();
  bar_.Hide();
  UpdateTracking();
}

void VoiceService::ShowCandidates(ITfContext* context) {
  if (!OwnsForeground()) { bar_.Hide(); return; }
  const RimeView& view = pinyin_.view();
  Rect caret{0, 0, 0, 0};
  Rect window{0, 0, 0, 0};
  if (context != nullptr && client_id_ != 0) {
    CaretSession session;
    session.context = context;
    ITfRange* anchor = nullptr;
    if (composition_ && context == composition_context_ && SUCCEEDED(composition_->GetRange(&anchor)))
      session.anchor_range = anchor;
    HRESULT session_hr = E_FAIL;
    context->RequestEditSession(client_id_, &session, TF_ES_SYNC | TF_ES_READ, &session_hr);
    if (anchor) anchor->Release();
    if (SUCCEEDED(session_hr)) {
      caret = Rect{session.caret_x, session.caret_y, session.caret_w, session.caret_h};
      window = Rect{session.window_x, session.window_y, session.window_w, session.window_h};
    }
  }
  if (caret.w > 0 && caret.h > 0) {
    candidate_anchor_ = caret; candidate_window_ = window; candidate_anchor_known_ = true;
  } else if (candidate_anchor_known_) {
    caret = candidate_anchor_; window = candidate_window_;
  } else return; // Retry on the next tracking tick; do not flash at (0,0).
  bar_.Show(caret, window, view, pinyin_.mode());
}

void VoiceService::SyncAfterRime(ITfContext* context) {
  const RimeView& view = pinyin_.view();
  UpdateTracking();
  if (!view.composing) candidate_anchor_known_ = false;
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
  ITfContext* context = composition_context_;
  if (context != nullptr) {
    context->AddRef();
    RunComposition(context, CompositionEdit::Cancel, L"");
    context->Release();
  }
  ReleaseCompositionPointer();
}

void VoiceService::OnDocumentFocus(ITfDocumentMgr* current, ITfDocumentMgr* previous) {
  if (current != previous) {
    candidate_anchor_known_ = false;
    last_caret_sent_.clear();
    CancelFocusComposition();
    rime_.Clear();
    pending_toggle_ = false;
    bar_.Hide();
  }
  if (active_) HandleCaret();
  // During OnSetFocus, GetFocus can still return the previous document.
  // Use the document supplied by the notification, including a null focus.
  if(input_indicator_) {
    ITfContext* context=nullptr;
    if(current)current->GetTop(&context);
    input_indicator_->SetEnabled(DocumentAcceptsText(context) && !active_);
    if(context)context->Release();
  }
  UpdateTracking();
}

HRESULT VoiceService::OnKey(ITfContext* context, WPARAM vk, LPARAM lparam, WINBOOL* eaten, bool down) {
  if (eaten != nullptr) *eaten = FALSE;
  // Focus notifications may be deferred. Never use an old document's range
  // with the edit cookie supplied by a different context.
  if (composition_context_ != nullptr && composition_context_ != context) {
    CancelFocusComposition();
    rime_.Clear();
    bar_.Hide();
    pending_toggle_ = false;
  }
  const KeyState state = MakeKeyState(context, vk, lparam);
  const Hotkey hotkey = hotkey_cache_.value();
  const KeyAction action = RouteKey(state, hotkey).action;
  // Bare Shift is ToggleMode. Apply waits until keyup so Shift+letter stays in this mode.
  if (action == KeyAction::ToggleMode) {
    if (down && !state.repeat) pending_toggle_ = true;
    if (down) eaten_vk_ = static_cast<int>(vk);
    if (eaten != nullptr) *eaten = TRUE;
    return S_OK;
  }
  pending_toggle_ = false;
  // Enter and Esc repeats used to be swallowed with no second voice message.
  const bool voice_repeat = state.repeat && (action == KeyAction::VoiceEnter || action == KeyAction::VoiceEscape);

  if (!down) {
    if (voice_repeat) {
      if (eaten != nullptr) *eaten = TRUE;
      return S_OK;
    }
    if (action == KeyAction::ToRime && rime_failed_) return S_OK;
    // A host that sees eaten false does not call OnKeyDown, so the clear has to happen here.
    if (action == KeyAction::PassAfterClear || (action == KeyAction::OpenVoice && !PipeConnected())) {
      DropUncommitted(context, state, hotkey);
      return S_OK;
    }
    if (NeedsVoicePipe(action) && !PipeConnected()) return S_OK;
    if (eaten != nullptr) *eaten = EatsKey(action) ? TRUE : FALSE;
    return S_OK;
  }

  if (voice_repeat) {
    eaten_vk_ = static_cast<int>(vk);
    if (eaten != nullptr) *eaten = TRUE;
    return S_OK;
  }
  // The test call already dropped this. If this keydown still runs, drop again and send nothing.
  if (action == KeyAction::OpenVoice && !PipeConnected()) {
    if (composition_ != nullptr || pinyin_.view().composing) DropUncommitted(context, state, hotkey);
    return S_OK;
  }
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

HRESULT VoiceService::OnKeyUp(ITfContext* context, WPARAM vk, WINBOOL* eaten, bool down) {
  const bool shift = vk == 0x10 || vk == 0xA0 || vk == 0xA1;
  if (!down) {
    const bool eat = (pending_toggle_ && shift) || (eaten_vk_ != 0 && eaten_vk_ == static_cast<int>(vk));
    if (eaten != nullptr) *eaten = eat ? TRUE : FALSE;
    return S_OK;
  }
  if (pending_toggle_ && shift) {
    const KeyState state = MakeKeyState(context, vk, 0);
    const KeyAction decided = pinyin_.Apply(state, hotkey_cache_.value()).action;
    pending_toggle_ = false;
    if (decided == KeyAction::ToggleMode) {
      RunComposition(context, CompositionEdit::Cancel, L"");
      bar_.Hide();
      UpdateInputIndicator();
    }
  }
  const bool eat = eaten_vk_ != 0 && eaten_vk_ == static_cast<int>(vk);
  if (eat) eaten_vk_ = 0;
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
