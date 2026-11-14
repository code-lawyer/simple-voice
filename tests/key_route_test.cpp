#include "harness.hpp"
#include "key_route.hpp"

void ChineseLetterGoesToRime() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
}
void ChordPassesToTheDocument() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.ctrl = true;
  s.vk = 0x43;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.ctrl = false;
  s.alt = true;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void DisabledDocumentPassesEveryKey() {
  KeyState s{};
  s.mode = InputMode::Chinese;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void DigitPastThePagePasses() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.candidate_count = 2;
  s.vk = 0x33;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.candidate_count = 5;
  s.vk = 0x36;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.vk = 0x65;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
}
void ShiftLetterDoesNotToggle() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.shift = true;
  s.vk = 0x42;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::PassAfterClear);
  s.shift = false;
  s.caps = true;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::PassAfterClear);
}
void BareShiftToggles() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::English;
  s.vk = 0xA0;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToggleMode);
}
void VoiceHotkeyAndItsRepeat() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::OpenVoice);
  s.repeat = true;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Consume);
}
void VoiceSessionStealsEnterOnly() {
  KeyState s{};
  s.accepts = true;
  s.voice_active = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.vk = 0x0D;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::VoiceEnter);
  s.vk = 0x1B;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::VoiceEscape);
}
void IdleEnterPasses() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0x0D;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void ChinesePeriodGoesToRime() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0xBE;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
  s.mode = InputMode::English;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void PageKeyOnlyWhileComposing() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0xBD;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.composing = true;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
}
void VoiceHotkeyStillOpensWhileVoiceIsActive() {
  KeyState s{};
  s.accepts = true;
  s.voice_active = true;
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::OpenVoice);
}
