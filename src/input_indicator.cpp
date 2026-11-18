#include <initguid.h>
#include "input_indicator.hpp"
#include "brand_icon.hpp"
#include "product.hpp"
#include <oleauto.h>
#include <ocidl.h>
#include <olectl.h>
#include <string>

InputIndicator::InputIndicator(HINSTANCE module,Toggle toggle,void* owner,InputMode mode,Enabled enabled)
    :module_(module),toggle_(toggle),query_enabled_(enabled),owner_(owner),mode_(mode) {}
InputIndicator::~InputIndicator() { if(sink_) sink_->Release(); }
HRESULT InputIndicator::Attach(ITfThreadMgr* manager) {
  if(!manager || manager_) return E_INVALIDARG;
  HRESULT hr=manager->QueryInterface(IID_ITfLangBarItemMgr,reinterpret_cast<void**>(&manager_));
  if(FAILED(hr)) return hr;
  hr=manager_->AddItem(static_cast<ITfLangBarItemButton*>(this));
  if(FAILED(hr)) { manager_->Release();manager_=nullptr; }
  return hr;
}
void InputIndicator::Detach() {
  toggle_=nullptr;query_enabled_=nullptr;owner_=nullptr;enabled_=false;
  auto* manager=manager_;manager_=nullptr;
  if(manager) { manager->RemoveItem(static_cast<ITfLangBarItemButton*>(this));manager->Release(); }
  if(sink_) { auto* sink=sink_;sink_=nullptr;sink->Release(); }
}
HRESULT InputIndicator::QueryInterface(REFIID iid,void** out) {
  if(!out)return E_POINTER;
  *out=nullptr;
  if(iid==IID_IUnknown || iid==IID_ITfLangBarItem || iid==kLangBarButtonIid)
    *out=static_cast<ITfLangBarItemButton*>(this);
  else if(iid==IID_ITfSource) *out=static_cast<ITfSource*>(this);
  if(!*out)return E_NOINTERFACE;
  AddRef();return S_OK;
}
ULONG InputIndicator::AddRef() { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
ULONG InputIndicator::Release() {
  const LONG refs=InterlockedDecrement(&refs_);if(!refs)delete this;return static_cast<ULONG>(refs);
}
HRESULT InputIndicator::GetInfo(TF_LANGBARITEMINFO* info) {
  if(!info)return E_POINTER;
  *info={};info->clsidService=CLSID_VoiceIme;info->guidItem=kSystemInputModeItem;
  info->dwStyle=TF_LBI_STYLE_BTN_BUTTON|TF_LBI_STYLE_SHOWNINTRAY;
  lstrcpynW(info->szDescription,kProductDisplayName,TF_LBI_DESC_MAXLEN);return S_OK;
}
HRESULT InputIndicator::GetStatus(DWORD* status) {
  if(!status)return E_POINTER;
  // Some TSF hosts defer or omit focus notifications. Query the current context
  // when the shell asks, rather than presenting a stale enabled state.
  const bool enabled=query_enabled_ ? query_enabled_(owner_) : enabled_;
  *status=enabled ? 0 : TF_LBI_STATUS_DISABLED;return S_OK;
}
HRESULT InputIndicator::Show(BOOL) { return S_OK; }
HRESULT InputIndicator::GetTooltipString(BSTR* text) {
  if(!text)return E_POINTER;
  std::wstring value=std::wstring(kProductDisplayName)+L" · Simple Voice\n";
  if (mode_ == InputMode::Chinese) {
    value += ascii_punctuation_ ? L"中文输入 · 英文标点" : L"中文输入 · 中文标点";
    value += L"\nShift 切换英文，Ctrl + . 切换标点";
  } else {
    value += L"英文直输 · Shift 切换中文";
  }
  *text=SysAllocString(value.c_str());return *text ? S_OK : E_OUTOFMEMORY;
}
HRESULT InputIndicator::GetText(BSTR* text) {
  if(!text)return E_POINTER;
  *text=SysAllocString(mode_==InputMode::Chinese ? L"中" : L"A");return *text ? S_OK : E_OUTOFMEMORY;
}
HRESULT InputIndicator::GetIcon(HICON* icon) {
  if(!icon)return E_POINTER;
  const bool white=ShellUsesLightIcons();
  const int resource=mode_==InputMode::Chinese ? (white?IDI_MODE_CHINESE_LIGHT:IDI_MODE_CHINESE)
                                             : (white?IDI_MODE_ENGLISH_LIGHT:IDI_MODE_ENGLISH);
  *icon=LoadProductIcon(module_,resource,ShellIconPixels());
  return *icon ? S_OK : E_FAIL; // TSF owns this copy and calls DestroyIcon.
}
HRESULT InputIndicator::OnClick(TfLBIClick click,POINT,const RECT*) {
  if(click!=TF_LBI_CLK_LEFT)return S_OK;
  DWORD status=0;GetStatus(&status);
  return !(status&TF_LBI_STATUS_DISABLED) && toggle_ && toggle_(owner_) ? S_OK : S_FALSE;
}
HRESULT InputIndicator::InitMenu(ITfMenu*) { return S_OK; }
HRESULT InputIndicator::OnMenuSelect(UINT) { return E_NOTIMPL; }
HRESULT InputIndicator::AdviseSink(REFIID iid,IUnknown* object,DWORD* cookie) {
  if(!object || !cookie)return E_INVALIDARG;
  *cookie=TF_INVALID_COOKIE;
  if(iid!=IID_ITfLangBarItemSink)return CONNECT_E_CANNOTCONNECT;
  if(sink_)return CONNECT_E_ADVISELIMIT;
  HRESULT hr=object->QueryInterface(IID_ITfLangBarItemSink,reinterpret_cast<void**>(&sink_));
  if(FAILED(hr))return hr;
  if(++cookie_==TF_INVALID_COOKIE || cookie_==0)cookie_=1;
  *cookie=cookie_;return S_OK;
}
HRESULT InputIndicator::UnadviseSink(DWORD cookie) {
  if(!sink_ || cookie!=cookie_)return CONNECT_E_NOCONNECTION;
  auto* sink=sink_;sink_=nullptr;sink->Release();return S_OK;
}
void InputIndicator::Notify(DWORD flags) {
  auto* sink=sink_;
  if(sink) {sink->AddRef();sink->OnUpdate(flags);sink->Release();}
}
void InputIndicator::SetMode(InputMode mode, bool ascii_punctuation) {
  if (mode_ == mode && ascii_punctuation_ == ascii_punctuation) return;
  DWORD flags = TF_LBI_TOOLTIP;
  if (mode_ != mode) flags |= TF_LBI_ICON | TF_LBI_TEXT;
  mode_ = mode;
  ascii_punctuation_ = ascii_punctuation;
  Notify(flags);
}
void InputIndicator::SetEnabled(bool enabled) {
  if(enabled_==enabled)return;
  enabled_=enabled;Notify(TF_LBI_STATUS);
}
void InputIndicator::RefreshIcon() { Notify(TF_LBI_ICON); }
