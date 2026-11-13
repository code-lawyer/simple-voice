#pragma once

#include <string>

enum class Phase { Idle, Listening, Finalizing, Pending };

class Session {
 public:
  Phase phase() const;
  bool mic_open() const;
  const std::string& box_text() const;
  void Hotkey();
  void OnDraft(std::string text);
  void OnSpeechEnded();
  void OnFinal(std::string punctuated);
  void Enter();
  void Escape();
  bool TakeCommit(std::string* text);
  bool TakeLexiconWrite(std::string* text);

 private:
  Phase phase_ = Phase::Idle;
  bool mic_open_ = false;
  bool enter_during_final_ = false;
  bool commit_ready_ = false;
  bool lexicon_ready_ = false;
  std::string box_;
  std::string committed_;
};
