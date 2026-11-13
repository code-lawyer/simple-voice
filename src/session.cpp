#include "session.hpp"

#include <utility>

Phase Session::phase() const { return phase_; }

bool Session::mic_open() const { return mic_open_; }

const std::string& Session::box_text() const { return box_; }

void Session::Hotkey() {
  if (phase_ == Phase::Finalizing) return;
  if (phase_ == Phase::Listening) {
    phase_ = Phase::Finalizing;
    mic_open_ = false;
    return;
  }
  phase_ = Phase::Listening;
  mic_open_ = true;
  box_.clear();
  enter_during_final_ = false;
}

void Session::OnDraft(std::string text) {
  if (phase_ != Phase::Listening) return;
  box_ = std::move(text);
}

void Session::OnSpeechEnded() {
  if (phase_ != Phase::Listening) return;
  phase_ = Phase::Finalizing;
  mic_open_ = false;
}

void Session::OnFinal(std::string punctuated) {
  if (phase_ != Phase::Finalizing) return;
  mic_open_ = false;
  if (enter_during_final_) {
    committed_ = std::move(punctuated);
    commit_ready_ = true;
    lexicon_ready_ = true;
    box_.clear();
    phase_ = Phase::Idle;
    enter_during_final_ = false;
    return;
  }
  box_ = std::move(punctuated);
  phase_ = Phase::Pending;
}

void Session::Enter() {
  if (phase_ == Phase::Listening || phase_ == Phase::Finalizing) {
    enter_during_final_ = true;
    return;
  }
  if (phase_ != Phase::Pending) return;
  committed_ = box_;
  commit_ready_ = true;
  lexicon_ready_ = true;
  box_.clear();
  phase_ = Phase::Idle;
  mic_open_ = false;
}

void Session::RestorePending(std::string text) {
  phase_ = Phase::Pending;
  mic_open_ = false;
  enter_during_final_ = false;
  commit_ready_ = false;
  lexicon_ready_ = false;
  committed_.clear();
  box_ = std::move(text);
}

void Session::Escape() {
  if (phase_ != Phase::Listening && phase_ != Phase::Finalizing && phase_ != Phase::Pending) return;
  phase_ = Phase::Idle;
  mic_open_ = false;
  box_.clear();
  enter_during_final_ = false;
}

bool Session::TakeCommit(std::string* text) {
  if (!commit_ready_) return false;
  commit_ready_ = false;
  if (text != nullptr) *text = committed_;
  return true;
}

bool Session::TakeLexiconWrite(std::string* text) {
  if (!lexicon_ready_) return false;
  lexicon_ready_ = false;
  if (text != nullptr) *text = committed_;
  return true;
}
