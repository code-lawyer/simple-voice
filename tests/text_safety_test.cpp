#include "harness.hpp"
#include "lexicon.hpp"
#include "recognition_text.hpp"
#include "capture_health.hpp"
#include "audio_segments.hpp"
#include "settings.hpp"
#include <filesystem>
#include <fstream>
#include <ctime>

void RecognitionFormattingPreservesInformation() {
  CHECK(SameShortRecognition(" Nine. ","9"));
  CHECK(SameShortRecognition("零。","Zero."));
  CHECK(!SameShortRecognition("1.5","15"));
  CHECK(!SameShortRecognition("six","七"));
  CHECK(!SameShortRecognition("",""));
  CHECK(PreserveRecognizedContent("今天使用GitHub处理20个文件","今天使用GitHub处理20个文件。") == "今天使用GitHub处理20个文件。");
  for(const auto& bad:{"今天使用Github处理20个文件。","今天使用GitHub处理10个文件。","今天处理20个文件。"})
    CHECK(PreserveRecognizedContent("今天使用GitHub处理20个文件",bad)=="今天使用GitHub处理20个文件");
  CHECK(PreserveRecognizedContent("价格1.5元","价格15元。") == "价格1.5元");
  CHECK(PreserveRecognizedContent("10:30开始","1030开始。") == "10:30开始");
  CHECK(PreserveRecognizedContent("v1.2.3","v12.3。") == "v1.2.3");
  CHECK(PreserveRecognizedContent("GitHub API","Git Hub API.")=="GitHub API");
  CHECK(PreserveRecognizedContent("10 20","1020。")=="10 20");
  CHECK(PreserveRecognizedContent("a part","apart.")=="a part");
  CHECK(PreserveRecognizedContent("values 1 5","values 1.5.")=="values 1 5");
  CHECK(PreserveRecognizedContent("10:30 和 10.30","10.30 和 10:30。")=="10:30 和 10.30");
  CHECK(PreserveRecognizedContent("剂量 .5 和 1,000","剂量 5 和 1,000。")=="剂量 .5 和 1,000");
  CHECK(PreserveRecognizedContent("at 10:30 pay 10.30","at 10:30, pay 10.30.")=="at 10:30, pay 10.30.");
  CHECK(PreserveRecognizedContent("don't change foo_bar","dont change foo bar.")=="don't change foo_bar");
  CHECK(HasUnknownSpeech("这是<unk>")); CHECK(HasUnknownSpeech("< unk >。"));
  CHECK(!HasUnknownSpeech("unknown"));
}

void AudioReadinessAndChunkingProtectBoundaries() {
  CaptureHealth no_frames(100); CHECK(!no_frames.received()); CHECK(no_frames.Error(5099)==nullptr); CHECK(no_frames.Error(5100)!=nullptr);
  CaptureHealth silent(0); std::vector<std::int16_t> samples(16000*8,0);
  silent.Push(samples.data(),samples.size(),8000); CHECK(silent.received()); CHECK(silent.level()==0);
  CHECK(silent.Error(8000)==nullptr); CHECK(silent.quiet()); CHECK(silent.Error(13000)!=nullptr);
  CaptureHealth quiet(0);samples.assign(16000*12,20);
  quiet.Push(samples.data(),samples.size(),12000);CHECK(quiet.Error(12000)==nullptr);CHECK(quiet.quiet());
  CaptureHealth signal(0); std::vector<std::int16_t> voice(320,1500); signal.Push(voice.data(),voice.size(),20);
  CHECK(signal.received()); CHECK(signal.level()>0); CHECK(signal.Error(20)==nullptr); CHECK(signal.Error(5020)!=nullptr);
  quiet.Push(voice.data(),voice.size(),12020);CHECK(!quiet.quiet());
  samples.assign(16000*8,0);
  quiet.Push(samples.data(),samples.size(),20020);CHECK(quiet.quiet() && !quiet.Error(20020));
  quiet.Push(voice.data(),voice.size(),20040);CHECK(!quiet.quiet());
  samples.assign(16000*60,2000);
  for(int second:{12,24,36,48}) std::fill(samples.begin()+second*16000,samples.begin()+second*16000+1600,0);
  const auto parts=SegmentAudio(samples); std::size_t covered=0;
  for(const auto& part:parts) { CHECK(part.offset==covered); CHECK(part.count>0 && part.count<=15*16000); covered+=part.count; }
  CHECK(covered==samples.size()); CHECK(parts.size()>=3); CHECK(!parts.front().forced);
  CHECK(SegmentAudio({}).empty());
  samples.assign(16*16000,1500);
  // A short dip inside a word is quieter than the true, softly noisy pause.
  std::fill_n(samples.begin()+11*16000,320,0);
  std::fill_n(samples.begin()+13*16000,3200,30);
  CHECK(SegmentAudio(samples).front().count==13*16000+1600);
  std::size_t begin=0,end=0;
  samples.assign(8000,0);
  samples[3000]=15000;
  CHECK(!ShortVoicedRegion(samples,begin,end));
  for(std::size_t i=2000;i<5200;++i)
    samples[i]=static_cast<std::int16_t>(4000*std::sin(i*2*3.14159265/80));
  CHECK(ShortVoicedRegion(samples,begin,end));
  SpeechPause pause;
  std::vector<std::int16_t> speech(5*SpeechPause::rate,1500);
  CHECK(!pause.Scan(speech,0) && !pause.PreviewEnd());
  speech.insert(speech.end(),3*SpeechPause::frame,0);
  CHECK(!pause.Scan(speech,0) && !pause.ShortPreviewEnd());
  speech.insert(speech.end(),SpeechPause::frame,0);
  CHECK(!pause.Scan(speech,0) && pause.ShortPreviewEnd()==speech.size() && !pause.PreviewEnd());
  speech.insert(speech.end(),SpeechPause::tail-4*SpeechPause::frame,0);
  CHECK(!pause.Scan(speech,0) && pause.PreviewEnd()==speech.size());
  speech.push_back(900);CHECK(!pause.QuietRemainder(speech));
  speech.resize(speech.size()+2*SpeechPause::rate-1,900);
  CHECK(!pause.Scan(speech,0) && !pause.PreviewEnd() && !pause.ShortPreviewEnd());
  const auto last_sound=speech.size();
  speech.insert(speech.end(),SpeechPause::rate*13/20,0);
  CHECK(!pause.Scan(speech,0) && pause.PreviewEnd()==last_sound+SpeechPause::tail);
  speech.insert(speech.end(),SpeechPause::frame,700);
  CHECK(!pause.Scan(speech,last_sound) && pause.PendingCut() && !pause.PreviewEnd());
  CHECK(pause.Scan(speech,speech.size()-SpeechPause::frame)==last_sound+SpeechPause::tail);
  InputPreferences preferences; preferences.learning=false; preferences.chinese_only=true; preferences.pause_ms=1200;
  preferences.microphone=L"{0.0.1.test}.测试设备";
  const auto file = (TestScratchDirectory() / L"preferences.ini").wstring();
  CHECK(SaveInputPreferences(file,preferences)); const auto loaded=LoadInputPreferences(file);
  CHECK(!loaded.learning && loaded.chinese_only && loaded.pause_ms==1200 && loaded.microphone==preferences.microphone);
  preferences.microphone=L"device\nlearning=1"; CHECK(!SaveInputPreferences(file,preferences));
  CHECK(!LoadInputPreferences(file).learning);
}
void PersonalTermsAreExplicitSearchableAndReversible() {
  Lexicon lex(":memory:"); const auto now=std::time(nullptr);
  for(int i=0;i<1000;++i) lex.Commit("今天讨论项目进度",now);
  CHECK(lex.Count()==0); CHECK(lex.LegacyCount()==0);
  lex.SaveTerm("Simple Voice","simplevoice",now);
  lex.SaveTerm("轻音输入法","qingyin",now);
  CHECK(lex.Search("VOICE").size()==1); CHECK(lex.Search("qingyin").size()==1);
  CHECK(lex.Search("%_").empty());
  lex.Commit("轻音输入法",now+1);
  const auto before=lex.Search("轻音").at(0);
  Entry removed;
  CHECK(lex.Delete(before.text,&removed)); lex.Commit(before.text,now+2); CHECK(lex.Search("轻音").empty());
  CHECK(lex.RestoreDeletedTerm(removed));
  const auto restored=lex.Search("轻音").at(0);
  CHECK(restored.pinyin==before.pinyin && restored.count==before.count && before.count==1);
  CHECK(restored.last_commit_unix==before.last_commit_unix && restored.kind==before.kind);
  CHECK(lex.Count()==2);
  lex.SaveTerm("轻音","qy",now+1,before.text); CHECK(lex.Search("qingyin").empty());
  CHECK(lex.Search("qy").at(0).text=="轻音");
  bool rejected=false; try{lex.SaveTerm("bad\ntext","bad",now);}catch(...){rejected=true;} CHECK(rejected);
  rejected=false; try{lex.SaveTerm("bad","bad\tkey",now);}catch(...){rejected=true;} CHECK(rejected);
  for(const auto& invalid:{"123","ai1","GitHub"}) {
    rejected=false; try{lex.SaveTerm("invalid code",invalid,now);}catch(...){rejected=true;} CHECK(rejected);
  }
  const auto path = TestScratchDirectory() / L"voiceime_personal.txt";
  CHECK(lex.ExportTerms(path.wstring()));
  std::ifstream file(path);
  std::string body((std::istreambuf_iterator<char>(file)),{});
  CHECK(body.find("轻音\tqy\t")!=std::string::npos);
  Lexicon large(":memory:");
  for(int i=0;i<10000;++i) large.SaveTerm("term "+std::to_string(i),"term",now);
  CHECK(large.Count()==10000); CHECK(large.Search("term",100,9900).size()==100);
  CHECK(large.Search("term",100,10000).empty());
  rejected=false; try{large.SaveTerm("overflow","overflow",now);}catch(...){rejected=true;} CHECK(rejected);
  large.SaveTerm("term 0","updated",now+1); CHECK(large.Search("updated").size()==1);
  CHECK(large.Count()==10000);
}
