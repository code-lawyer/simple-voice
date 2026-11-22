#include "sherpa_recognizer.hpp"
#include "recording.hpp"

#include "recognition_text.hpp"
#include "audio_segments.hpp"
#include "live_draft.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <condition_variable>
#include <deque>
#include <filesystem>
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

void ToFloat(const std::int16_t* samples, std::size_t count,std::vector<float>& out) {
  out.resize(count);
  for (std::size_t i = 0; i < count; ++i) out[i] = static_cast<float>(samples[i]) / 32768.0f;
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
  struct OfflineJob {
    std::vector<std::int16_t> pcm;
    std::string text;
    std::size_t padding=0;
    bool ready=false, retry_short=false, recovered=false;
  };
  using Job=std::shared_ptr<OfflineJob>;
 public:
  SherpaRecognizer(const ModelFiles& files,std::function<void(bool)> final_loaded)
      : model_(files.model), qwen_(files.model == SpeechModel::Qwen3) {
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
    online.model_config.model_type = "zipformer";
    // Staggered streams share these sessions. A second intra-op thread costs
    // more CPU between small encoder calls and competes with background Qwen.
    online.model_config.num_threads = qwen_ ? 1 : 2;
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
    ready_=true;
    decode_thread_=std::thread([this,notify=std::move(final_loaded)] {
      bool ok=false;
      try { ok=LoadFinalModel(); } catch(...) {}
      if(notify) notify(ok);
      {
        std::lock_guard<std::mutex> lock(mu_);
        final_ready_=ok;
        final_load_done_=true;
      }
      work_.notify_all();
      if(ok) DecodeLoop();
    });
  }

 private:
  bool LoadFinalModel() {
    std::string provider="cpu";
    if(qwen_) {
      wchar_t executable[32768];
      const auto length=GetModuleFileNameW(nullptr,executable,32768);
      if(length && length<32768) {
        const auto config=std::filesystem::path(executable).parent_path()/L"qwen-cpu.conf";
        std::error_code ec;
        if(std::filesystem::is_regular_file(config,ec)) {
          // The worker runs in its own directory. An ASCII config path also
          // works for installations under a non-ASCII Windows user name.
          provider+=std::filesystem::equivalent(config.filename(),config,ec)?
              ":qwen-cpu.conf":":"+WideToUtf8(config.wstring());
        }
      }
    }
    SherpaOnnxOfflineRecognizerConfig offline{};
    std::memset(&offline, 0, sizeof(offline));
    offline.feat_config.sample_rate = 16000;
    offline.feat_config.feature_dim = 80;
    if(model_==SpeechModel::ChineseLegacy) {
      offline.model_config.zipformer_ctc.model=final_model_.c_str(); offline.model_config.tokens=final_tokens_.c_str();
    } else if(model_==SpeechModel::SenseVoice) {
      offline.model_config.sense_voice.model=final_model_.c_str();
      offline.model_config.sense_voice.language="auto";
      offline.model_config.sense_voice.use_itn=1;
      offline.model_config.tokens=final_tokens_.c_str();
    } else if(model_==SpeechModel::Whisper) {
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
    offline.model_config.num_threads = qwen_ && std::thread::hardware_concurrency() >= 8 ? 4 : 2;
    offline.model_config.provider = provider.c_str();
    offline.decoding_method = "greedy_search";
    offline_ = api_.create_offline(&offline);
    if (offline_ == nullptr) {
      Missing(final_model_);
      return false;
    }

    // Qwen already returns punctuation. Running a second punctuation model
    // produces duplicated full stops and commas; only CTC needs this stage.
    if(!punct_model_.empty()) {
      SherpaOnnxOfflinePunctuationConfig punct{};
      punct.model.ct_transformer = punct_model_.c_str();
      punct.model.num_threads = 1;
      punct.model.provider = "cpu";
      punct_ = api_.create_punct(&punct);
      if (punct_ == nullptr) { Missing(punct_model_);return false; }
    }
    return true;
  }

 public:
  ~SherpaRecognizer() override {
    {std::lock_guard<std::mutex> lock(mu_);stopping_=true;chunks_.clear();}
    work_.notify_all();
    if(decode_thread_.joinable())decode_thread_.join();
    DropDraftStreams();
    if (online_stream_ != nullptr && api_.destroy_online_stream != nullptr) api_.destroy_online_stream(online_stream_);
    if (online_ != nullptr && api_.destroy_online != nullptr) api_.destroy_online(online_);
    if (offline_ != nullptr && api_.destroy_offline != nullptr) api_.destroy_offline(offline_);
    if (punct_ != nullptr && api_.destroy_punct != nullptr) api_.destroy_punct(punct_);
    if (api_.module != nullptr) FreeLibrary(api_.module);
  }

  bool ready() const { return ready_; }
  bool WaitFinalLoad() {
    std::unique_lock<std::mutex> lock(mu_);
    work_.wait(lock,[this]{return final_load_done_;});
    return final_ready_;
  }
  bool FinalReady() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return final_ready_;
  }

  bool AcceptAudio(const std::int16_t* samples, std::size_t count) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_ || finalizing_ || samples == nullptr || count == 0) return false;
    if(!FitsRecording(received_,count))throw std::runtime_error("录音已达到 10 分钟上限。");
    if(qwen_ && !received_) {
      static constexpr float lead[kRecordingSampleRate*240/1000]{};
      for(std::size_t i=0;i<draft_streams_.size();++i) {
        draft_streams_[i]=api_.create_online_stream(online_);
        if(draft_streams_[i])Feed(api_.accept_online,draft_streams_[i],lead,(i+1)*kRecordingSampleRate*120/1000);
      }
    }
    received_+=count;
    samples_.insert(samples_.end(), samples, samples + count);
    ToFloat(samples,count,stream_wave_);
    Feed(api_.accept_online, online_stream_, stream_wave_.data(), stream_wave_.size());
    std::string next=ReadDraft(online_stream_,true);
    if(qwen_) {
      std::array<std::wstring,LiveDraft::streams> updates{};
      updates[0]=Utf8ToWide(next);
      for(std::size_t i=0;i<draft_streams_.size();++i) {
        if(!draft_streams_[i])continue;
        Feed(api_.accept_online,draft_streams_[i],stream_wave_.data(),stream_wave_.size());
        updates[i+1]=Utf8ToWide(ReadDraft(draft_streams_[i]));
      }
      next=live_draft_.Update(updates)?WideToUtf8(live_draft_.text()):std::string{};
    }
    bool changed=false;
    if(!next.empty() && partial_!=next) {partial_=std::move(next);changed=true;}
    PrepareSegments();
    return changed;
  }

  std::string Partial() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return partial_;
  }

  std::string Finalize() override {
    std::unique_lock<std::mutex> lock(mu_);
    DropDraftStreams();
    if (!ready_ || !received_) { warning_="没有收到可识别的音频。请检查麦克风后重试。"; return {}; }
    if(finalizing_)return partial_;
    finalizing_=true;
    if(pause_.PendingCut())DropCandidate();
    const auto preview_end=qwen_?pause_.PreviewEnd():0;
    if(!candidate_ && preview_end && pause_.QuietRemainder(samples_)) {
      candidate_=Schedule(preview_end,false,true);
      candidate_end_=preview_end;
    }
    if(candidate_ && pause_.QuietRemainder(samples_)) {
      // The microphone has sent only quiet frames since this exact boundary.
      // Keep the original PCM until Qwen actually returns a usable result.
      work_.wait(lock,[this]{return stopping_ || candidate_->ready || (final_load_done_ && !final_ready_);});
      if(candidate_->ready && !candidate_->text.empty()) {
        committed_.push_back(candidate_);
        samples_.clear();
      }
    }
    DropCandidate();
    if(!samples_.empty() && !(qwen_ && !committed_.empty() && pause_.OnlyQuiet(samples_)))
      Commit(samples_.size(),received_<kRecordingSampleRate && qwen_);
    samples_.clear();
    work_.wait(lock,[this]{return stopping_ || (final_load_done_ && (!final_ready_ || (!decoding_ && chunks_.empty())));});
    if(!final_ready_) throw std::runtime_error("定稿模型加载失败，已保留当前草稿。");
    if(committed_.empty())return {};
    std::string text;
    bool incomplete=false;
    for(const auto& job:committed_) {
      if(job->recovered)warning_="录音较短，首次识别为空，请核对重试得到的文字。";
      if(job->text.empty()) {incomplete=true;continue;}
      if(!text.empty() && static_cast<unsigned char>(text.back())<128 &&
         static_cast<unsigned char>(job->text.front())<128 && text.back()!=' ')text+=' ';
      text+=job->text;
    }
    if(forced_) warning_="长段语音缺少停顿，已分段校准。请编辑核对分段处，再确认写入。";
    if(incomplete) warning_="部分语音没有得到最终文字。请编辑核对，或取消后分段重录。";
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
    DropCandidate();
    work_.wait(lock,[this]{return stopping_ || (final_load_done_ && !final_ready_) || (!decoding_ && chunks_.empty());});
    if(final_load_done_ && !final_ready_) throw std::runtime_error("定稿模型加载失败，请重新准备。");
    // Sherpa's endpoint reset carries acoustic and decoder context into the
    // next segment. A new hotkey recording is an independent utterance.
    DropDraftStreams();
    live_draft_={};
    if (ready_ && received_) {
      const auto* next = api_.create_online_stream(online_);
      if (!next) throw std::runtime_error("无法创建新的语音草稿，请重新准备模型。");
      api_.destroy_online_stream(online_stream_);
      online_stream_ = next;
    }
    samples_.clear();
    partial_.clear();
    warning_.clear(); original_.clear();committed_.clear();received_=0;
    pause_={};confirmed_speech_=0;forced_=false;finalizing_=false;
  }

  std::string Original() const override { std::lock_guard<std::mutex> lock(mu_); return original_; }

  std::string TakeWarning() override {
    std::lock_guard<std::mutex> lock(mu_);
    std::string result; result.swap(warning_); return result;
  }

 private:
  std::string DecodeSegment(const std::vector<float>& wave) {
    std::unique_ptr<const SherpaOnnxOfflineStream, decltype(api_.destroy_offline_stream)>
        stream(api_.create_offline_stream(offline_), api_.destroy_offline_stream);
    if (!stream) return {};
    Feed(api_.accept_offline, stream.get(), wave.data(), wave.size());
    api_.decode_offline(offline_, stream.get());
    std::unique_ptr<const SherpaOnnxOfflineRecognizerResult, decltype(api_.destroy_offline_result)>
        result(api_.offline_result(stream.get()), api_.destroy_offline_result);
    return result && result->text ? result->text : "";
  }

  Job Schedule(std::size_t count,bool retry_short=false,bool at_pause=false) {
    auto job=std::make_shared<OfflineJob>();
    job->pcm.assign(samples_.begin(),samples_.begin()+count);
    // Qwen punctuation and number formatting depend on trailing context.
    // These zeros belong only to the offline candidate, never to capture or draft.
    if(at_pause) job->padding=count<2*kRecordingSampleRate ? kRecordingSampleRate*55/100 : SpeechPause::tail;
    job->retry_short=retry_short;
    chunks_.push_back(job);work_.notify_all();
    return job;
  }
  void DropCandidate() {
    // A running job owns its copy and its result. Removing the session's
    // reference makes late results harmless if speech resumes or is cancelled.
    if(candidate_ && !chunks_.empty() && chunks_.back()==candidate_) chunks_.pop_back();
    candidate_.reset();candidate_end_=0;
  }
  void Commit(std::size_t count,bool retry_short=false,bool at_pause=false) {
    if(candidate_ && candidate_end_==count) {
      committed_.push_back(std::move(candidate_));
      candidate_end_=0;
      return;
    }
    DropCandidate();
    if(std::any_of(samples_.begin(),samples_.begin()+count,[](auto x){return x>8 || x< -8;}))
      committed_.push_back(Schedule(count,retry_short,at_pause));
  }
  void PrepareSegments() {
    for(;;) {
      const auto offset=received_-samples_.size();
      auto cut=qwen_?pause_.Scan(samples_,confirmed_speech_>offset?confirmed_speech_-offset:0):0;
      bool at_pause=cut!=0;
      if(!cut && samples_.size()>15*kRecordingSampleRate) {
        cut=qwen_?pause_.PreviewEnd():0;
        at_pause=cut!=0;
        if(!cut) {
          const auto part=SegmentAudio(samples_).front();
          cut=part.count;forced_|=part.forced;
        }
      }
      if(!cut)break;
      Commit(cut,false,at_pause);
      samples_.erase(samples_.begin(),samples_.begin()+cut);
      pause_={};
    }
    if(!qwen_)return;
    // A cough or chair noise can end acoustic silence. Retain the full PCM
    // until two actual draft tokens fall on the far side of the pause.
    if(const auto pending=pause_.PendingCut()) {
      if(candidate_ && candidate_end_!=pending)DropCandidate();
      return;
    }
    const auto end=pause_.PreviewEnd();
    if(candidate_ && candidate_end_!=end)DropCandidate();
    if(end && !candidate_ && final_ready_ && !decoding_ && chunks_.empty()) {
      candidate_=Schedule(end,false,true);
      candidate_end_=end;
    }
  }
  void DecodeLoop() {
    std::vector<float> wave;
    for(;;) {
      std::vector<std::int16_t> samples;
      Job job;
      {std::unique_lock<std::mutex> lock(mu_);
        work_.wait(lock,[this]{return stopping_ || !chunks_.empty();});
        if(stopping_)return;
        job=std::move(chunks_.front());chunks_.pop_front();decoding_=true;
        samples=std::move(job->pcm);
      }
      std::string text;
      bool recovered_short_word = false;
      try {
        ToFloat(samples.data(),samples.size(),wave);
        std::vector<std::int16_t>().swap(samples);
        wave.resize(wave.size()+job->padding,0.0f);
        text = DecodeSegment(wave);
        // A very short word can end at the feature boundary. Retry an empty
        // Qwen result once with 160 ms of trailing silence, retaining all audio.
        if (text.empty() && job->retry_short) {
          constexpr auto tail_samples = kRecordingSampleRate * 160 / 1000;
          wave.resize(wave.size() + tail_samples, 0.0f);
          text = DecodeSegment(wave);
          recovered_short_word = !text.empty();
        }
      } catch(...) {}
      {std::lock_guard<std::mutex> lock(mu_);
        job->text=std::move(text);job->recovered=recovered_short_word;job->ready=true;
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
  std::string ReadDraft(const SherpaOnnxOnlineStream* stream,bool primary=false) {
    if(!api_.online_ready(online_,stream))return {};
    do {api_.decode_online(online_,stream);} while(api_.online_ready(online_,stream));
    const auto* result=api_.online_result(online_,stream);
    if(!result)return {};
    if(primary && result->timestamps && result->count>=2) {
      const auto seconds=result->timestamps[result->count-2];
      if(seconds>=0 && seconds<=kMaxRecordingSeconds)
        confirmed_speech_=static_cast<std::size_t>(seconds*kRecordingSampleRate);
    }
    std::string text=result->text?result->text:"";
    api_.destroy_online_result(result);
    return text;
  }
  void DropDraftStreams() {
    for(auto& stream:draft_streams_) {
      if(stream)api_.destroy_online_stream(stream);
      stream=nullptr;
    }
  }
  const SherpaOnnxOnlineStream* online_stream_ = nullptr;
  std::array<const SherpaOnnxOnlineStream*,LiveDraft::streams-1> draft_streams_{};
  LiveDraft live_draft_;
  const SherpaOnnxOfflineRecognizer* offline_ = nullptr;
  const SherpaOnnxOfflinePunctuation* punct_ = nullptr;
  bool ready_ = false;
  bool final_load_done_=false,final_ready_=false;
  const SpeechModel model_;
  const bool qwen_;
  mutable std::mutex mu_;
  std::vector<std::int16_t> samples_;
  std::vector<float> stream_wave_;
  std::string partial_;
  std::string warning_;
  std::condition_variable work_;
  std::deque<Job> chunks_;
  std::vector<Job> committed_;
  Job candidate_;
  SpeechPause pause_;
  std::size_t candidate_end_=0,confirmed_speech_=0;
  std::thread decode_thread_;
  size_t received_=0;
  bool stopping_=false,decoding_=false,finalizing_=false,forced_=false;
};

#endif

}  // namespace

std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, bool chinese_only) {
  ModelSettings settings;if(chinese_only)settings.model=SpeechModel::ChineseLegacy;
  return MakeSherpaRecognizer(model_dir,settings);
}
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir,
    const ModelSettings& settings,std::string* error,ModelStartup startup,std::function<void(bool)> final_loaded) {
  ModelFiles files;std::string reason;
  if(!ResolveModelFiles(model_dir,settings,files,reason)) {if(error)*error=reason;return nullptr;}
#if VOICEIME_HAS_SHERPA
  auto recognizer=std::make_unique<SherpaRecognizer>(files,std::move(final_loaded));
  if(!recognizer->ready() || (startup==ModelStartup::Full && !recognizer->WaitFinalLoad())) {
    if(error)*error="模型未能加载，请检查文件是否匹配此型号。";return nullptr;
  }
  if(error)error->clear();return recognizer;
#else
  if(error)*error="安装包缺少语音运行时，请修复安装。";
  Missing("sherpa-onnx headers were not present at build time"); return nullptr;
#endif
}
