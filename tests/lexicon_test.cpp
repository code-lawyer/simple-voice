#include "harness.hpp"
#include "lexicon.hpp"

#include <cstdio>
#include <string>

namespace {

std::string TempDb() {
  static int n = 0;
  std::string path = "build/lexicon-" + std::to_string(++n) + ".sqlite";
  std::remove(path.c_str());
  return path;
}

}  // namespace

void FirstSentenceAndSingleCountDoNotReplace() {
  Lexicon lex(TempDb());
  const char* sentence = "明天下午三点在会议室见客户";
  std::vector<Token> tokens = {{"明天", "ming tian"}, {"客户", "ke hu"}};
  lex.Commit(sentence, tokens, 1000);
  for (const auto& entry : lex.Recent(10)) CHECK(entry.text != sentence);
  lex.Commit(sentence, tokens, 2000);
  CHECK(lex.Apply("明天开会见客户",
                  {{"明天", "ming tian"}, {"开会", "kai hui"}, {"见", "jian"}, {"客户", "ke hu"}},
                  2000) == "明天开会见客户");
  lex.Commit("的", {{"的", "de"}}, 3000);
  lex.Commit("的", {{"的", "de"}}, 4000);
  for (const auto& entry : lex.Recent(10)) CHECK(entry.text != "的");
}

void PreferredHomophoneReplacesOnlySamePinyin() {
  Lexicon lex(TempDb());
  lex.Commit("张杉", {{"张杉", "zhang shan"}}, 1000);
  lex.Commit("张杉", {{"张杉", "zhang shan"}}, 2000);
  CHECK(lex.Apply("张山到了", {{"张山", "zhang shan"}, {"到了", "dao le"}}, 2000) == "张杉到了");
  CHECK(lex.Apply("张三到了", {{"张三", "zhang san"}, {"到了", "dao le"}}, 2000) == "张三到了");
  CHECK(lex.Delete("张杉"));
  CHECK(lex.Apply("张山到了", {{"张山", "zhang shan"}, {"到了", "dao le"}}, 2000) == "张山到了");
}

void PunctuationDoesNotBlockARepeatedSentence() {
  Lexicon lex(TempDb());
  const char* sentence = "明天下午三点在会议室见客户";
  std::vector<Token> tokens = {{"明天", "ming tian"}, {"。", ""}, {"客户", "ke hu"}};
  lex.Commit(sentence, tokens, 1000);
  lex.Commit(sentence, tokens, 2000);
  CHECK(lex.Apply("明天。客户", tokens, 2000) == sentence);
}

void ExpiredEntriesDropOut() {
  Lexicon lex(TempDb());
  const auto day = 86400LL;
  lex.Commit("张杉", {{"张杉", "zhang shan"}}, 0);
  lex.Commit("张杉", {{"张杉", "zhang shan"}}, 1);
  CHECK(lex.Apply("张山", {{"张山", "zhang shan"}}, 180 * day + 10) == "张山");
  CHECK(lex.Recent(10).empty() == false);
  lex.Apply("张山", {{"张山", "zhang shan"}}, 366 * day);
  CHECK(lex.Recent(10).empty());
}
