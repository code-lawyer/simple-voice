#include "hotkey_dialog.hpp"
#include <algorithm>

HotkeyAvailability ProbeHotkey(Hotkey key) {
  if (!HotkeyAccepted(key)) return HotkeyAvailability::Conflict;
  // Use a disposable window so registration can never outlive the probe,
  // including an unexpected UnregisterHotKey failure. It never takes focus.
  HWND probe = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!probe) return HotkeyAvailability::Unavailable;
  UINT modifiers = MOD_NOREPEAT;
  if (key.modifiers & 1) modifiers |= MOD_CONTROL;
  if (key.modifiers & 2) modifiers |= MOD_ALT;
  if (key.modifiers & 4) modifiers |= MOD_SHIFT;
  const BOOL registered = RegisterHotKey(probe, 1, modifiers, key.vk);
  const DWORD error = registered ? ERROR_SUCCESS : GetLastError();
  const BOOL released = registered ? UnregisterHotKey(probe, 1) : TRUE;
  DestroyWindow(probe);
  if (registered && released) return HotkeyAvailability::Available;
  return error == ERROR_HOTKEY_ALREADY_REGISTERED ? HotkeyAvailability::Conflict : HotkeyAvailability::Unavailable;
}

std::wstring HotkeyLabel(Hotkey hotkey) {
  std::wstring label;
  if (hotkey.modifiers & 1) label += L"Ctrl + ";
  if (hotkey.modifiers & 2) label += L"Alt + ";
  if (hotkey.modifiers & 4) label += L"Shift + ";
  if (hotkey.vk == VK_SPACE) return label + L"Space";
  wchar_t name[80]{};
  const UINT scan = MapVirtualKeyW(hotkey.vk, MAPVK_VK_TO_VSC);
  if (GetKeyNameTextW(static_cast<LONG>(scan << 16),name,80)) return label + name;
  return label + std::to_wstring(hotkey.vk);
}

HotkeyDialog::~HotkeyDialog() { Close(); }
void HotkeyDialog::Close() {
  if (hwnd_) DestroyWindow(hwnd_);
  hwnd_ = nullptr;
  if (font_) DeleteObject(font_);
  font_ = nullptr;
}

void HotkeyDialog::Open(HINSTANCE instance, Hotkey current, Save save, void* owner, Probe probe) {
  if (hwnd_) { SetForegroundWindow(hwnd_); return; }
  UiDpiScope awareness;
  save_ = save;
  probe_ = probe;
  owner_ = owner;
  hotkey_ = current;
  saved_ = false;
  WNDCLASSW klass{};
  klass.lpfnWndProc = Proc;
  klass.hInstance = instance;
  klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  klass.hCursor = LoadCursor(nullptr,IDC_ARROW);
  klass.lpszClassName = L"VoiceImeHotkey";
  RegisterClassW(&klass);
  POINT point{}; GetCursorPos(&point);
  const auto work = UiWorkArea({point.x,point.y,1,1});
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
      klass.lpszClassName,L"语音快捷键",WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
      work.x+(work.w-460)/2,work.y+(work.h-240)/2,460,240,nullptr,nullptr,instance,this);
  if (!hwnd_) return;
  dpi_ = UiDpi(hwnd_);
  auto label = [&](const wchar_t* text, int id) {
    return CreateWindowExW(0,L"STATIC",text,WS_CHILD | WS_VISIBLE | SS_LEFT,
        0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
  };
  current_ = label((L"当前快捷键："+HotkeyLabel(current)).c_str(),10);
  instruction_ = label(L"按下新的组合键，或使用功能键。",11);
  status_ = label(L"Esc 取消，原快捷键会保留。",12);
  close_ = CreateWindowExW(0,L"BUTTON",L"取消",WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
      0,0,0,0,hwnd_,reinterpret_cast<HMENU>(IDCANCEL),instance,nullptr);
  Layout();
  ShowWindow(hwnd_,SW_SHOWNORMAL);
  SetForegroundWindow(hwnd_);
  SetFocus(hwnd_);
}

void HotkeyDialog::Layout() {
  if (!hwnd_) return;
  if (font_) DeleteObject(font_);
  font_ = UiFont(15,dpi_);
  for (HWND child : {current_,instruction_,status_,close_}) if (child) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
  RECT desired{0,0,UiPx(440,dpi_),UiPx(210,dpi_)};
  AdjustWindowRectEx(&desired,static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_STYLE)),FALSE,
      static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_EXSTYLE)));
  RECT rect{}; GetWindowRect(hwnd_,&rect);
  const auto placed=ClampRect({rect.left,rect.top,desired.right-desired.left,desired.bottom-desired.top},UiWorkArea({rect.left,rect.top,1,1}));
  SetWindowPos(hwnd_,nullptr,placed.x,placed.y,placed.w,placed.h,SWP_NOZORDER | SWP_NOACTIVATE);
  const int pad=UiPx(22,dpi_);
  RECT client{}; GetClientRect(hwnd_,&client);
  MoveWindow(current_,pad,UiPx(20,dpi_),client.right-2*pad,UiPx(28,dpi_),TRUE);
  MoveWindow(instruction_,pad,UiPx(57,dpi_),client.right-2*pad,UiPx(28,dpi_),TRUE);
  MoveWindow(status_,pad,UiPx(96,dpi_),client.right-2*pad,UiPx(53,dpi_),TRUE);
  MoveWindow(close_,client.right-pad-UiPx(86,dpi_),client.bottom-pad-UiPx(32,dpi_),UiPx(86,dpi_),UiPx(32,dpi_),TRUE);
}

void HotkeyDialog::Record(int vk) {
  if (vk==VK_SHIFT || vk==VK_CONTROL || vk==VK_MENU || (vk>=VK_LSHIFT && vk<=VK_RMENU)) return;
  int modifiers=0;
  if (GetKeyState(VK_CONTROL)&0x8000) modifiers|=1;
  if (GetKeyState(VK_MENU)&0x8000) modifiers|=2;
  if (GetKeyState(VK_SHIFT)&0x8000) modifiers|=4;
  const Hotkey next{modifiers,vk};
  const bool windows = (GetKeyState(VK_LWIN)&0x8000) || (GetKeyState(VK_RWIN)&0x8000);
  if (windows || !HotkeyAccepted(next)) {
    const bool plain = !windows && modifiers == 0 && vk != VK_F12 && vk != VK_LWIN && vk != VK_RWIN && vk != VK_SNAPSHOT;
    SetWindowTextW(status_, plain ? L"请同时按住 Ctrl、Alt 或 Shift，或改用功能键。" :
        L"这个组合由系统保留，请换一个组合键。");
    return;
  }
  const auto available = probe_ ? probe_(next) : HotkeyAvailability::Unavailable;
  if (available != HotkeyAvailability::Available) {
    SetWindowTextW(status_, available == HotkeyAvailability::Conflict ?
        L"这个组合已被系统或其他程序占用。请换一个，当前快捷键不变。" :
        L"暂时无法检查这个组合是否可用。请稍后重试，当前快捷键不变。");
    return;
  }
  if (!save_ || !save_(owner_,next)) {
    SetWindowTextW(status_,L"未能保存，仍使用当前快捷键。请稍后重试。");
    return;
  }
  hotkey_=next;
  saved_=true;
  SetWindowTextW(current_,(L"当前快捷键："+HotkeyLabel(next)).c_str());
  SetWindowTextW(status_,(L"已保存："+HotkeyLabel(next)+L"。请在常用应用中试用新快捷键。").c_str());
  SetWindowTextW(close_,L"完成");
}

bool HotkeyDialog::Translate(MSG* message) {
  if (!hwnd_ || (message->hwnd!=hwnd_ && !IsChild(hwnd_,message->hwnd))) return false;
  if (message->message==WM_KEYDOWN || message->message==WM_SYSKEYDOWN) {
    if (message->wParam==VK_ESCAPE) { Close(); return true; }
    if (message->wParam==VK_TAB && !(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000)) {
      return IsDialogMessageW(hwnd_,message) != FALSE;
    }
    if (message->wParam==VK_RETURN && (saved_ || message->hwnd==close_)) { Close(); return true; }
    // Buttons keep native Space keydown/keyup behavior; focused buttons are
    // not a recording surface. The message loop dispatches these normally.
    if (message->hwnd==close_) return false;
    Record(static_cast<int>(message->wParam));
    return true;
  }
  return false;
}

LRESULT CALLBACK HotkeyDialog::Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  auto* self=reinterpret_cast<HotkeyDialog*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if (message==WM_NCCREATE) {
    self=static_cast<HotkeyDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    self->hwnd_=hwnd;
  }
  if (self) {
    if (message==WM_CTLCOLORSTATIC) {
      const auto dc=reinterpret_cast<HDC>(wp);
      SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
      SetBkColor(dc,GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    if (message==WM_DPICHANGED) { self->dpi_=HIWORD(wp); self->Layout(); return 0; }
    if (message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wp)==IDCANCEL)) { self->Close(); return 0; }
    if (message==WM_KEYDOWN || message==WM_SYSKEYDOWN) {
      MSG key{hwnd,message,wp,lp};
      if (self->Translate(&key)) return 0;
    }
    if (message==WM_NCDESTROY) self->hwnd_=nullptr;
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}
