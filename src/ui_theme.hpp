#pragma once
#include "place.hpp"
#include "settings.hpp"
#include <algorithm>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

inline int UiPx(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }
inline UINT UiDpi(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  const UINT dpi = fn != nullptr ? fn(hwnd) : 96;
  return dpi != 0 ? dpi : 96;
}
class UiDpiScope {
 public:
  UiDpiScope() {
    fn_ = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    if (fn_ != nullptr) old_ = fn_(reinterpret_cast<HANDLE>(-4));
  }
  ~UiDpiScope() { if (fn_ != nullptr && old_ != nullptr) fn_(old_); }
 private:
  using Fn = HANDLE(WINAPI*)(HANDLE);
  Fn fn_ = nullptr;
  HANDLE old_ = nullptr;
};
inline HFONT UiFont(int dip, UINT dpi, bool bold = false) {
  return CreateFontW(-UiPx(dip, dpi), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
      FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}
inline Rect UiWorkArea(Rect rect) {
  RECT bounds{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &info)) {
    return {info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left, info.rcWork.bottom - info.rcWork.top};
  }
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &bounds, 0);
  return {bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
}
struct UiColors {
  COLORREF background = RGB(32,32,32);
  COLORREF text = RGB(245,245,245);
  COLORREF secondary = RGB(196,207,221);
  COLORREF selected = RGB(47,79,129);
  COLORREF selected_text = RGB(255,255,255);
  COLORREF hover = RGB(56,56,56);
  COLORREF border = RGB(82,82,82);
  COLORREF accent = RGB(22,104,121);
  COLORREF rule = RGB(205,215,222);
  COLORREF icon_background = RGB(212,226,232);
  int radius = 10;
  bool outlined = true;
  bool high_contrast = false;
};
inline UINT UiSkinChangedMessage() {
  static const UINT message=RegisterWindowMessageW(L"VoiceIme.AppearanceChanged");
  return message;
}
inline HMENU UiSkinMenu(UiSkin current, UINT first_command) {
  HMENU menu=CreatePopupMenu();
  if (!menu) return nullptr;
  for (auto skin : {UiSkin::Titanium,UiSkin::Daylight,UiSkin::DeepSpace})
    AppendMenuW(menu,MF_STRING,first_command+static_cast<UINT>(skin),SkinLabel(skin));
  CheckMenuRadioItem(menu,first_command,first_command+2,first_command+static_cast<UINT>(current),MF_BYCOMMAND);
  return menu;
}
inline UiSkin UiCurrentSkin() {
#ifdef VOICEIME_UI_TESTING
  // Fixture selection never reads or writes the user's appearance preference.
  wchar_t fixture[32]{};
  GetEnvironmentVariableW(L"VOICEIME_TEST_SKIN",fixture,32);
  if (lstrcmpW(fixture,L"daylight")==0) return UiSkin::Daylight;
  if (lstrcmpW(fixture,L"deep-space")==0) return UiSkin::DeepSpace;
  GetEnvironmentVariableW(L"VOICEIME_TEST_THEME",fixture,32);
  if (lstrcmpW(fixture,L"light")==0) return UiSkin::Daylight;
  if (lstrcmpW(fixture,L"dark")==0) return UiSkin::DeepSpace;
  return UiSkin::Titanium;
#else
  wchar_t directory[32768]{};
#ifdef VOICEIME_TESTING
  const auto count=GetEnvironmentVariableW(L"VOICEIME_TEST_DATA",directory,32768);
  return count && count<32768 ? LoadSkin(std::wstring(directory,count)+L"\\appearance.json") : UiSkin::Titanium;
#else
  const auto count=GetEnvironmentVariableW(L"LOCALAPPDATA",directory,32768);
  return count && count<32768 ? LoadSkin(std::wstring(directory,count)+L"\\VoiceIme\\appearance.json") : UiSkin::Titanium;
#endif
#endif
}
inline UiColors UiPalette(UiSkin skin = UiSkin::Titanium) {
  UiColors colors;
  if (skin==UiSkin::Daylight) {
    colors.background=RGB(255,255,255); colors.text=RGB(32,46,60);
    colors.secondary=RGB(92,106,120); colors.selected=RGB(229,237,254);
    colors.selected_text=RGB(35,78,155); colors.hover=RGB(237,242,254);
    colors.border=RGB(224,230,237); colors.rule=colors.border;
    colors.accent=RGB(42,98,206); colors.icon_background=RGB(237,242,254);
    colors.radius=14; colors.outlined=false;
  } else if (skin==UiSkin::DeepSpace) {
    colors.background=RGB(16,27,35); colors.text=RGB(237,244,247);
    colors.secondary=RGB(155,175,185); colors.selected=RGB(38,61,73);
    colors.selected_text=RGB(196,238,243); colors.hover=RGB(32,49,60);
    colors.border=RGB(58,79,91); colors.rule=RGB(41,60,71);
    colors.accent=RGB(146,208,219); colors.icon_background=colors.selected;
    colors.radius=12;
  } else {
    colors.background=RGB(240,243,245); colors.text=RGB(34,51,61);
    colors.secondary=RGB(83,105,118); colors.selected=RGB(212,226,232);
    colors.selected_text=RGB(22,78,96); colors.hover=RGB(227,234,239);
    colors.border=RGB(184,198,207);
  }
  HIGHCONTRASTW contrast{};
  contrast.cbSize = sizeof(contrast);
  bool high_contrast=SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
      (contrast.dwFlags & HCF_HIGHCONTRASTON);
#ifdef VOICEIME_UI_TESTING
  wchar_t contrast_fixture[4]{};
  GetEnvironmentVariableW(L"VOICEIME_TEST_HIGH_CONTRAST",contrast_fixture,4);
  high_contrast=high_contrast || lstrcmpW(contrast_fixture,L"1")==0;
#endif
  if (high_contrast) {
    colors.high_contrast=true;
    colors.background = GetSysColor(COLOR_WINDOW);
    colors.text = colors.secondary = GetSysColor(COLOR_WINDOWTEXT);
    colors.selected = colors.hover = GetSysColor(COLOR_HIGHLIGHT);
    colors.selected_text = GetSysColor(COLOR_HIGHLIGHTTEXT);
    colors.border = GetSysColor(COLOR_WINDOWTEXT);
    colors.rule = colors.accent = colors.secondary;
    colors.icon_background = colors.background;
    colors.outlined = true;
  }
  return colors;
}
inline void UiFill(HDC dc, const RECT& rect, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &rect, brush);
  DeleteObject(brush);
}
inline void UiSurface(HDC dc, const RECT& rect, const UiColors& colors, UINT dpi) {
  UiFill(dc,rect,colors.background);
  if (!colors.outlined) return;
  HPEN pen=CreatePen(PS_SOLID,std::max(1,UiPx(1,dpi)),colors.border);
  auto old_pen=SelectObject(dc,pen); auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
  RoundRect(dc,rect.left,rect.top,rect.right-1,rect.bottom-1,UiPx(colors.radius*2,dpi),UiPx(colors.radius*2,dpi));
  SelectObject(dc,old_brush); SelectObject(dc,old_pen); DeleteObject(pen);
}

// One complete offscreen frame prevents a background-only flash.
class UiPaintFrame {
 public:
  explicit UiPaintFrame(HWND hwnd) : hwnd_(hwnd) {
    target_=BeginPaint(hwnd_,&paint_); GetClientRect(hwnd_,&rect_);
    buffer_=CreateCompatibleDC(target_);
    bitmap_=CreateCompatibleBitmap(target_,std::max(1L,rect_.right),std::max(1L,rect_.bottom));
    if (buffer_ && bitmap_) old_=SelectObject(buffer_,bitmap_);
  }
  ~UiPaintFrame() {
    if (old_) { BitBlt(target_,0,0,rect_.right,rect_.bottom,buffer_,0,0,SRCCOPY); SelectObject(buffer_,old_); }
    if (bitmap_) DeleteObject(bitmap_);
    if (buffer_) DeleteDC(buffer_);
    EndPaint(hwnd_,&paint_);
  }
  HDC dc() const { return old_ ? buffer_ : target_; }
  RECT rect() const { return rect_; }
 private:
  HWND hwnd_; PAINTSTRUCT paint_{}; RECT rect_{};
  HDC target_=nullptr,buffer_=nullptr; HBITMAP bitmap_=nullptr; HGDIOBJ old_=nullptr;
};
inline void UiRoundWindow(HWND hwnd, UINT dpi, int radius) {
  RECT rect{}; GetWindowRect(hwnd,&rect);
  const int diameter=UiPx(radius*2,dpi);
  HRGN region=CreateRoundRectRgn(0,0,rect.right-rect.left+1,rect.bottom-rect.top+1,diameter,diameter);
  if (region && !SetWindowRgn(hwnd,region,TRUE)) DeleteObject(region);
}
inline bool UiPlaceWindow(HWND hwnd, Rect placed, UINT dpi, int radius) {
  RECT current{}; GetWindowRect(hwnd,&current);
  const bool moved=current.left!=placed.x || current.top!=placed.y;
  const bool sized=current.right-current.left!=placed.w || current.bottom-current.top!=placed.h;
  const bool hidden=!IsWindowVisible(hwnd);
  if (moved || sized || hidden) {
    UINT flags=SWP_NOACTIVATE|SWP_NOZORDER;
    if (!moved) flags|=SWP_NOMOVE;
    if (!sized) flags|=SWP_NOSIZE;
    if (hidden) flags|=SWP_SHOWWINDOW;
    SetWindowPos(hwnd,nullptr,placed.x,placed.y,placed.w,placed.h,flags);
    if (sized || hidden) UiRoundWindow(hwnd,dpi,radius);
  }
  return sized || hidden;
}
