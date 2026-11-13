#pragma once

struct Hotkey {
  int modifiers;
  int vk;
};

inline Hotkey DefaultHotkey() { return Hotkey{1 | 2, 0x20}; }
