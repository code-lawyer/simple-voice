#pragma once

#include "place.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

class ConfirmBox {
 public:
  using Hook = LRESULT (*)(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, void* self);

  bool Create(HINSTANCE instance, Hook hook, void* self);
  void ShowText(const std::string& utf8, Rect caret, Rect window_client);
  void Hide();
  HWND hwnd() const { return hwnd_; }

 private:
  HWND hwnd_ = nullptr;
  Hook hook_ = nullptr;
  void* hook_self_ = nullptr;

  void Paint();
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
};
