#include "candidate_bar.hpp"
#include "candidate_paint.hpp"
#include "ui_theme.hpp"
#include <commctrl.h>
#include <oleacc.h>
#include <algorithm>
#include <atomic>

namespace {
HINSTANCE Module() {
  HMODULE module = nullptr;
  static int marker;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&marker), &module);
  return module;
}
}  // namespace

// Simple MSAA children expose the complete candidate labels, selected state,
// bounds and actions without moving focus away from the editor. The standard
// Windows accessibility bridge can project these semantics into UI Automation.
class CandidateAccessible final : public IAccessible {
 public:
  explicit CandidateAccessible(CandidateBar* owner) : owner_(owner) {}
  void Detach() { owner_ = nullptr; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER; *out = nullptr;
    if (iid != IID_IUnknown && iid != IID_IDispatch && iid != IID_IAccessible) return E_NOINTERFACE;
    *out = static_cast<IAccessible*>(this); AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override { if (!n) return E_POINTER; *n=0; return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT,LCID,ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID,LPOLESTR*,UINT,LCID,DISPID*) override { return DISP_E_UNKNOWNNAME; }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID,REFIID,LCID,WORD,DISPPARAMS*,VARIANT*,EXCEPINFO*,UINT*) override { return DISP_E_MEMBERNOTFOUND; }
  HRESULT STDMETHODCALLTYPE get_accParent(IDispatch** out) override {
    if (!out) return E_POINTER; *out=nullptr;
    return owner_ ? AccessibleObjectFromWindow(owner_->hwnd_,OBJID_WINDOW,IID_IAccessible,reinterpret_cast<void**>(out)) : E_FAIL;
  }
  HRESULT STDMETHODCALLTYPE get_accChildCount(long* count) override {
    if (!count) return E_POINTER; *count=owner_ ? static_cast<long>(owner_->labels_.size()+1) : 0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child,IDispatch** out) override {
    if (!out) return E_POINTER; *out=nullptr; return Valid(child) ? S_FALSE : E_INVALIDARG;
  }
  HRESULT STDMETHODCALLTYPE get_accName(VARIANT child,BSTR* out) override {
    if (!Valid(child)) return E_INVALIDARG;
    const auto id=child.lVal;
    return String(out,id==0 ? L"拼音候选" : id==Count() ? L"设置" : owner_->labels_[id-1]);
  }
  HRESULT STDMETHODCALLTYPE get_accValue(VARIANT,BSTR* out) override { return Empty(out); }
  HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT child,BSTR* out) override {
    if (!Valid(child)) return E_INVALIDARG;
    return String(out,child.lVal==Count() ? L"打开输入法设置" : L"选择候选后写入当前文档");
  }
  HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child,VARIANT* out) override {
    if (!out) return E_POINTER; VariantInit(out); if (!Valid(child)) return E_INVALIDARG;
    out->vt=VT_I4; out->lVal=child.lVal==0 ? ROLE_SYSTEM_LIST : child.lVal==Count() ? ROLE_SYSTEM_PUSHBUTTON : ROLE_SYSTEM_LISTITEM;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accState(VARIANT child,VARIANT* out) override {
    if (!out) return E_POINTER; VariantInit(out); if (!Valid(child)) return E_INVALIDARG;
    out->vt=VT_I4; out->lVal=IsWindowVisible(owner_->hwnd_) ? 0 : STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN;
    if (child.lVal>0 && child.lVal<Count()) {
      out->lVal|=STATE_SYSTEM_SELECTABLE;
      if (child.lVal-1==owner_->view_.highlight) out->lVal|=STATE_SYSTEM_SELECTED;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT,BSTR* out) override { return Empty(out); }
  HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR* out,VARIANT,long* topic) override {
    if (topic) *topic=-1; return Empty(out);
  }
  HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT child,BSTR* out) override {
    if (!Valid(child)) return E_INVALIDARG;
    return child.lVal>0 && child.lVal<Count() ? String(out,std::to_wstring(child.lVal)) : Empty(out);
  }
  HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT* out) override {
    if (!out) return E_POINTER; VariantInit(out); return S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT* out) override {
    if (!out) return E_POINTER; VariantInit(out);
    if (!owner_ || owner_->view_.highlight<0 || owner_->view_.highlight>=Count()-1) return S_FALSE;
    out->vt=VT_I4; out->lVal=owner_->view_.highlight+1; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT child,BSTR* out) override {
    if (!Valid(child)) return E_INVALIDARG;
    return child.lVal==0 ? Empty(out) : String(out,child.lVal==Count() ? L"打开设置" : L"选择并写入");
  }
  HRESULT STDMETHODCALLTYPE accSelect(long,VARIANT) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE accLocation(long* x,long* y,long* width,long* height,VARIANT child) override {
    if (!x || !y || !width || !height) return E_POINTER;
    if (!Valid(child)) return E_INVALIDARG;
    RECT rect{}; GetWindowRect(owner_->hwnd_,&rect);
    if (child.lVal>0) {
      RECT local{}; GetClientRect(owner_->hwnd_,&local);
      if (child.lVal==Count()) local.left=local.right-UiPx(52,owner_->dpi_);
      else local=owner_->items_[child.lVal-1];
      POINT origin{local.left,local.top}; ClientToScreen(owner_->hwnd_,&origin);
      rect={origin.x,origin.y,origin.x+local.right-local.left,origin.y+local.bottom-local.top};
    }
    *x=rect.left; *y=rect.top; *width=rect.right-rect.left; *height=rect.bottom-rect.top; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accNavigate(long direction,VARIANT start,VARIANT* out) override {
    if (!out) return E_POINTER; VariantInit(out); if (!Valid(start)) return E_INVALIDARG;
    long child=0;
    if (start.lVal==0 && direction==NAVDIR_FIRSTCHILD) child=1;
    else if (start.lVal==0 && direction==NAVDIR_LASTCHILD) child=Count();
    else if (start.lVal>0 && direction==NAVDIR_NEXT) child=start.lVal+1;
    else if (start.lVal>0 && direction==NAVDIR_PREVIOUS) child=start.lVal-1;
    if (child<1 || child>Count()) return S_FALSE;
    out->vt=VT_I4; out->lVal=child; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accHitTest(long x,long y,VARIANT* out) override {
    if (!out) return E_POINTER; VariantInit(out); if (!owner_) return E_FAIL;
    POINT point{x,y}; ScreenToClient(owner_->hwnd_,&point);
    RECT client{}; GetClientRect(owner_->hwnd_,&client); if (!PtInRect(&client,point)) return S_FALSE;
    const auto hit=owner_->Hit(point.x,point.y);
    out->vt=VT_I4; out->lVal=hit==5 ? Count() : hit<0 ? 0 : hit+1; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT child) override {
    if (!Valid(child) || child.lVal==0 || !IsWindowVisible(owner_->hwnd_)) return E_INVALIDARG;
    if (child.lVal==Count()) {
      if (owner_->settings_handler_) owner_->settings_handler_(owner_->settings_user_);
    } else if (owner_->candidate_handler_) owner_->candidate_handler_(owner_->candidate_user_,child.lVal-1);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE put_accName(VARIANT,BSTR) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE put_accValue(VARIANT,BSTR) override { return E_NOTIMPL; }
 private:
  std::atomic<ULONG> refs_{1};
  CandidateBar* owner_;
  long Count() const { return owner_ ? static_cast<long>(owner_->labels_.size()+1) : 0; }
  bool Valid(VARIANT child) const { return owner_ && child.vt==VT_I4 && child.lVal>=0 && child.lVal<=Count(); }
  static HRESULT String(BSTR* out,const std::wstring& value) {
    if (!out) return E_POINTER; *out=SysAllocStringLen(value.data(),static_cast<UINT>(value.size()));
    return *out ? S_OK : E_OUTOFMEMORY;
  }
  static HRESULT Empty(BSTR* out) { if (!out) return E_POINTER; *out=nullptr; return S_FALSE; }
};

CandidateBar::~CandidateBar() {
  if (accessible_) { accessible_->Detach(); accessible_->Release(); accessible_=nullptr; }
  settings_handler_ = nullptr;
  candidate_handler_ = nullptr;
  if (tooltip_ != nullptr) DestroyWindow(tooltip_);
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
}

void CandidateBar::EnsureWindow() {
  if (hwnd_ != nullptr) return;
  UiDpiScope awareness;
  WNDCLASSW klass{};
  klass.style=CS_DROPSHADOW;
  klass.lpfnWndProc = Proc;
  klass.hInstance = Module();
  klass.lpszClassName = L"VoiceImeCandidates";
  klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&klass);
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, klass.lpszClassName,
      L"拼音候选", WS_POPUP, 0,0,200,36,nullptr,nullptr,klass.hInstance,this);
  if (!hwnd_) return;
  if (accessible_) { accessible_->Detach(); accessible_->Release(); }
  accessible_=new CandidateAccessible(this);
  dpi_ = UiDpi(hwnd_);
  INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);
  tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW,nullptr,
      WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,0,0,0,0,hwnd_,nullptr,klass.hInstance,nullptr);
  if (tooltip_) SendMessageW(tooltip_,TTM_SETMAXTIPWIDTH,0,UiPx(480,dpi_));
}

void CandidateBar::Show(Rect caret, Rect window, const RimeView& view, InputMode mode) {
  UiDpiScope coordinates;
  if (!skin_loaded_) SetSkin(UiCurrentSkin());
  const bool unchanged=view.composing && view_.composing && view.page==view_.page && view.highlight==view_.highlight &&
      mode==mode_ && SameRect(caret,caret_) && SameRect(window,window_) && hwnd_ && IsWindowVisible(hwnd_);
  view_ = view;
  mode_ = mode;
  caret_ = caret;
  window_ = window;
  if (!view.composing) { Hide(); return; }
  if (unchanged) return;
  EnsureWindow();
  if (!hwnd_) return;
  Layout();
}

void CandidateBar::Layout() {
  if (!hwnd_) return;
  UiDpiScope coordinates;
  for (std::size_t i=0;i<labels_.size();++i) {
    TOOLINFOW tool{}; tool.cbSize=sizeof(tool); tool.hwnd=hwnd_; tool.uId=i+1;
    if (tooltip_) SendMessageW(tooltip_,TTM_DELTOOLW,0,reinterpret_cast<LPARAM>(&tool));
  }
  labels_.clear(); items_.clear();
  HDC dc = GetDC(hwnd_);
  HFONT font = UiFont(16,dpi_);
  auto old = SelectObject(dc,font);
  const auto work = UiWorkArea(PlaceBox(caret_,window_,1,1,16));
  const int pad = UiPx(6,dpi_);
  const int settings = UiPx(52,dpi_);
  const int height = UiPx(40,dpi_);
  const int count = std::min(5,static_cast<int>(view_.page.size()));
  int remaining = std::max(count,std::min(work.w,UiPx(960,dpi_)) - 2*pad-settings);
  int x=pad;
  std::wstring accessible=L"拼音候选：";
  for (int i=0;i<count;++i) {
    labels_.push_back(std::to_wstring(i+1)+L" "+Utf8ToWide(view_.page[i]));
    RECT measure{};
    DrawTextW(dc,labels_.back().c_str(),-1,&measure,DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    const int width=std::min(static_cast<int>(measure.right)+UiPx(18,dpi_),remaining/(count-i));
    items_.push_back(RECT{x,pad,x+width,height-pad});
    x+=width;
    remaining-=width;
    accessible+=labels_.back()+L"；";
  }
  SelectObject(dc,old); DeleteObject(font); ReleaseDC(hwnd_,dc);
  const auto placed=PlacePopup(caret_,window_,work,x+pad+settings,height,UiPx(5,dpi_),UiPx(16,dpi_));
  UiPlaceWindow(hwnd_,placed,dpi_,UiPalette(skin_).radius);
  SetWindowTextW(hwnd_,(accessible+L"设置").c_str());
  NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,hwnd_,OBJID_WINDOW,CHILDID_SELF);
  NotifyWinEvent(EVENT_OBJECT_REORDER,hwnd_,OBJID_CLIENT,CHILDID_SELF);
  if (view_.highlight>=0 && view_.highlight<count) NotifyWinEvent(EVENT_OBJECT_SELECTION,hwnd_,OBJID_CLIENT,view_.highlight+1);
  for (std::size_t i=0;i<labels_.size();++i) {
    TOOLINFOW tool{}; tool.cbSize=sizeof(tool); tool.hwnd=hwnd_; tool.uId=i+1;
    tool.uFlags=TTF_SUBCLASS; tool.rect=items_[i]; tool.lpszText=labels_[i].data();
    if (tooltip_) SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
  }
  InvalidateRect(hwnd_,nullptr,FALSE);
}

void CandidateBar::Hide() {
  hover_=-1;
  if (tooltip_) SendMessageW(tooltip_,TTM_POP,0,0);
  if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_,SW_HIDE);
}

void CandidateBar::SetSkin(UiSkin skin) {
  skin_loaded_=true;
  if (skin_==skin) return;
  skin_=skin;
  if (!hwnd_) return;
  UiDpiScope coordinates;
  UiRoundWindow(hwnd_,dpi_,UiPalette(skin_).radius);
  InvalidateRect(hwnd_,nullptr,FALSE);
}

int CandidateBar::Hit(int x,int y) const {
  RECT client{}; GetClientRect(hwnd_,&client);
  if (HitSettings({0,0,client.right,client.bottom},x,y,UiPx(52,dpi_))) return 5;
  const POINT point{x,y};
  for (std::size_t i=0;i<items_.size();++i) if (PtInRect(&items_[i],point)) return static_cast<int>(i);
  return -1;
}

void CandidateBar::Paint() {
  UiDpiScope coordinates;
  UiPaintFrame frame(hwnd_);
  PaintCandidateStrip(frame.dc(),frame.rect(),skin_,dpi_,items_,view_.page,view_.highlight,hover_);
}

LRESULT CALLBACK CandidateBar::Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  auto* self=reinterpret_cast<CandidateBar*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if (message==WM_NCCREATE) {
    self=static_cast<CandidateBar*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    self->hwnd_=hwnd;
  }
  if (message==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (self) {
    if (message==UiSkinChangedMessage()) { self->SetSkin(UiCurrentSkin()); return 0; }
    if (message==WM_GETOBJECT && static_cast<LONG>(lp)==OBJID_CLIENT && self->accessible_)
      return LresultFromObject(IID_IAccessible,wp,self->accessible_);
    if (message==WM_PAINT) { self->Paint(); return 0; }
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_DPICHANGED) { self->dpi_=HIWORD(wp); self->Layout(); return 0; }
    if (message==WM_DISPLAYCHANGE) { if(IsWindowVisible(hwnd)) self->Layout(); return 0; }
    if (message==WM_SETTINGCHANGE || message==WM_THEMECHANGED) { InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    if (message==WM_MOUSEMOVE) {
      const int hit=self->Hit(static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp)));
      if (hit!=self->hover_) { self->hover_=hit; InvalidateRect(hwnd,nullptr,FALSE); }
      TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
      return 0;
    }
    if (message==WM_MOUSELEAVE) { self->hover_=-1; InvalidateRect(hwnd,nullptr,FALSE); return 0; }
    if (message==WM_LBUTTONUP) {
      const int hit=self->Hit(static_cast<short>(LOWORD(lp)),static_cast<short>(HIWORD(lp)));
      if (hit==5 && self->settings_handler_) self->settings_handler_(self->settings_user_);
      else if (hit>=0 && hit<5 && self->candidate_handler_) self->candidate_handler_(self->candidate_user_,hit);
      return 0;
    }
    if (message==WM_NCDESTROY) {
      if (self->accessible_) self->accessible_->Detach();
      self->hwnd_=nullptr;
    }
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}
