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
};

class Composer {
 public:
  virtual ~Composer() = default;
  virtual RimeView ProcessKey(int vk, bool shift) = 0;
  virtual void Clear() = 0;
  virtual const RimeView& view() const = 0;
};

class PinyinController {
 public:
  explicit PinyinController(Composer* composer) : composer_(composer) {}
  InputMode mode() const { return mode_; }
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
  const KeyDecision decision = RouteKey(state, hotkey);
  switch (decision.action) {
    case KeyAction::ToggleMode:
      composer_->Clear();
      mode_ = mode() == InputMode::Chinese ? InputMode::English : InputMode::Chinese;
      break;
    case KeyAction::OpenVoice:
      composer_->Clear();
      break;
    case KeyAction::PassAfterClear:
      composer_->Clear();
      break;
    case KeyAction::ToRime:
      composer_->ProcessKey(state.vk, state.shift);
      break;
    case KeyAction::Pass:
    case KeyAction::Consume:
    case KeyAction::VoiceEnter:
    case KeyAction::VoiceEscape:
      break;
  }
  return decision;
}
