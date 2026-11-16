#pragma once
#include "brand_resources.h"
#include "ui_theme.hpp"

// All returned icons are owned; DestroyIcon after the consumer has copied them.
inline HICON LoadProductIcon(HINSTANCE module, int resource, int pixels) {
  return static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(resource), IMAGE_ICON,
                                     pixels, pixels, LR_DEFAULTCOLOR));
}
inline int ShellIconPixels() {
  // GetDpiForWindow avoids inheriting a legacy host's virtualized screen DPI.
  const HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
  return std::max(16,UiPx(16,taskbar ? UiDpi(taskbar) : 96));
}
inline bool ShellUsesLightIcons() {
  HIGHCONTRASTW contrast{sizeof(HIGHCONTRASTW),0,nullptr};
  if(SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(contrast),&contrast,0) && (contrast.dwFlags&HCF_HIGHCONTRASTON)) {
    const COLORREF ink=GetSysColor(COLOR_WINDOWTEXT);
    return 299*GetRValue(ink)+587*GetGValue(ink)+114*GetBValue(ink)>128000;
  }
  DWORD light=0,bytes=sizeof(light);
  RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&bytes);
  return light==0;
}
