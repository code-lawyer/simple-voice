#include "sherpa_recognizer.hpp"

#include "recognition_text.hpp"
#include "audio_segments.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <condition_variable>
#include <deque>
#include <stdexcept>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if VOICEIME_HAS_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace {

void Missing(const std::string& path) { std::fprintf(stderr, "missing models: %s\n", path.c_str()); }

#if VOICEIME_HAS_SHERPA

template <typename T>
T LoadFn(HMODULE module, const char* name) {
  return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

struct SherpaApi {
  HMODULE module = nullptr;
  const SherpaOnnxOnlineRecognizer* (*create_online)(const SherpaOnnxOnlineRecognizerConfig*) = nullptr;
  void (*destroy_online)(const SherpaOnnxOnlineRecognizer*) = nullptr;
  const SherpaOnnxOnlineStream* (*create_online_stream)(const SherpaOnnxOnlineRecognizer*) = nullptr;
  void (*destroy_online_stream)(const SherpaOnnxOnlineStream*) = nullptr;
  void (*accept_online)(const SherpaOnnxOnlineStream*, int32_t, const float*, int32_t) = nullptr;
  int32_t (*online_ready)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*) = nullptr;
  void (*decode_online)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*) = nullptr;
  const SherpaOnnxOnlineRecognizerResult* (*online_result)(const SherpaOnnxOnlineRecognizer*,
                                                           const SherpaOnnxOnlineStream*) = nullptr;
  void (*destroy_online_result)(const SherpaOnnxOnlineRecognizerResult*) = nullptr;
  void (*reset_online)(const SherpaOnnxOnlineRecognizer*, const SherpaOnnxOnlineStream*) = nullptr;
  const SherpaOnnxOfflineRecognizer* (*create_offline)(const SherpaOnnxOfflineRecognizerConfig*) = nullptr;
  void (*destroy_offline)(const SherpaOnnxOfflineRecognizer*) = nullptr;
  const SherpaOnnxOfflineStream* (*create_offline_stream)(const SherpaOnnxOfflineRecognizer*) = nullptr;
  void (*destroy_offline_stream)(const SherpaOnnxOfflineStream*) = nullptr;
  void (*accept_offline)(const SherpaOnnxOfflineStream*, int32_t, const float*, int32_t) = nullptr;
  void (*decode_offline)(const SherpaOnnxOfflineRecognizer*, const SherpaOnnxOfflineStream*) = nullptr;
  const SherpaOnnxOfflineRecognizerResult* (*offline_result)(const SherpaOnnxOfflineStream*) = nullptr;
  void (*destroy_offline_result)(const SherpaOnnxOfflineRecognizerResult*) = nullptr;
  const SherpaOnnxOfflinePunctuation* (*create_punct)(const SherpaOnnxOfflinePunctuationConfig*) = nullptr;
  void (*destroy_punct)(const SherpaOnnxOfflinePunctuation*) = nullptr;
  const char* (*add_punct)(const SherpaOnnxOfflinePunctuation*, const char*) = nullptr;
  void (*free_punct)(const char*) = nullptr;
};

HMODULE LoadSherpaDll(const std::wstring& path) {
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
  wchar_t full[MAX_PATH];
  const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, full, nullptr);
  if (length == 0 || length >= MAX_PATH) return nullptr;
  // System32 ships an older onnxruntime.dll. Load from the DLL's own directory
  // so v2.0.1 gets the runtime that was packaged with it.
  return LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool Bind(SherpaApi& api) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring exe_dir(exe);
  const auto slash = exe_dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) exe_dir.resize(slash);
  const std::wstring sdk =
      L"third_party\\sherpa-onnx\\sherpa-onnx-v2.0.1-win-x64-shared-MD-Release\\lib\\sherpa-onnx-c-api.dll";
  const std::wstring candidates[] = {
      exe_dir + L"\\sherpa-onnx-c-api.dll",
      sdk,
      exe_dir + L"\\..\\" + sdk,
  };
  for (const std::wstring& path : candidates) {
    api.module = LoadSherpaDll(path);
    if (api.module != nullptr) break;
  }
  if (api.module == nullptr) {
    Missing("sherpa-onnx-c-api.dll");
    return false;
  }
#define BIND(field, name)                                          \
  api.field = LoadFn<decltype(api.field)>(api.module, name);      \
  if (api.field == nullptr) {                                      \
    Missing(name);                                                 \
    return false;                                                  \
  }
  BIND(create_online, "SherpaOnnxCreateOnlineRecognizer");
  BIND(destroy_online, "SherpaOnnxDestroyOnlineRecognizer");
  BIND(create_online_stream, "SherpaOnnxCreateOnlineStream");
  BIND(destroy_online_stream, "SherpaOnnxDestroyOnlineStream");
  BIND(accept_online, "SherpaOnnxOnlineStreamAcceptWaveform");
  BIND(online_ready, "SherpaOnnxIsOnlineStreamReady");
  BIND(decode_online, "SherpaOnnxDecodeOnlineStream");
  BIND(online_result, "SherpaOnnxGetOnlineStreamResult");
  BIND(destroy_online_result, "SherpaOnnxDestroyOnlineRecognizerResult");
  BIND(reset_online, "SherpaOnnxOnlineStreamReset");
  BIND(create_offline, "SherpaOnnxCreateOfflineRecognizer");
  BIND(destroy_offline, "SherpaOnnxDestroyOfflineRecognizer");
  BIND(create_offline_stream, "SherpaOnnxCreateOfflineStream");
  BIND(destroy_offline_stream, "SherpaOnnxDestroyOfflineStream");
  BIND(accept_offline, "SherpaOnnxAcceptWaveformOffline");
  BIND(decode_offline, "SherpaOnnxDecodeOfflineStream");
  BIND(offline_result, "SherpaOnnxGetOfflineStreamResult");
  BIND(destroy_offline_result, "SherpaOnnxDestroyOfflineRecognizerResult");
  BIND(create_punct, "SherpaOnnxCreateOfflinePunctuation");
  BIND(destroy_punct, "SherpaOnnxDestroyOfflinePunctuation");
  BIND(add_punct, "SherpaOfflinePunctuationAddPunct");
  BIND(free_punct, "SherpaOfflinePunctuationFreeText");
#undef BIND
  return true;
}

std::vector<float> ToFloat(const std::int16_t* samples, std::size_t count) {
  std::vector<float> out(count);
  for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(samples[i]) / 32768.0f;
  return out;
}

template <typename Stream, typename Accept>
void Feed(Accept accept, const Stream* stream, const float* samples, std::size_t count) {
  std::size_t offset = 0;
  while (offset < count) {
    const std::size_t chunk = std::min<std::size_t>(count - offset, static_cast<std::size_t>(16000 * 30));
    accept(stream, 16000, samples + offset, static_cast<int32_t>(chunk));
    offset += chunk;
  }
}

class SherpaRecognizer : public Recognizer {
 public:
  explicit SherpaRecognizer(const ModelFiles& files) {
    streaming_model_=files.stream_encoder;streaming_decoder_=files.stream_decoder;
    streaming_joiner_=files.stream_joiner;streaming_tokens_=files.stream_tokens;
    final_model_=files.final_model;final_tokens_=files.final_tokens;
    final_encoder_=files.final_encoder;final_decoder_=files.final_decoder;
    punct_model_=files.punctuation;hotwords_=files.hotwords;
    if (!Bind(api_)) return;
    SherpaOnnxOnlineRecognizerConfig online{};
    std::memset(&online, 0, sizeof(online));
    online.feat_config.sample_rate = 16000;
    online.feat_config.feature_dim = 80;
    online.model_config.transducer.encoder=streaming_model_.c_str();
    online.model_config.transducer.decoder=streaming_decoder_.c_str();
    online.model_config.transducer.joiner=streaming_joiner_.c_str();
    online.model_config.tokens = streaming_tokens_.c_str();
    online.model_config.num_threads = 2;
    online.model_config.provider = "cpu";
    online.decoding_method = "greedy_search";
    online.enable_endpoint = 0;
    online_ = api_.create_online(&online);
    if (online_ == nullptr) {
      Missing(streaming_model_);
      return;
    }
    online_stream_ = api_.create_online_stream(online_);
    if (online_stream_ == nullptr) {
      Missing(streaming_model_);
      return;
    }

    SherpaOnnxOfflineRecognizerConfig offline{};
    std::memset(&offline, 0, sizeof(offline));
    offline.feat_config.sample_rate = 16000;
    offline.feat_config.feature_dim = 80;
    if(files.model==SpeechModel::ChineseLegacy) {
      offline.model_config.zipformer_ctc.model=final_model_.c_str(); offline.model_config.tokens=final_tokens_.c_str();
    } else if(files.model==SpeechModel::SenseVoice) {
      offline.model_config.sense_voice.model=final_model_.c_str();
      offline.model_config.sense_voice.language="auto";
      offline.model_config.sense_voice.use_itn=1;
      offline.model_config.tokens=final_tokens_.c_str();
    } else if(files.model==SpeechModel::Whisper) {
      auto& w=offline.model_config.whisper;
      w.encoder=final_encoder_.c_str();w.decoder=final_decoder_.c_str();
      w.language="";w.task="transcribe";w.tail_paddings=-1;
      offline.model_config.tokens=final_tokens_.c_str();
    } else {
      auto& q=offline.model_config.qwen3_asr;
      q.conv_frontend=final_model_.c_str(); q.encoder=final_encoder_.c_str(); q.decoder=final_decoder_.c_str();
      q.tokenizer=final_tokens_.c_str(); q.max_total_len=1024; q.max_new_tokens=512;
      q.temperature=1e-6f; q.top_p=0.8f; q.seed=42; q.hotwords=hotwords_.c_str();
    }
    offline.model_config.num_threads = 2;
    offline.model_config.provider = "cpu";
    offline.decoding_method = "greedy_search";
    offline_ = api_.create_offline(&offline);
    if (offline_ == nullptr) {
      Missing(final_model_);
      return;
    }

    // Qwen already returns punctuation. Running a second punctuation model
    // produces duplicated full stops and commas; only CTC needs this stage.
    if(!punct_model_.empty()) {
      SherpaOnnxOfflinePunctuationConfig punct{};
      punct.model.ct_transformer = punct_model_.c_str();
      punct.model.num_threads = 1;
      punct.model.provider = "cpu";
      punct_ = api_.create_punct(&punct);
      if (punct_ == nullptr) { Missing(punct_model_);return; }
    }
    ready_ = true;
    decode_thread_=std::thread([this]{DecodeLoop();});
  }

  ~SherpaRecognizer() override {
    {std::lock_guard<std::mutex> lock(mu_);stopping_=true;chunks_.clear();}
    work_.notify_all();
    if(decode_thread_.joinable())decode_thread_.join();
    if (online_stream_ != nullptr && api_.destroy_online_stream != nullptr) api_.destroy_online_stream(online_stream_);
    if (online_ != nullptr && api_.destroy_online != nullptr) api_.destroy_online(online_);
    if (offline_ != nullptr && api_.destroy_offline != nullptr) api_.destroy_offline(offline_);
    if (punct_ != nullptr && api_.destroy_punct != nullptr) api_.destroy_punct(punct_);
    if (api_.module != nullptr) FreeLibrary(api_.module);
  }

  bool ready() const { return ready_; }

  void AcceptAudio(const std::int16_t* samples, std::size_t count) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_ || finalizing_ || samples == nullptr || count == 0) return;
    if(count>16000*61 || received_+count>16000*61)throw std::runtime_error("录音已达到长度上限。");
    received_+=count;
    samples_.insert(samples_.end(), samples, samples + count);
    const std::vector<float> wave = ToFloat(samples, count);
    Feed(api_.accept_online, online_stream_, wave.data(), wave.size());
    while (api_.online_ready(online_, online_stream_) != 0) api_.decode_online(online_, online_stream_);
    const SherpaOnnxOnlineRecognizerResult* result = api_.online_result(online_, online_stream_);
    if (result != nullptr) {
      if (result->text != nullptr) partial_ = result->text;
      api_.destroy_online_result(result);
    }
    // Commit a cut only after the complete 10–15 s search window exists.
    // The offline worker never holds this lock while running inference.
    while(samples_.size()>15*16000) {
      const auto segment=SegmentAudio(samples_).front();
      Queue(samples_.data(),segment.count,segment.forced);
      samples_.erase(samples_.begin(),samples_.begin()+segment.count);
    }
  }

  std::string Partial() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return partial_;
  }

  std::string Finalize() override {
    std::unique_lock<std::mutex> lock(mu_);
    if (!ready_ || !received_) { warning_="没有收到可识别的音频。请检查麦克风后重试。"; return {}; }
    if(finalizing_)return partial_;
    finalizing_=true;Queue(samples_.data(),samples_.size(),false);samples_.clear();
    work_.wait(lock,[this]{return stopping_ || (!decoding_ && chunks_.empty());});
    if(!voiced_)return {};
    std::string text=decoded_;
    if(forced_) warning_="长段语音缺少停顿，已分段校准。请编辑核对分段处，再确认写入。";
    if(incomplete_) warning_="部分语音没有得到最终文字。请编辑核对，或取消后分段重录。";
    original_=text;
    if (text.empty()) { warning_="最终识别没有得到文字。当前仅保留草稿，请编辑后确认。"; return partial_; }
    const char* punctuated = punct_?api_.add_punct(punct_, text.c_str()):nullptr;
    if (punctuated != nullptr) {
      text = PreserveRecognizedContent(text, punctuated);
      api_.free_punct(punctuated);
    }
    partial_ = text;
    return text;
  }

  void Reset() override {
    std::unique_lock<std::mutex> lock(mu_);
    work_.wait(lock,[this]{return stopping_ || (!decoding_ && chunks_.empty());});
    samples_.clear();
    partial_.clear();
    warning_.clear(); original_.clear();decoded_.clear();received_=0;
    voiced_=false;forced_=false;incomplete_=false;finalizing_=false;
    if (ready_) api_.reset_online(online_, online_stream_);
  }

  std::string Original() const override { std::lock_guard<std::mutex> lock(mu_); return original_; }

  std::string TakeWarning() override {
    std::lock_guard<std::mutex> lock(mu_);
    std::string result; result.swap(warning_); return result;
  }

 private:
  void Queue(const std::int16_t* samples,std::size_t count,bool forced) {
    if(!count || std::none_of(samples,samples+count,[](auto x){return x>8 || x< -8;}))return;
    voiced_=true;forced_|=forced;
    chunks_.emplace_back(samples,samples+count);work_.notify_all();
  }
  void DecodeLoop() {
    for(;;) {
      std::vector<std::int16_t> samples;
      {std::unique_lock<std::mutex> lock(mu_);
        work_.wait(lock,[this]{return stopping_ || !chunks_.empty();});
        if(stopping_)return;
        samples=std::move(chunks_.front());chunks_.pop_front();decoding_=true;
      }
      std::string text;
      try {
        const auto wave=ToFloat(samples.data(),samples.size());
        const auto* stream=api_.create_offline_stream(offline_);
        if(stream) {
          Feed(api_.accept_offline,stream,wave.data(),wave.size());api_.decode_offline(offline_,stream);
          const auto* result=api_.offline_result(stream);
          if(result && result->text)text=result->text;
          if(result)api_.destroy_offline_result(result);
          api_.destroy_offline_stream(stream);
        }
      } catch(...) {}
      {std::lock_guard<std::mutex> lock(mu_);
        if(text.empty())incomplete_=true;
        else {
          if(!decoded_.empty() && static_cast<unsigned char>(decoded_.back())<128 &&
             static_cast<unsigned char>(text.front())<128 && decoded_.back()!=' ')decoded_+=' ';
          decoded_+=text;
        }
        decoding_=false;
      }
      work_.notify_all();
    }
  }
  std::string streaming_model_;
  std::string streaming_tokens_;
  std::string final_model_;
  std::string final_tokens_;
  std::string punct_model_;
  std::string streaming_decoder_,streaming_joiner_,final_encoder_,final_decoder_,original_,hotwords_;
  SherpaApi api_{};
  const SherpaOnnxOnlineRecognizer* online_ = nullptr;
  const SherpaOnnxOnlineStream* online_stream_ = nullptr;
  const SherpaOnnxOfflineRecognizer* offline_ = nullptr;
  const SherpaOnnxOfflinePunctuation* punct_ = nullptr;
  bool ready_ = false;
  mutable std::mutex mu_;
  std::vector<std::int16_t> samples_;
  std::string partial_;
  std::string warning_;
  std::condition_variable work_;
  std::deque<std::vector<std::int16_t>> chunks_;
  std::thread decode_thread_;
  std::string decoded_;
  size_t received_=0;
  bool stopping_=false,decoding_=false,finalizing_=false,voiced_=false,forced_=false,incomplete_=false;
};

#endif

}  // namespace

std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, bool chinese_only) {
  ModelSettings settings;if(chinese_only)settings.model=SpeechModel::ChineseLegacy;
  return MakeSherpaRecognizer(model_dir,settings);
}
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir,
    const ModelSettings& settings,std::string* error) {
  ModelFiles files;std::string reason;
  if(!ResolveModelFiles(model_dir,settings,files,reason)) {if(error)*error=reason;return nullptr;}
#if VOICEIME_HAS_SHERPA
  auto recognizer=std::make_unique<SherpaRecognizer>(files);
  if(!recognizer->ready()) {if(error)*error="模型未能加载，请检查文件是否匹配此型号。";return nullptr;}
  if(error)error->clear();return recognizer;
#else
  if(error)*error="安装包缺少语音运行时，请修复安装。";
  Missing("sherpa-onnx headers were not present at build time"); return nullptr;
#endif
}
