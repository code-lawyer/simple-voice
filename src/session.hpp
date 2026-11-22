#pragma once

#include <string>

// Append new phases to preserve the existing IPC values.
enum class Phase { Idle, Listening, Finalizing, Pending, Committing, Stopped };

class Session {
 public:
  Phase phase() const;
  bool mic_open() const;
  bool can_finalize() const;
  const std::string& box_text() const;
  void Hotkey();
  void OnDraft(std::string text);
  void OnSpeechEnded();
  void OnCaptureLimit();
  void OnCaptureDrained(std::string text);
  void OnFinal(std::string punctuated);
  void Enter();
  void RestorePending(std::string text);
  void ConfirmInserted();
  void Escape();
  bool TakeCommit(std::string* text);
  bool TakeLexiconWrite(std::string* text);

 private:
  Phase phase_ = Phase::Idle;
  bool mic_open_ = false;
  bool commit_ready_ = false;
  bool lexicon_ready_ = false;
  std::string box_;
  std::string committed_;
};
