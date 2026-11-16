// Isolated benchmark. No production configuration, user data, or microphone access.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <sherpa-onnx/c-api/c-api.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point t) { return std::chrono::duration<double,std::milli>(Clock::now()-t).count(); }
static std::string Q(const std::string& s) {
  std::ostringstream o; o << '"';
  for (unsigned char c:s) {
    if(c=='"'||c=='\\') o << '\\' << c;
    else if(c=='\n') o << "\\n";
    else if(c=='\r') o << "\\r";
    else if(c=='\t') o << "\\t";
    else if(c<32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c) << std::dec;
    else o << c;
  } return o.str()+'"';
}
struct Memory { size_t working=0,peak=0,private_bytes=0; };
static Memory Mem() { PROCESS_MEMORY_COUNTERS_EX p{};p.cb=sizeof(p);GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&p),sizeof(p));return {p.WorkingSetSize,p.PeakWorkingSetSize,p.PrivateUsage}; }
static void WriteMem(std::ostream& o,Memory m) {o<<",\"working_set_bytes\":"<<m.working<<",\"peak_working_set_bytes\":"<<m.peak<<",\"private_bytes\":"<<m.private_bytes;}

#define API_LIST(X) \
X(SherpaOnnxCreateOnlineRecognizer) X(SherpaOnnxDestroyOnlineRecognizer) \
X(SherpaOnnxCreateOnlineStream) X(SherpaOnnxDestroyOnlineStream) \
X(SherpaOnnxOnlineStreamAcceptWaveform) X(SherpaOnnxIsOnlineStreamReady) \
X(SherpaOnnxDecodeOnlineStream) X(SherpaOnnxGetOnlineStreamResult) \
X(SherpaOnnxDestroyOnlineRecognizerResult) X(SherpaOnnxOnlineStreamInputFinished) \
X(SherpaOnnxOnlineStreamSetOption) \
X(SherpaOnnxCreateOfflineRecognizer) X(SherpaOnnxDestroyOfflineRecognizer) \
X(SherpaOnnxCreateOfflineStream) X(SherpaOnnxDestroyOfflineStream) \
X(SherpaOnnxOfflineStreamSetOption) \
X(SherpaOnnxAcceptWaveformOffline) X(SherpaOnnxDecodeOfflineStream) \
X(SherpaOnnxGetOfflineStreamResult) X(SherpaOnnxDestroyOfflineRecognizerResult) \
X(SherpaOnnxReadWave) X(SherpaOnnxFreeWave)
struct Api {
  HMODULE dll=nullptr;
#define DECL(n) decltype(&n) n=nullptr;
  API_LIST(DECL)
#undef DECL
  explicit Api(const std::string& path) {
    const int n=MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,nullptr,0);std::wstring wide(n,L'\0');MultiByteToWideChar(CP_UTF8,0,path.c_str(),-1,wide.data(),n);
    dll=LoadLibraryExW(wide.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!dll) throw std::runtime_error("LoadLibrary failed "+std::to_string(GetLastError()));
#define BIND(name) {auto fp=GetProcAddress(dll,#name);static_assert(sizeof(name)==sizeof(fp));std::memcpy(&name,&fp,sizeof(fp));if(!name)throw std::runtime_error("missing export " #name);}
    API_LIST(BIND)
#undef BIND
  }
  ~Api(){if(dll)FreeLibrary(dll);}
};
struct Sample {std::string id,path;};
static std::vector<Sample> Corpus(const char* path) {
  std::ifstream f(std::filesystem::u8path(path)); if(!f)throw std::runtime_error("cannot read corpus");std::vector<Sample> samples;std::string row;
  while(std::getline(f,row)){if(!row.empty()&&row.back()=='\r')row.pop_back();if(row.empty()||row[0]=='#')continue;auto p=row.find('\t');if(p==std::string::npos)throw std::runtime_error("bad corpus row");samples.push_back({row.substr(0,p),row.substr(p+1)});}return samples;
}
static int Run(int argc,char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);SetConsoleOutputCP(CP_UTF8);
  try {
    if(argc<7) {std::cerr<<"benchmark <kind> <model-dir> <corpus.tsv> <out.jsonl> <run-id> <runtime-dll> [threads=2] [suite=main]\n";return 2;}
    const std::string kind=argv[1],dir=argv[2];const int run=std::stoi(argv[5]);
    const int threads=argc>7?std::stoi(argv[7]):2;const std::string suite=argc>8?argv[8]:"main";
    if(threads<1||threads>64||run<1)throw std::runtime_error("invalid threads or run ID");
    Api a(argv[6]);std::ofstream out(std::filesystem::u8path(argv[4]),std::ios::binary);if(!out)throw std::runtime_error("cannot write output");out<<std::fixed<<std::setprecision(3);
    const auto corpus=Corpus(argv[3]);const auto baseline=Mem();
    const std::string tokens=dir+"/tokens.txt",model=dir+"/model.int8.onnx";
    std::string encoder,decoder,joiner,frontend,tokenizer;
    const bool online=kind=="ctc_online"||kind=="zip_online"||kind=="para_online"||kind=="para_online_final";
    const SherpaOnnxOnlineRecognizer* on=nullptr;const SherpaOnnxOfflineRecognizer* off=nullptr;
    const auto load=Clock::now();
    if(online){
      SherpaOnnxOnlineRecognizerConfig c{};c.feat_config.sample_rate=16000;c.feat_config.feature_dim=80;c.model_config.tokens=tokens.c_str();c.model_config.num_threads=threads;c.model_config.provider="cpu";c.decoding_method="greedy_search";c.enable_endpoint=0;
      if(kind=="ctc_online")c.model_config.zipformer2_ctc.model=model.c_str();
      else if(kind=="zip_online") {encoder=dir+"/encoder-epoch-99-avg-1.int8.onnx";decoder=dir+"/decoder-epoch-99-avg-1.onnx";joiner=dir+"/joiner-epoch-99-avg-1.int8.onnx";c.model_config.transducer.encoder=encoder.c_str();c.model_config.transducer.decoder=decoder.c_str();c.model_config.transducer.joiner=joiner.c_str();}
      else {encoder=dir+"/encoder.int8.onnx";decoder=dir+"/decoder.int8.onnx";c.model_config.paraformer.encoder=encoder.c_str();c.model_config.paraformer.decoder=decoder.c_str();}
      on=a.SherpaOnnxCreateOnlineRecognizer(&c);if(!on)throw std::runtime_error("online creation failed");
    } else {
      SherpaOnnxOfflineRecognizerConfig c{};c.feat_config.sample_rate=16000;c.feat_config.feature_dim=80;c.model_config.num_threads=threads;c.model_config.provider="cpu";c.decoding_method="greedy_search";
      if(kind=="ctc_offline") {c.model_config.tokens=tokens.c_str();c.model_config.zipformer_ctc.model=model.c_str();}
      else if(kind=="sense"||kind=="sense_zh") {c.model_config.tokens=tokens.c_str();c.model_config.sense_voice.model=model.c_str();c.model_config.sense_voice.language=kind=="sense_zh"?"zh":"auto";c.model_config.sense_voice.use_itn=1;}
      else if(kind=="qwen"||kind=="qwen_chinese"||kind=="qwen_debug_2048") {frontend=dir+"/conv_frontend.onnx";encoder=dir+"/encoder.int8.onnx";decoder=dir+"/decoder.int8.onnx";tokenizer=dir+"/tokenizer";auto& q=c.model_config.qwen3_asr;q.conv_frontend=frontend.c_str();q.encoder=encoder.c_str();q.decoder=decoder.c_str();q.tokenizer=tokenizer.c_str();q.max_total_len=kind=="qwen_debug_2048"?2048:1024;q.max_new_tokens=512;q.temperature=1e-6f;q.top_p=0.8f;q.seed=42;q.hotwords="";if(kind=="qwen_debug_2048")c.model_config.debug=1;}
      else throw std::runtime_error("unknown model kind");
      off=a.SherpaOnnxCreateOfflineRecognizer(&c);if(!off)throw std::runtime_error("offline creation failed");
    }
    out<<"{\"type\":\"load\",\"kind\":"<<Q(kind)<<",\"suite\":"<<Q(suite)<<",\"threads\":"<<threads<<",\"run\":"<<run<<",\"load_ms\":"<<Ms(load)<<",\"baseline_working_set_bytes\":"<<baseline.working;WriteMem(out,Mem());out<<"}\n";out.flush();
    for(const auto& sample:corpus){
      const auto* wave=a.SherpaOnnxReadWave(sample.path.c_str());if(!wave)throw std::runtime_error("invalid wave "+sample.path);
      const double duration=double(wave->num_samples)/wave->sample_rate;
      std::string text,unflushed;double first_audio_ms=-1,first_compute_ms=-1,flush_ms=0;int updates=0;const auto start=Clock::now();
      if(online){
        const auto* s=a.SherpaOnnxCreateOnlineStream(on);if(!s)throw std::runtime_error("online stream failed");
        const int chunk=std::max(1,wave->sample_rate/50);
        for(int pos=0;pos<wave->num_samples;pos+=chunk){int n=std::min(chunk,wave->num_samples-pos);a.SherpaOnnxOnlineStreamAcceptWaveform(s,wave->sample_rate,wave->samples+pos,n);while(a.SherpaOnnxIsOnlineStreamReady(on,s))a.SherpaOnnxDecodeOnlineStream(on,s);const auto* r=a.SherpaOnnxGetOnlineStreamResult(on,s);std::string next=r&&r->text?r->text:"";if(!next.empty()&&first_audio_ms<0){first_audio_ms=double(pos+n)*1000/wave->sample_rate;first_compute_ms=Ms(start);}if(next!=text){++updates;text=next;}if(r)a.SherpaOnnxDestroyOnlineRecognizerResult(r);}
        unflushed=text;const auto flush=Clock::now();std::vector<float> tail(wave->sample_rate*3/10,0);a.SherpaOnnxOnlineStreamAcceptWaveform(s,wave->sample_rate,tail.data(),static_cast<int32_t>(tail.size()));a.SherpaOnnxOnlineStreamInputFinished(s);if(kind=="para_online_final")a.SherpaOnnxOnlineStreamSetOption(s,"is_final","1");while(a.SherpaOnnxIsOnlineStreamReady(on,s))a.SherpaOnnxDecodeOnlineStream(on,s);const auto* r=a.SherpaOnnxGetOnlineStreamResult(on,s);text=r&&r->text?r->text:"";if(r)a.SherpaOnnxDestroyOnlineRecognizerResult(r);flush_ms=Ms(flush);a.SherpaOnnxDestroyOnlineStream(s);
      }else{
        const auto* s=a.SherpaOnnxCreateOfflineStream(off);if(!s)throw std::runtime_error("offline stream failed");if(kind=="qwen_chinese")a.SherpaOnnxOfflineStreamSetOption(s,"language","Chinese");a.SherpaOnnxAcceptWaveformOffline(s,wave->sample_rate,wave->samples,wave->num_samples);a.SherpaOnnxDecodeOfflineStream(off,s);const auto* r=a.SherpaOnnxGetOfflineStreamResult(s);text=r&&r->text?r->text:"";if(r)a.SherpaOnnxDestroyOfflineRecognizerResult(r);a.SherpaOnnxDestroyOfflineStream(s);
      }
      const double elapsed=Ms(start);out<<"{\"type\":\"sample\",\"kind\":"<<Q(kind)<<",\"suite\":"<<Q(suite)<<",\"threads\":"<<threads<<",\"run\":"<<run<<",\"id\":"<<Q(sample.id)<<",\"duration_s\":"<<duration<<",\"sample_rate\":"<<wave->sample_rate<<",\"elapsed_ms\":"<<elapsed<<",\"rtf\":"<<(elapsed/1000/duration)<<",\"text\":"<<Q(text);
      if(online)out<<",\"unflushed\":"<<Q(unflushed)<<",\"flush_ms\":"<<flush_ms<<",\"first_partial_audio_ms\":"<<first_audio_ms<<",\"first_partial_compute_ms\":"<<first_compute_ms<<",\"updates\":"<<updates;
      WriteMem(out,Mem());out<<"}\n";out.flush();std::cout<<kind<<" run="<<run<<" "<<sample.id<<" "<<int(elapsed)<<"ms "<<text<<"\n";std::cout.flush();a.SherpaOnnxFreeWave(wave);
    }
    if(on)a.SherpaOnnxDestroyOnlineRecognizer(on);
    if(off)a.SherpaOnnxDestroyOfflineRecognizer(off);
    return 0;
  }catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<"\n";return 1;}
}

// Windows command lines are Unicode; convert paths to the C API's UTF-8 convention.
int wmain(int argc, wchar_t** wideArgv) {
  std::vector<std::string> values;
  for (int i = 0; i < argc; ++i) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return 2;
    std::string value(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wideArgv[i], -1, value.data(), size, nullptr, nullptr);
    value.pop_back();
    values.push_back(std::move(value));
  }
  std::vector<char*> argv;
  for (auto& value : values) argv.push_back(value.data());
  return Run(argc, argv.data());
}
