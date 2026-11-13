#pragma once

struct Hotkey {
  int modifiers;
  int vk;
};

inline Hotkey DefaultHotkey() { return Hotkey{1 | 2, 0x20}; }

inline bool IsKeyRepeat(unsigned int lparam_bits) { return (lparam_bits & 0x40000000u) != 0; }
