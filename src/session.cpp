#include "session.hpp"

#include <utility>

Phase Session::phase() const { return phase_; }

bool Session::mic_open() const { return mic_open_; }

bool Session::can_finalize() const { return phase_ == Phase::Listening || phase_ == Phase::Stopped; }

const std::string& Session::box_text() const { return box_; }

void Session::Hotkey() {
  if (phase_ == Phase::Finalizing || phase_ == Phase::Committing) return;
  if (can_finalize()) {
    OnSpeechEnded();
    return;
  }
  phase_ = Phase::Listening;
  mic_open_ = true;
  box_.clear();
  commit_ready_ = lexicon_ready_ = false;
  committed_.clear();
}

void Session::OnDraft(std::string text) {
  if (phase_ != Phase::Listening) return;
  box_ = std::move(text);
}

void Session::OnSpeechEnded() {
  if (!can_finalize()) return;
  phase_ = Phase::Finalizing;
  mic_open_ = false;
}

void Session::OnCaptureLimit() {
  if (phase_ != Phase::Listening) return;
  phase_ = Phase::Stopped;
  mic_open_ = false;
}

void Session::OnFinal(std::string punctuated) {
  if (phase_ != Phase::Finalizing) return;
  mic_open_ = false;
  box_ = std::move(punctuated);
  phase_ = Phase::Pending;
}

void Session::Enter() {
  if (can_finalize()) {
    OnSpeechEnded();
    return;
  }
  if (phase_ != Phase::Pending || box_.empty()) return;
  committed_ = box_;
  commit_ready_ = true;
  lexicon_ready_ = false;
  phase_ = Phase::Committing;
  mic_open_ = false;
}

void Session::ConfirmInserted() {
  if (phase_ != Phase::Committing) return;
  phase_ = Phase::Idle;
  lexicon_ready_ = true;
  box_.clear();
}

void Session::RestorePending(std::string text) {
  phase_ = Phase::Pending;
  mic_open_ = false;
  commit_ready_ = false;
  lexicon_ready_ = false;
  committed_.clear();
  box_ = std::move(text);
}

void Session::Escape() {
  if (!can_finalize() && phase_ != Phase::Finalizing && phase_ != Phase::Pending) return;
  phase_ = Phase::Idle;
  mic_open_ = false;
  box_.clear();
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
