#include "harness.hpp"
#include "lexicon.hpp"

void FirstSentenceAndSingleCountDoNotReplace() {
  Lexicon lex(":memory:");
  for(int n=0;n<10;++n) lex.Commit("明天下午三点在会议室见客户",{{"明天","ming tian"},{"客户","ke hu"}},1000+n);
  CHECK(lex.Recent(20).empty()); CHECK(lex.LegacyCount()==0);
}
void PersonalHomophonesNeverRewriteSpeech() {
  Lexicon lex(":memory:"); lex.SaveTerm("张杉","zhangshan",1000);
  lex.Commit("张杉",{},1001); lex.Commit("张杉",{},1002);
  CHECK(lex.Apply("张山到了",{{"张山","zhang shan"}},1002)=="张山到了");
  CHECK(lex.Delete("张杉")); lex.Commit("张杉",{},1003); CHECK(lex.Count()==0);
}
void RepeatedSentencesNeverOverwriteNewInformation() {
  Lexicon lex(":memory:");
  for(const char* prior:{"今天处理10个文件。","今天使用Claude处理文件。","时间"}) {
    lex.Commit(prior,{},1000); lex.Commit(prior,{},1001);
  }
  for(const char* text:{"今天处理20个文件。","今天使用Codex处理文件。","把理论用于实践。","明天。客户"})
    CHECK(lex.Apply(text,{{"明天","ming tian"},{"客户","ke hu"}},1002)==text);
}
void ExpiredEntriesDropOut() {
  Lexicon lex(":memory:"); const auto day=86400LL;
  lex.SaveTerm("张杉","zhangshan",0);
  lex.Apply("",{},365*day); CHECK(lex.Count()==1);
  lex.Apply("",{},365*day+1); CHECK(lex.Count()==0);
}
