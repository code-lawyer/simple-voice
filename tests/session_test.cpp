#include "harness.hpp"
#include "session.hpp"

#include <string>

void SessionKeepsDraftUntilFinalThenEnterCommitsFinal() {
  Session s;
  s.Hotkey();
  s.OnDraft("草稿");
  s.OnSpeechEnded();
  s.Enter();
  std::string committed;
  CHECK(!s.TakeCommit(&committed));
  s.OnFinal("定稿。");
  CHECK(s.TakeCommit(&committed));
  CHECK(committed == "定稿。");
  CHECK(s.TakeLexiconWrite(&committed));
  CHECK(s.phase() == Phase::Idle);
  CHECK(!s.mic_open());
}

void EscapeAndRestartDoNotCommit() {
  Session s;
  s.Hotkey();
  s.OnDraft("错");
  s.OnSpeechEnded();
  s.OnFinal("错。");
  s.Escape();
  std::string committed;
  CHECK(!s.TakeCommit(&committed));
  CHECK(!s.TakeLexiconWrite(&committed));
  s.Hotkey();
  s.OnSpeechEnded();
  s.OnFinal("另一句。");
  s.Hotkey();
  CHECK(s.phase() == Phase::Listening);
  CHECK(!s.TakeCommit(&committed));
}
