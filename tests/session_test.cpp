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
  CHECK(s.phase() == Phase::Pending);
  CHECK(s.box_text() == "定稿。");
  CHECK(!s.TakeCommit(&committed));
  s.Enter();
  CHECK(s.TakeCommit(&committed));
  CHECK(committed == "定稿。");
  CHECK(!s.TakeLexiconWrite(&committed));
  CHECK(s.phase() == Phase::Committing);
  s.ConfirmInserted();
  CHECK(s.TakeLexiconWrite(&committed));
  CHECK(s.phase() == Phase::Idle);
  CHECK(!s.mic_open());
}

void EnterDuringListeningPreviewsBeforeSecondEnter() {
  Session s;
  s.Hotkey();
  s.OnDraft("草稿");
  s.Enter();
  CHECK(s.phase() == Phase::Finalizing);
  CHECK(!s.mic_open());
  s.Enter(); // A second press while calibration is running must not queue an insert.
  s.OnSpeechEnded();
  s.OnFinal("定稿。");
  std::string committed;
  CHECK(s.phase() == Phase::Pending);
  CHECK(s.box_text() == "定稿。");
  CHECK(!s.TakeCommit(&committed));
  CHECK(!s.TakeLexiconWrite(&committed));
  s.Enter();
  CHECK(s.TakeCommit(&committed));
  CHECK(committed == "定稿。");
  CHECK(s.phase() == Phase::Committing);
  s.ConfirmInserted();
  CHECK(s.phase() == Phase::Idle);
}

void RejectedInsertRestoresThePendingFinal() {
  Session s;
  s.Hotkey();
  s.OnSpeechEnded();
  s.OnFinal("定稿。");
  s.Enter();
  std::string text;
  CHECK(s.TakeCommit(&text));
  CHECK(text == "定稿。");
  s.RestorePending(text);
  CHECK(s.phase() == Phase::Pending);
  CHECK(s.box_text() == "定稿。");
  std::string ignored;
  CHECK(!s.TakeCommit(&ignored));
  CHECK(!s.TakeLexiconWrite(&ignored));
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
