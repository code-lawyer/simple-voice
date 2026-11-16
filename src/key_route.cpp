#include "key_route.hpp"

namespace {

int PressedModifiers(const KeyState& state) {
  int modifiers = 0;
  if (state.ctrl) modifiers |= 1;
  if (state.alt) modifiers |= 2;
  if (state.shift) modifiers |= 4;
  return modifiers;
}

// Minus and equals are inside 0xBA-0xC0, but they only page while composing.
bool IsPunctuation(int vk) {
  if (vk == 0xBB || vk == 0xBD) return false;
  return (vk >= 0xBA && vk <= 0xC0) || (vk >= 0xDB && vk <= 0xDF);
}

}  // namespace

KeyDecision RouteKey(const KeyState& state, Hotkey hotkey) {
  if (!state.accepts || state.win) return KeyDecision{KeyAction::Pass};

  if (PressedModifiers(state) == hotkey.modifiers && state.vk == hotkey.vk) {
    return KeyDecision{state.repeat ? KeyAction::Consume : KeyAction::OpenVoice};
  }

  if (state.voice_active) {
    if (state.vk == 0x0D) return KeyDecision{KeyAction::VoiceEnter};
    if (state.vk == 0x1B) return KeyDecision{KeyAction::VoiceEscape};
    return KeyDecision{KeyAction::Pass};
  }

  const bool plain = !state.ctrl && !state.alt;
  if ((state.vk == 0x10 || state.vk == 0xA0 || state.vk == 0xA1) && plain) {
    return KeyDecision{KeyAction::ToggleMode};
  }

  const bool letter = state.vk >= 0x41 && state.vk <= 0x5A;
  if (state.mode == InputMode::English) return KeyDecision{KeyAction::Pass};

  if (state.composing && state.ctrl && !state.alt && !state.shift && state.vk == 0x2E)
    return KeyDecision{KeyAction::ToRime};

  if (state.mode == InputMode::Chinese && plain) {
    if (letter) return KeyDecision{KeyAction::ToRime};
    if (state.composing) {
      if (state.vk == 0x08 || state.vk == 0x20 || state.vk == 0x0D || state.vk == 0x1B ||
          state.vk == 0x2E || (state.vk >= 0x23 && state.vk <= 0x28)) {
        return KeyDecision{KeyAction::ToRime};
      }
      if (state.vk == 0x21 || state.vk == 0x22 || state.vk == 0xBD || state.vk == 0xBB) {
        return KeyDecision{KeyAction::ToRime};
      }
      int index = 0;
      // Shift+digit is punctuation (!, （, ）), not a candidate number.
      if (!state.shift && state.vk >= 0x31 && state.vk <= 0x35) index = state.vk - 0x30;
      else if (state.vk >= 0x61 && state.vk <= 0x65) index = state.vk - 0x60;
      if (index >= 1 && index <= 5) {
        return KeyDecision{index <= state.candidate_count ? KeyAction::ToRime : KeyAction::Pass};
      }
    }
    if (IsPunctuation(state.vk)) return KeyDecision{KeyAction::ToRime};
    if (state.shift && state.vk >= 0x30 && state.vk <= 0x39) return KeyDecision{KeyAction::ToRime};
  }

  return KeyDecision{KeyAction::Pass};
}
