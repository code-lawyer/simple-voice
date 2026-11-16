#include "harness.hpp"
#include "lexicon.hpp"
#include "session.hpp"

namespace {
// Exercise the production confirmation boundary: the session owns text, while
// the lexicon receives a copy only after insertion has been acknowledged.
std::string Confirm(Session& session, Lexicon& lex, const std::string& text) {
  session.Hotkey(); session.OnSpeechEnded(); session.OnFinal(text); session.Enter();
  std::string committed, used;
  CHECK(session.TakeCommit(&committed));
  CHECK(!session.TakeLexiconWrite(&used));
  session.ConfirmInserted();
  CHECK(session.TakeLexiconWrite(&used));
  CHECK(used==committed);
  lex.Commit(used,1002);
  return committed;
}
}

void FirstSentenceAndSingleCountDoNotReplace() {
  Lexicon lex(":memory:");
  for(int n=0;n<10;++n) lex.Commit("明天下午三点在会议室见客户",1000+n);
  CHECK(lex.Recent(20).empty()); CHECK(lex.LegacyCount()==0);
}
void PersonalHomophonesNeverRewriteSpeech() {
  Lexicon lex(":memory:"); lex.SaveTerm("张杉","zhangshan",1000);
  lex.Commit("张杉",1001); lex.Commit("张杉",1002);
  Session session;
  CHECK(Confirm(session,lex,"张山到了")=="张山到了");
  CHECK(lex.Search("张山").empty());
  CHECK(lex.Delete("张杉")); lex.Commit("张杉",1003); CHECK(lex.Count()==0);
}
void RepeatedSentencesNeverOverwriteNewInformation() {
  Lexicon lex(":memory:");
  for(const char* prior:{"今天处理10个文件。","今天使用Claude处理文件。","时间"}) {
    lex.Commit(prior,1000); lex.Commit(prior,1001);
  }
  Session session;
  for(const char* text:{"今天处理20个文件。","今天使用Codex处理文件。","把理论用于实践。","明天。客户"})
    CHECK(Confirm(session,lex,text)==text);
  CHECK(lex.Count()==0); CHECK(lex.LegacyCount()==0);
}
void ExpiredEntriesDropOut() {
  Lexicon lex(":memory:"); const auto day=86400LL;
  lex.SaveTerm("张杉","zhangshan",0);
  lex.Commit("",365*day); CHECK(lex.Count()==1);
  lex.Commit("",365*day+1); CHECK(lex.Count()==0);
}
