#pragma once
#include <array>
#include <string>

// Public choices are curated model packages, not arbitrary architectures.
enum class SpeechModel { Qwen3, SenseVoice, Whisper, ChineseLegacy };
const char* SpeechModelId(SpeechModel model);
const wchar_t* SpeechModelLabel(SpeechModel model);
const wchar_t* SpeechModelDescription(SpeechModel model);
const char* SpeechModelFolder(SpeechModel model);
struct ModelSettings {
  static constexpr int kDefaultIdleMinutes = 5;
  SpeechModel model = SpeechModel::Qwen3;
  std::array<std::wstring,4> directories{};
  int idle_minutes = kDefaultIdleMinutes; // 0 keeps the model warm.
  std::string hotwords; // Explicit hints; only Qwen supports them here.
};
bool ParseModelSettings(const std::string& text, ModelSettings& result);
std::string SerializeModelSettings(const ModelSettings& settings);
ModelSettings LoadModelSettings(const std::wstring& path, bool legacy_chinese=false);
bool SaveModelSettings(const std::wstring& path, const ModelSettings& settings);
bool SameModelRuntime(const ModelSettings& a, const ModelSettings& b);
std::string NormalizeSpeechHints(const std::string& hints);
struct ModelFiles {
  SpeechModel model = SpeechModel::Qwen3;
  std::string stream_encoder, stream_decoder, stream_joiner, stream_tokens;
  std::string final_model, final_encoder, final_decoder, final_tokens, punctuation, hotwords;
};
bool ResolveModelFiles(const std::string& built_in_root, const ModelSettings& settings,
                       ModelFiles& files, std::string& error);
