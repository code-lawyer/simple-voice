#pragma once

#include "composer.hpp"
#include "place.hpp"
#include "settings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class CandidateAccessible;

class CandidateBar {
 public:
  CandidateBar() = default;
  CandidateBar(const CandidateBar&) = delete;
  CandidateBar& operator=(const CandidateBar&) = delete;
  ~CandidateBar();

  void Show(Rect caret_screen, Rect window_client_screen, const RimeView& view, InputMode mode);
  void Hide();
  void SetSkin(UiSkin skin);
  UiSkin skin() const { return skin_; }
  HWND hwnd() const { return hwnd_; }
  void SetSettingsHandler(void (*handler)(void*), void* user) {
    settings_handler_ = handler;
    settings_user_ = user;
  }
  void SetCandidateHandler(void (*handler)(void*, int), void* user) {
    candidate_handler_ = handler;
    candidate_user_ = user;
  }

 private:
  friend class CandidateAccessible;
  CandidateAccessible* accessible_ = nullptr;
  HWND hwnd_ = nullptr;
  RimeView view_{};
  InputMode mode_ = InputMode::Chinese;
  void (*settings_handler_)(void*) = nullptr;
  void* settings_user_ = nullptr;
  void (*candidate_handler_)(void*, int) = nullptr;
  void* candidate_user_ = nullptr;
  HWND tooltip_ = nullptr;
  std::vector<RECT> items_;
  std::vector<std::wstring> labels_;
  Rect caret_{};
  Rect window_{};
  UINT dpi_ = 96;
  int hover_ = -1;
  UiSkin skin_ = UiSkin::Titanium;
  bool skin_loaded_ = false;

  void EnsureWindow();
  void Paint();
  void Layout();
  int Hit(int x, int y) const;
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

// Width is shared by layout and hit testing, and scales with the control DPI.
inline bool HitSettings(Rect bar, int x, int y, int width = 52) {
  if (bar.w < width) return false;
  const int left = bar.x + bar.w - width;
  return x >= left && x < bar.x + bar.w && y >= bar.y && y < bar.y + bar.h;
}
