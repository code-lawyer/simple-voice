#pragma once
#include "settings.hpp"
#include "ui_theme.hpp"
#include <string>

std::wstring HotkeyLabel(Hotkey hotkey);
enum class HotkeyAvailability { Available, Conflict, Unavailable };
HotkeyAvailability ProbeHotkey(Hotkey key);

class HotkeyDialog {
 public:
  using Save = bool (*)(void*, Hotkey);
  using Probe = HotkeyAvailability (*)(Hotkey);
  ~HotkeyDialog();
  void Open(HINSTANCE instance, Hotkey current, Save save, void* owner, Probe probe = ProbeHotkey);
  void Close();
  bool Translate(MSG* message);
  HWND hwnd() const { return hwnd_; }
 private:
  HWND hwnd_ = nullptr;
  HWND current_ = nullptr;
  HWND instruction_ = nullptr;
  HWND status_ = nullptr;
  HWND close_ = nullptr;
  HFONT font_ = nullptr;
  UINT dpi_ = 96;
  Save save_ = nullptr;
  Probe probe_ = nullptr;
  void* owner_ = nullptr;
  Hotkey hotkey_{};
  bool saved_ = false;
  void Layout();
  void Record(int vk);
  static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp);
};
