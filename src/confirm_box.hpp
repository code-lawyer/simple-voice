#pragma once

#include "place.hpp"
#include "session.hpp"
#include "settings.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <algorithm>

class ConfirmBox {
 public:
  using Hook = LRESULT (*)(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* self);

  ~ConfirmBox();
  bool Create(HINSTANCE instance, Hook hook, void* self);
  void ShowText(const std::string& utf8, Rect caret, Rect window_client,
                Phase phase = Phase::Pending, bool error = false, bool uncertain = false);
  void Hide();
  void SetSkin(UiSkin skin);
  void SetActivity(std::wstring activity);
  void SetRecordingProgress(int seconds,bool quiet);
  UiSkin skin() const { return skin_; }
  HWND hwnd() const { return hwnd_; }
  std::wstring StatusText() const;
  std::wstring HintText() const;
  int ScrollPosition() const { return scroll_; }
  int ScrollLimit() const { return std::max(0, content_height_ - viewport_height_); }

 private:
  HWND hwnd_ = nullptr;
  Hook hook_ = nullptr;
  void* hook_self_ = nullptr;
  std::wstring text_;
  std::wstring activity_;
  Phase phase_ = Phase::Pending;
  int recording_seconds_ = 0;
  bool quiet_capture_ = false;
  bool error_ = false;
  bool uncertain_ = false;
  UINT dpi_ = 96;
  Rect caret_{};
  Rect window_{};
  int scroll_ = 0;
  int content_height_ = 0;
  int viewport_height_ = 0;
  int footer_height_ = 34;
  int line_height_ = 1;
  bool dragging_scroll_ = false;
  int drag_y_ = 0;
  int drag_scroll_ = 0;
  std::wstring accessible_text_;
  UiSkin skin_ = UiSkin::Titanium;
  bool skin_loaded_ = false;

  void Paint();
  void Layout();
  void Scroll(int position);
  RECT ScrollThumb() const;
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};
