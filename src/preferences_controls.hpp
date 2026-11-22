#pragma once
#include <windows.h>

namespace preference {
enum Id {
  Navigation=10, StatusLine=11, BrandName=12, BrandEnglish=13, BrandMark=14,
  Version=15, CompactNavigation=16, PageTitle=17, Content=18,
  Learning=20, HotkeyButton=21, Skin=22, SaveOptions=23,
  Query=30, Terms=31, Term=32, Code=33, AutoCode=34, SaveTerm=35, NewTerm=36,
  DeleteTerm=37, Undo=38, Previous=39, Next=40, Count=41,
  Devices=50, RefreshDevices=51, Retry=53, Diagnostics=54, CopyDiagnostics=55,
  Guide=56, ChineseOnly=57, ModelChoice=60, ModelDescription=61, ModelState=62,
  ModelUse=63, ModelLocal=65, ModelDefault=66, ModelCancel=67, ModelIdle=68,
  ModelHints=69, DataBackup=70, DataRestore=71, PracticeToggle=72,
  PracticeConfirm=73, PracticeCancel=74, PracticeText=75,
  SkinPreview=80, HotkeyValue=81, TermsEmpty=82, ModelPreload=83
};

// Navigation and feedback live in the frame. The form scrolls in its own child window.
inline HWND Field(HWND window, int id) {
  if (HWND frame_control=GetDlgItem(window,id)) return frame_control;
  return GetDlgItem(GetDlgItem(window,Content),id);
}
}
