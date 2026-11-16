#pragma once
#include <msctf.h>
#include <ctfutb.h>

// MinGW's ctfutb.h omits the button interface and these SDK constants.
// ABI source: Microsoft win32metadata, um/ctfutb.h. These are Windows IDs.
#ifndef __ITfLangBarItemButton_INTERFACE_DEFINED__
#define __ITfLangBarItemButton_INTERFACE_DEFINED__
struct ITfMenu;
enum TfLBIClick { TF_LBI_CLK_RIGHT=1, TF_LBI_CLK_LEFT=2 };
MIDL_INTERFACE("28c7f1d0-de25-11d2-afdd-00105a2799b5")
ITfLangBarItemButton : public ITfLangBarItem {
  virtual HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick,POINT,const RECT*)=0;
  virtual HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu*)=0;
  virtual HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT)=0;
  virtual HRESULT STDMETHODCALLTYPE GetIcon(HICON*)=0;
  virtual HRESULT STDMETHODCALLTYPE GetText(BSTR*)=0;
};
#endif
#ifndef TF_LBI_ICON
#define TF_LBI_ICON 0x00000001
#define TF_LBI_TEXT 0x00000002
#define TF_LBI_TOOLTIP 0x00000004
#define TF_LBI_STATUS 0x00010000
#define TF_LBI_STYLE_SHOWNINTRAY 0x00000002
#define TF_LBI_STYLE_TEXTCOLORICON 0x00000020
#define TF_LBI_STYLE_BTN_BUTTON 0x00010000
#define TF_LBI_STATUS_DISABLED 0x00000002
#endif
inline constexpr IID kLangBarButtonIid={0x28c7f1d0,0xde25,0x11d2,{0xaf,0xdd,0x00,0x10,0x5a,0x27,0x99,0xb5}};
inline constexpr GUID kSystemInputModeItem={0x2c77a81e,0x41cc,0x4178,{0xa3,0xa7,0x5f,0x8a,0x98,0x75,0x68,0xe6}};
