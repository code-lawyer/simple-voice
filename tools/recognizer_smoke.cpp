#include "sherpa_recognizer.hpp"
#include "session.hpp"
#include "sqlite3.h"
#include <filesystem>
#include "lexicon.hpp"
#include "segmenter.hpp"
#include "resample.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <vector>
#include <sstream>
#include "recognition_text.hpp"

static unsigned U16(const std::vector<unsigned char>& b, std::size_t p) {
  return b[p] | unsigned(b[p + 1]) << 8;
}
static unsigned U32(const std::vector<unsigned char>& b, std::size_t p) {
  return U16(b, p) | U16(b, p + 2) << 16;
}
static std::vector<std::int16_t> ReadWave(const char* file) {
  std::ifstream in(file, std::ios::binary);
  const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), {}};
  if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) return {};
  unsigned rate = 0;
  bool pcm16mono = false;
  for (std::size_t p = 12; p + 8 <= bytes.size();) {
    const auto size = U32(bytes, p + 4);
    if (size > bytes.size() - p - 8) return {};
    if (std::memcmp(bytes.data() + p, "fmt ", 4) == 0 && size >= 16) {
      pcm16mono = U16(bytes, p + 8) == 1 && U16(bytes, p + 10) == 1 && U16(bytes, p + 22) == 16;
      rate = U32(bytes, p + 12);
    }
    if (std::memcmp(bytes.data() + p, "data", 4) == 0 && pcm16mono && rate > 0) {
      std::vector<std::int16_t> samples(size / 2);
      std::memcpy(samples.data(), bytes.data() + p + 8, samples.size() * 2);
      return rate == 16000 ? samples : ResampleTo16k(samples.data(), samples.size(), rate);
    }
    p += 8 + size + (size & 1);
  }
  return {};
}

static std::string Fold(std::string text) { for(char& c:text) if(c>='A'&&c<='Z') c+=32; return text; }
static int Corpus(const char* path,bool chinese) {
  using Clock=std::chrono::steady_clock;
  auto recognizer=MakeSherpaRecognizer("models",nullptr,nullptr,chinese);
  if(!recognizer) return 2;
  std::ifstream list(path); std::string line; int passed=0,failed=0;
  while(std::getline(list,line)) {
    if(!line.empty()&&line.back()=='\r')line.pop_back(); if(line.empty()||line.front()=='#')continue;
    std::istringstream row(line); std::string id,file,expected; std::getline(row,id,'\t'); std::getline(row,file,'\t'); std::getline(row,expected,'\t');
    const auto samples=ReadWave(file.c_str()); if(samples.empty()) {std::fprintf(stderr,"Unreadable corpus fixture %s\n",id.c_str());return 2;}
    recognizer->Reset();
    for(std::size_t offset=0;offset<samples.size();offset+=320) recognizer->AcceptAudio(samples.data()+offset,std::min<std::size_t>(320,samples.size()-offset));
    const auto start=Clock::now(); const auto final=recognizer->Finalize(); const auto raw=recognizer->Original();
    bool ok=!HasUnknownSpeech(final) && PreserveRecognizedContent(raw,final)==final;
    if(expected=="<empty>") ok=ok&&final.empty();
    else if(expected!="<observe>") {
      ok=ok&&!final.empty(); const auto folded=Fold(final); std::istringstream words(expected); std::string word;
      while(std::getline(words,word,';')) {
        const auto equal=word.find('='); const int count=equal==std::string::npos?1:std::stoi(word.substr(equal+1));
        if(equal!=std::string::npos)word.resize(equal);
        std::size_t offset=0; int found=0; while(!word.empty()&&(offset=folded.find(Fold(word),offset))!=std::string::npos){++found;offset+=word.size();}
        if(found<count) ok=false;
      }
    }
    if(!final.empty()) { Session preview; preview.Hotkey(); preview.Enter(); preview.OnFinal(final); std::string commit;
      ok=ok&&preview.phase()==Phase::Pending&&!preview.TakeCommit(&commit)&&preview.box_text()==final; preview.Enter(); ok=ok&&preview.TakeCommit(&commit)&&commit==final; }
    std::printf("%s %s duration=%.2fs final_ms=%lld\nraw=%s\nfinal=%s\nwarning=%s\n",ok?"PASS":"FAIL",id.c_str(),samples.size()/16000.0,
      static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-start).count()),raw.c_str(),final.c_str(),recognizer->TakeWarning().c_str()); std::fflush(stdout);
    if(ok)++passed;else ++failed;
  }
  PROCESS_MEMORY_COUNTERS memory{};GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory));
  std::printf("corpus_passed=%d corpus_failed=%d peak_working_set_mib=%.1f\n",passed,failed,memory.PeakWorkingSetSize/1048576.0);
  return passed>0&&failed==0?0:1;
}
int main(int argc,char** argv) {
  if(argc>=2) return Corpus(argv[1],argc>=3 && std::string(argv[2])=="--chinese");
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  using Clock = std::chrono::steady_clock;
  const auto began = Clock::now();
  Segmenter segmenter("third_party/cppjieba/dict");
  const std::string db_path="build/audit-2026-11-15/recognizer-fault.sqlite";
  std::filesystem::create_directories("build/audit-2026-11-15");
  std::filesystem::remove(db_path);
  Lexicon lexicon(db_path);
  auto recognizer = MakeSherpaRecognizer("models", &segmenter, &lexicon);
  if (!recognizer) return 2;
  std::printf("model_load_ms=%lld\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()));
  if (!recognizer->Finalize().empty()) return 1;
  for (const char* file : {"models/final/test_wavs/0.wav", "models/final/test_wavs/1.wav", "models/final/test_wavs/8k.wav"}) {
    auto samples = ReadWave(file);
    if (samples.empty()) { std::fprintf(stderr, "unreadable wave: %s\n", file); return 2; }
    recognizer->Reset();
    const auto started = Clock::now();
    for (std::size_t offset = 0; offset < samples.size(); offset += 320) {
      recognizer->AcceptAudio(samples.data() + offset, std::min<std::size_t>(320, samples.size() - offset));
    }
    const auto draft = recognizer->Partial();
    const auto finalize_start = Clock::now();
    const auto final = recognizer->Finalize();
    const auto finished = Clock::now();
    std::printf("fixture=%s duration_s=%.2f inference_ms=%lld final_ms=%lld\ndraft=%s\nfinal=%s\n", file,
        samples.size() / 16000.0,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count()),
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(finished - finalize_start).count()),
        draft.c_str(), final.c_str());
    if (draft.empty() || final.empty()) return 1;
    Session preview;
    preview.Hotkey(); preview.OnDraft(draft); preview.Enter(); preview.OnFinal(final);
    std::string inserted;
    if (preview.phase()!=Phase::Pending || preview.box_text()!=final || preview.TakeCommit(&inserted)) return 1;
    preview.Enter();
    if (!preview.TakeCommit(&inserted) || inserted!=final) return 1;
    std::puts("PASS real calibrated text is previewed before confirmation and inserted unchanged");
  }
  PROCESS_MEMORY_COUNTERS memory{};
  GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
  std::printf("working_set_mib=%.1f peak_working_set_mib=%.1f\n", memory.WorkingSetSize / 1048576.0, memory.PeakWorkingSetSize / 1048576.0);
  sqlite3* database=nullptr;
  if (sqlite3_open(db_path.c_str(),&database)!=SQLITE_OK) return 2;
  const auto sql_rc=sqlite3_exec(database,"DROP TABLE entries",nullptr,nullptr,nullptr);
  sqlite3_close(database);
  if (sql_rc!=SQLITE_OK) return 2;
  recognizer->Reset();
  const auto samples=ReadWave("models/final/test_wavs/0.wav");
  recognizer->AcceptAudio(samples.data(),samples.size());
  if (recognizer->Finalize().empty() || !recognizer->TakeWarning().empty()) return 1;
  std::puts("3 real-model fixtures passed; broken legacy dictionary cannot affect recognition");
  return 0;
}
