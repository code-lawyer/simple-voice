#include "sherpa_recognizer.hpp"

#include "lexicon.hpp"
#include "segmenter.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#if VOICEIME_HAS_SHERPA
#include "sherpa-onnx/c-api/c-api.h"
#endif

namespace {

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  if (last == '/' || last == '\\') return dir + name;
  return dir + "\\" + name;
}

bool Exists(const std::string& path) {
  return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string FindOnnx(const std::string& dir) {
  const std::string preferred = JoinPath(dir, "model.int8.onnx");
  if (Exists(preferred)) return preferred;
  const std::string pattern = JoinPath(dir, "*.onnx");
  WIN32_FIND_DATAA data{};
  HANDLE found = FindFirstFileA(pattern.c_str(), &data);
  std::string int8;
  std::string any;
  if (found != INVALID_HANDLE_VALUE) {
    do {
      const std::string path = JoinPath(dir, data.cFileName);
      if (any.empty()) any = path;
      if (int8.empty() && std::string(data.cFileName).find("int8") != std::string::npos) int8 = path;
    } while (FindNextFileA(found, &data));
    FindClose(found);
  }
  if (!int8.empty()) return int8;
  return any;
}

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
  SherpaRecognizer(std::string streaming_model, std::string streaming_tokens, std::string final_model,
                   std::string final_tokens, std::string punct_model, Segmenter* segmenter, Lexicon* lexicon)
      : streaming_model_(std::move(streaming_model)),
        streaming_tokens_(std::move(streaming_tokens)),
        final_model_(std::move(final_model)),
        final_tokens_(std::move(final_tokens)),
        punct_model_(std::move(punct_model)),
        segmenter_(segmenter),
        lexicon_(lexicon) {
    if (!Bind(api_)) return;
    SherpaOnnxOnlineRecognizerConfig online{};
    std::memset(&online, 0, sizeof(online));
    online.feat_config.sample_rate = 16000;
    online.feat_config.feature_dim = 80;
    online.model_config.zipformer2_ctc.model = streaming_model_.c_str();
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
    offline.model_config.zipformer_ctc.model = final_model_.c_str();
    offline.model_config.tokens = final_tokens_.c_str();
    offline.model_config.num_threads = 2;
    offline.model_config.provider = "cpu";
    offline.decoding_method = "greedy_search";
    offline_ = api_.create_offline(&offline);
    if (offline_ == nullptr) {
      Missing(final_model_);
      return;
    }

    SherpaOnnxOfflinePunctuationConfig punct{};
    std::memset(&punct, 0, sizeof(punct));
    punct.model.ct_transformer = punct_model_.c_str();
    punct.model.num_threads = 1;
    punct.model.provider = "cpu";
    punct_ = api_.create_punct(&punct);
    if (punct_ == nullptr) {
      Missing(punct_model_);
      return;
    }
    ready_ = true;
  }

  ~SherpaRecognizer() override {
    if (online_stream_ != nullptr && api_.destroy_online_stream != nullptr) api_.destroy_online_stream(online_stream_);
    if (online_ != nullptr && api_.destroy_online != nullptr) api_.destroy_online(online_);
    if (offline_ != nullptr && api_.destroy_offline != nullptr) api_.destroy_offline(offline_);
    if (punct_ != nullptr && api_.destroy_punct != nullptr) api_.destroy_punct(punct_);
    if (api_.module != nullptr) FreeLibrary(api_.module);
  }

  bool ready() const { return ready_; }

  void AcceptAudio(const std::int16_t* samples, std::size_t count) override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_ || samples == nullptr || count == 0) return;
    samples_.insert(samples_.end(), samples, samples + count);
    const std::vector<float> wave = ToFloat(samples, count);
    Feed(api_.accept_online, online_stream_, wave.data(), wave.size());
    while (api_.online_ready(online_, online_stream_) != 0) api_.decode_online(online_, online_stream_);
    const SherpaOnnxOnlineRecognizerResult* result = api_.online_result(online_, online_stream_);
    if (result != nullptr) {
      if (result->text != nullptr) partial_ = result->text;
      api_.destroy_online_result(result);
    }
  }

  std::string Partial() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return partial_;
  }

  std::string Finalize() override {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_ || samples_.empty()) return partial_;
    const std::vector<float> wave = ToFloat(samples_.data(), samples_.size());
    const SherpaOnnxOfflineStream* stream = api_.create_offline_stream(offline_);
    if (stream == nullptr) return partial_;
    Feed(api_.accept_offline, stream, wave.data(), wave.size());
    api_.decode_offline(offline_, stream);
    const SherpaOnnxOfflineRecognizerResult* result = api_.offline_result(stream);
    std::string text;
    if (result != nullptr) {
      if (result->text != nullptr) text = result->text;
      api_.destroy_offline_result(result);
    }
    api_.destroy_offline_stream(stream);
    if (text.empty()) return partial_;
    const char* punctuated = api_.add_punct(punct_, text.c_str());
    if (punctuated != nullptr) {
      text = punctuated;
      api_.free_punct(punctuated);
    }
    if (segmenter_ != nullptr && lexicon_ != nullptr) {
      const std::vector<Token> tokens = segmenter_->Cut(text);
      const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
      text = lexicon_->Apply(text, tokens, now);
    }
    partial_ = text;
    return text;
  }

  void Reset() override {
    std::lock_guard<std::mutex> lock(mu_);
    samples_.clear();
    partial_.clear();
    if (ready_) api_.reset_online(online_, online_stream_);
  }

 private:
  std::string streaming_model_;
  std::string streaming_tokens_;
  std::string final_model_;
  std::string final_tokens_;
  std::string punct_model_;
  Segmenter* segmenter_ = nullptr;
  Lexicon* lexicon_ = nullptr;
  SherpaApi api_{};
  const SherpaOnnxOnlineRecognizer* online_ = nullptr;
  const SherpaOnnxOnlineStream* online_stream_ = nullptr;
  const SherpaOnnxOfflineRecognizer* offline_ = nullptr;
  const SherpaOnnxOfflinePunctuation* punct_ = nullptr;
  bool ready_ = false;
  mutable std::mutex mu_;
  std::vector<std::int16_t> samples_;
  std::string partial_;
};

#endif

}  // namespace

std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, Segmenter* segmenter, Lexicon* lexicon) {
  const std::string streaming = JoinPath(model_dir, "streaming");
  const std::string final_dir = JoinPath(model_dir, "final");
  const std::string punct_dir = JoinPath(model_dir, "punct");
  for (const std::string& dir : {streaming, final_dir, punct_dir}) {
    if (!Exists(dir)) {
      Missing(dir);
      return nullptr;
    }
  }
  const std::string streaming_model = FindOnnx(streaming);
  const std::string streaming_tokens = JoinPath(streaming, "tokens.txt");
  const std::string final_model = FindOnnx(final_dir);
  const std::string final_tokens = JoinPath(final_dir, "tokens.txt");
  const std::string punct_model = FindOnnx(punct_dir);
  for (const std::string& path : {streaming_model, streaming_tokens, final_model, final_tokens, punct_model}) {
    if (path.empty() || !Exists(path)) {
      Missing(path.empty() ? model_dir : path);
      return nullptr;
    }
  }
#if VOICEIME_HAS_SHERPA
  auto recognizer = std::make_unique<SherpaRecognizer>(streaming_model, streaming_tokens, final_model, final_tokens,
                                                       punct_model, segmenter, lexicon);
  if (!recognizer->ready()) return nullptr;
  return recognizer;
#else
  (void)segmenter;
  (void)lexicon;
  Missing("sherpa-onnx headers were not present at build time");
  return nullptr;
#endif
}
