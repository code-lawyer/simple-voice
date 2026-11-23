#pragma once

#include "key_route.hpp"

#include <string>
#include <vector>

struct RimeView {
  bool composing = false;
  std::string preedit;
  std::vector<std::string> page;
  int highlight = 0;
  std::string commit;
  int caret = 0;  // UTF-8 byte offset into preedit, as supplied by librime.
  bool ascii_punctuation = false;
  std::string raw_input;  // Original spelling before Rime inserts spaces or formats vowels.
};

class Composer {
 public:
  virtual ~Composer() = default;
  virtual RimeView ProcessKey(int vk, bool shift) = 0;
  virtual RimeView ProcessKeyWithModifiers(int vk, bool shift, bool ctrl, bool alt) {
    return ProcessKey(vk, shift);
  }
  virtual void Clear() = 0;
  virtual const RimeView& view() const = 0;
};

class PinyinController {
 public:
  explicit PinyinController(Composer* composer) : composer_(composer) {}
  InputMode mode() const { return mode_; }
  void ToggleMode() {
    composer_->Clear();
    mode_ = mode_ == InputMode::Chinese ? InputMode::English : InputMode::Chinese;
  }
  const RimeView& view() const { return composer_->view(); }
  KeyDecision Apply(KeyState state, Hotkey hotkey);

 private:
  Composer* composer_;
  InputMode mode_ = InputMode::Chinese;
};

inline KeyDecision PinyinController::Apply(KeyState state, Hotkey hotkey) {
  // Caller's mode, composing, and candidate_count are ignored.
  const RimeView& current = view();
  state.mode = mode();
  state.composing = current.composing;
  state.candidate_count = static_cast<int>(current.page.size());
  state.ascii_punctuation = current.ascii_punctuation;
  const KeyDecision decision = RouteKey(state, hotkey);
  switch (decision.action) {
    case KeyAction::ToggleMode:
      ToggleMode();
      break;
    case KeyAction::OpenVoice:
      composer_->Clear();
      break;
    case KeyAction::PassAfterClear:
      composer_->Clear();
      break;
    case KeyAction::ToRime:
      composer_->ProcessKeyWithModifiers(state.vk,
          state.shift != (state.caps && state.vk>=0x41 && state.vk<=0x5a), state.ctrl, state.alt);
      break;
    case KeyAction::Pass:
    case KeyAction::Consume:
    case KeyAction::VoiceEnter:
    case KeyAction::VoiceEscape:
      break;
  }
  return decision;
}
