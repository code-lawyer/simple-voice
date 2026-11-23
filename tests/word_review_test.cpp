#include "harness.hpp"
#include "review_collector.hpp"
#include "data_backup.hpp"
#include "sqlite3.h"
#include "utf8.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <thread>

namespace {
const ReviewWord* Word(const std::vector<ReviewWord>& words,const std::string& text) {
  const auto found=std::find_if(words.begin(),words.end(),[&](const auto& row){return row.text==text;});
  return found==words.end()?nullptr:&*found;
}
}

void ReviewCountsTheRollingWindowAndHonorsConsent() {
  const auto path=(TestScratchDirectory()/"weekly.sqlite").u8string();
  Lexicon a(path),b(path);
  const auto now=std::time(nullptr);
  CHECK(!a.ReviewEpoch() && !a.ReviewSettings(now).since);
  a.SetReviewEnabled(true);
  CHECK(!a.ReviewSettings(now).since);
  CHECK(b.ReviewSettings(now).enabled);
  const auto epoch=b.ReviewEpoch();
  const std::vector<ReviewWord> old={{"轻音","早期短例",4,UserEdited}};
  b.RecordReview(old,WordSource::Voice,now-kReviewWindow,epoch);
  b.RecordReview({{"轻音","第一天的轻音",1,DraftDiffers}},WordSource::Voice,now-kReviewWindow+1,epoch);
  b.RecordReview({{"轻音","今天的轻音",2,0},{"单例","仅单例",1,0}},WordSource::Pinyin,now,epoch);
  auto frequent=a.ReviewWords(ReviewFilter::Frequent,"",40,0,now);
  CHECK(frequent.size()==1 && frequent[0].count==3 && frequent[0].sources==3 &&
        frequent[0].reasons==DraftDiffers && frequent[0].example=="今天的轻音");
  CHECK(a.ReviewWords(ReviewFilter::Uncertain,"",40,0,now).size()==1);
  CHECK(a.ReviewWords(ReviewFilter::All,"单",40,0,now).size()==1);
  a.ExpireReview(now+1);
  CHECK(a.ReviewSettings(now+1).since==now);
  CHECK(a.ReviewWords(ReviewFilter::Uncertain,"",40,0,now+1).empty());
  CHECK(a.ReviewWords(ReviewFilter::Frequent,"",40,0,now+1)[0].count==2);
  a.SetReviewEnabled(false); b.RecordReview(old,WordSource::Voice,now,epoch);
  CHECK(!b.ReviewEpoch() && a.ReviewWords(ReviewFilter::All,"轻",5,0,now)[0].count==2);
  a.SetReviewEnabled(true); b.ReviewSettings(now);
  const auto restarted=b.ReviewEpoch(); CHECK(restarted!=epoch);
  a.ClearReview(); b.RecordReview(old,WordSource::Voice,now,restarted);
  CHECK(a.ReviewWords(ReviewFilter::All,"",40,0,now).empty());
  CHECK(!a.ReviewSettings(now).since && a.Count()==0);

  b.ReviewSettings(now);
  const auto current=b.ReviewEpoch();
  const auto before_skip=std::chrono::system_clock::now()-std::chrono::milliseconds(1);
  b.RecordReview(old,WordSource::Voice,before_skip,current);
  a.SkipReview({"轻音"});
  b.RecordReview({{"轻音","旧回执",1,0},{"同句","保留其他词",1,0}},WordSource::Pinyin,before_skip,current);
  CHECK(!a.ReviewCandidateExists("轻音",now) && a.ReviewCandidateExists("同句",now));
  const auto after_skip=std::chrono::system_clock::now()+std::chrono::microseconds(1);
  b.RecordReview({{"轻音","新回执",1,0}},WordSource::Pinyin,after_skip,current);
  const auto reused=a.ReviewWords(ReviewFilter::All,"轻音",5,0,now);
  CHECK(reused.size()==1 && reused[0].count==1 && reused[0].example=="新回执");
  a.AcceptReview({{"轻音","清音","qingyin"}},now);
  b.RecordReview(old,WordSource::Voice,before_skip,current);
  CHECK(!a.ReviewCandidateExists("轻音",now));
  a.SaveTerm("同句","tongju",now);
  try {b.AcceptReview({{"同句","旧选择","jiuxuanze"}},now); CHECK(false);}
  catch(const std::invalid_argument&) {}
  CHECK(a.Search("jiuxuanze").empty());

  const auto table=TestScratchDirectory()/"weekly-personal.txt";
  auto publish=[&](Lexicon& db,const std::string& term) {
    db.SaveTerm(term,"ceshi",now);
    return db.ExportTerms(table.wstring());
  };
  auto first_export=std::async(std::launch::async,publish,std::ref(a),"并发甲");
  auto second_export=std::async(std::launch::async,publish,std::ref(b),"并发乙");
  CHECK(first_export.get());
  CHECK(second_export.get());
  std::ifstream published(table,std::ios::binary);
  const std::string snapshot{std::istreambuf_iterator<char>(published),{}};
  CHECK(snapshot.find("并发甲\tceshi\t100\n")!=snapshot.npos && snapshot.find("并发乙\tceshi\t100\n")!=snapshot.npos);
}

void ReviewBatchHonorsEditsAndExplicitSpeechSelection() {
  Lexicon db(":memory:");
  const auto now=std::time(nullptr);
  db.SetReviewEnabled(true);
  db.RecordReview({{"轻音","打开轻音输入法",2,UserEdited},{"清隆","同事清隆",1,DraftDiffers},
                   {"单词","测试单词",1,0}},WordSource::Voice,now,db.ReviewEpoch());
  const ReviewChoice extra{"轻音","轻音输入法","qingyinshurufa",true,true};
  try {db.AcceptReview({extra,{"过期","过期","guoqi"}},now); CHECK(false);}
  catch(const std::invalid_argument&) {}
  CHECK(db.Count()==0 && db.PendingSpeechNames().empty());
  db.AcceptReview({extra,{"清隆","青龙","qinglong"}},now);
  CHECK(db.Count()==2 && db.ReviewCandidateExists("轻音",now) && !db.ReviewCandidateExists("清隆",now));
  CHECK(db.Search("qinglong").size()==1 && db.PendingSpeechNames()==std::vector<std::string>{"轻音输入法"});
  db.CompleteSpeechNames({"轻音输入法"}); CHECK(db.PendingSpeechNames().empty());
  db.RecordReview({{"清龙","清龙旧拼写",1,0},{"京龙","京龙旧拼写",1,0}},WordSource::Voice,now,db.ReviewEpoch());
  try {db.AcceptReview({{"京龙","新词","xinci",true},{"清龙","青龙","long",true}},now); CHECK(false);}
  catch(const std::invalid_argument&) {}
  CHECK(db.Count()==2 && db.PendingSpeechNames().empty() && db.ReviewCandidateExists("京龙",now));
  try {db.AcceptReview({{"京龙","同词","tongci",true},{"清龙","同词","tc"}},now); CHECK(false);}
  catch(const std::invalid_argument&) {}
  CHECK(db.Count()==2 && db.ReviewCandidateExists("清龙",now));
  CHECK(!db.Commit("青龙",now));
  db.AcceptReview({{"清龙","青龙","qinglong"}},now);
  const auto already_saved=db.Search("qinglong");
  CHECK(already_saved.size()==1 && already_saved[0].count==1 && !db.ReviewCandidateExists("清龙",now));
  const auto root=TestScratchDirectory();
  const auto folder=CreateDataBackup(db,root.wstring(),root.wstring());
  std::ifstream in(std::filesystem::path(folder)/"SimpleVoice.backup",std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(in),{}};
  CHECK(bytes.find("轻音输入法")!=bytes.npos && bytes.find("打开轻音输入法")==bytes.npos &&
        bytes.find("测试单词")==bytes.npos);
  db.SkipReview({"轻音"}); CHECK(!db.ReviewCandidateExists("轻音",now));
  db.ClearReview(); CHECK(db.Count()==2 && !db.ReviewCandidateExists("单词",now));

  Lexicon expiry(":memory:");
  expiry.SaveTerm("保留","baoliu",now);
  expiry.SaveTerm("将到期","jiangdaoqi",now-365*86400);
  CHECK(!expiry.Commit("普通输入",now));
  CHECK(!expiry.Commit("保留",now));
  CHECK(expiry.Commit("普通输入",now+1));
  CHECK(!expiry.Commit("普通输入",now+2) && expiry.Count()==1);
}

void ReviewExplanationUsesTheThreeActualTexts() {
  Segmenter segmenter("third_party/cppjieba/dict");
  const auto pinyin=ExtractReviewWords(segmenter,WordSource::Pinyin,"轻音输入法");
  CHECK(Word(pinyin,"轻音输入法") && Word(pinyin,"轻音输入法")->count==1);
  const auto voice=ExtractReviewWords(segmenter,WordSource::Voice,"请打开轻音输入法的设置","请打开清音输入法的设置","请打开清音输入法的设置");
  const auto* term=Word(voice,"轻音输入法");
  CHECK(term && term->count==1 && term->reasons==UserEdited && term->example.find("轻音输入法")!=std::string::npos);
  Lexicon db(":memory:");const auto now=std::time(nullptr);db.SetReviewEnabled(true);
  for(int use=0;use<2;++use) db.RecordReview(pinyin,WordSource::Pinyin,now,db.ReviewEpoch());
  db.RecordReview(voice,WordSource::Voice,now,db.ReviewEpoch());
  const auto merged=db.ReviewWords(ReviewFilter::Frequent,"轻音输入法",5,0,now);
  CHECK(merged.size()==1 && merged[0].count==3 && merged[0].sources==3 && merged[0].reasons==UserEdited);
  const auto marked=ExtractReviewWords(segmenter,WordSource::Voice,"明天用 Codex 写文档","明天用 Codex 写文档","明天用 Cortex 写文档");
  CHECK(Word(marked,"Codex") && Word(marked,"Codex")->reasons==DraftDiffers);
  const auto unrelated=ExtractReviewWords(segmenter,WordSource::Voice,"使用 Codex 整理文件","使用 Codex 整理文件","假字");
  CHECK(std::none_of(unrelated.begin(),unrelated.end(),[](const auto& row){return row.reasons!=0;}));
  const auto retry=ExtractReviewWords(segmenter,WordSource::Voice,"Nine.","Nine.","",true);
  db.RecordReview(retry,WordSource::Voice,now,db.ReviewEpoch());
  const auto retried=db.ReviewWords(ReviewFilter::Uncertain,"Nine",5,0,now);
  CHECK(retried.size()==1 && retried[0].reasons==ShortRecognitionRetried);
  const auto ordinary=ExtractReviewWords(segmenter,WordSource::Voice,"今天用 CODEX，下午五点见。","今天用 Codex 下午5点见","今天用 codex");
  CHECK(std::none_of(ordinary.begin(),ordinary.end(),[](const auto& row){return row.reasons!=0;}));
  const auto repeat=ExtractReviewWords(segmenter,WordSource::Voice,
    "请打开 Codex 然后在另一个窗口继续使用 Codex 完成余下所有文档工作。");
  CHECK(Word(repeat,"Codex") && Word(repeat,"Codex")->count==2 && CodePointCount(Word(repeat,"Codex")->example)<=17);
  const auto technical=ExtractReviewWords(segmenter,WordSource::Voice,"用 C++ 调用 GPT-4o，再用 C++ 核对。");
  CHECK(Word(technical,"C++") && Word(technical,"C++")->count==2 && Word(technical,"GPT-4o"));
  std::string context;
  for(int i=0;i<220;++i) context+="今天 ";
  const auto long_written=context+"Codex "+context;
  const auto long_recognized=context+"Codax "+context;
  const auto long_draft=context+"Cortex "+context;
  const auto long_words=ExtractReviewWords(segmenter,WordSource::Voice,long_written,long_recognized,long_draft);
  const auto* corrected=Word(long_words,"Codex");
  const auto* surrounding=Word(long_words,"今天");
  CHECK(corrected && corrected->reasons==(UserEdited|DraftDiffers));
  CHECK(surrounding && surrounding->reasons==0 && surrounding->count==440);
}

void ReviewCollectorRecoversFromTemporaryDbLock() {
  const auto file=(TestScratchDirectory()/"review-locked.sqlite").u8string();
  Lexicon db(file);db.SetReviewEnabled(true);
  const auto now=std::time(nullptr);
  sqlite3* blocker=nullptr;
  CHECK(sqlite3_open(file.c_str(),&blocker)==SQLITE_OK);
  {
    ReviewCollector collector(db,"third_party/cppjieba/dict");
    collector.Submit(WordSource::Pinyin,"Codex");
    for(int i=0;i<300 && !db.ReviewCandidateExists("Codex",now);++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(db.ReviewCandidateExists("Codex",now));
    CHECK(sqlite3_exec(blocker,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
    collector.Submit(WordSource::Pinyin,"Codex");
    std::this_thread::sleep_for(std::chrono::milliseconds(1750));
    // This connection only holds the writer lock. Committing an empty transaction
    // can request an exclusive lock while the collector is retrying its write.
    CHECK(sqlite3_exec(blocker,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);
  }
  const auto rows=db.ReviewWords(ReviewFilter::Frequent,"Codex",5,0,now);
  CHECK(rows.size()==1 && rows[0].count==2);
  CHECK(sqlite3_exec(blocker,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)==SQLITE_OK);
  auto stalled=std::make_unique<ReviewCollector>(db,"third_party/cppjieba/dict");
  for(int i=0;i<530;++i) stalled->Submit(WordSource::Pinyin,"本地测试");
  CHECK(stalled->Failed());
  auto shutdown=std::async(std::launch::async,[collector=std::move(stalled)]() mutable {collector.reset();});
  const auto stopped=shutdown.wait_for(std::chrono::seconds(7));
  CHECK(sqlite3_exec(blocker,"ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK);
  shutdown.get();
  CHECK(stopped==std::future_status::ready);
  sqlite3_close(blocker);
  const auto bad=TestScratchDirectory()/"bad-review-dict";
  std::filesystem::create_directories(bad);
  std::ofstream(bad/"jieba.dict.utf8")<<"incomplete dictionary";
  try {Segmenter corrupted(bad.u8string()); CHECK(false);} catch(const std::runtime_error&) {}
}
