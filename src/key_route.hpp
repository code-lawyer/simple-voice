#pragma once

#include "hotkey.hpp"

enum class InputMode { Chinese, English };

enum class KeyAction {
  Pass,
  ToRime,
  ToggleMode,
  OpenVoice,
  VoiceEnter,
  VoiceEscape,
  Consume,
  PassAfterClear
};

struct KeyState {
  InputMode mode;
  bool accepts;
  bool voice_active;
  bool composing;
  int candidate_count;
  bool shift;
  bool ctrl;
  bool alt;
  bool caps;
  bool repeat;
  int vk;
  bool win = false;
  bool ascii_punctuation = false;
};

struct KeyDecision {
  KeyAction action;
};

KeyDecision RouteKey(const KeyState& state, Hotkey hotkey);
