#pragma once

#include "composer.hpp"
#include "place.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class CandidateBar {
 public:
  CandidateBar() = default;
  CandidateBar(const CandidateBar&) = delete;
  CandidateBar& operator=(const CandidateBar&) = delete;
  ~CandidateBar();

  void Show(Rect caret_screen, Rect window_client_screen, const RimeView& view, InputMode mode);
  void Hide();
  HWND hwnd() const { return hwnd_; }

 private:
  HWND hwnd_ = nullptr;
  RimeView view_{};
  InputMode mode_ = InputMode::Chinese;

  void EnsureWindow();
  void Paint();
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};

// Right 36 pixels, full height. A bar narrower than 36 pixels has no button.
inline bool HitSettings(Rect bar, int x, int y) {
  if (bar.w < 36) return false;
  const int left = bar.x + bar.w - 36;
  return x >= left && x < bar.x + bar.w && y >= bar.y && y < bar.y + bar.h;
}
