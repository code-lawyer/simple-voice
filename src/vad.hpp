#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct VadConfig {
  int sample_rate;
  int silence_ms;
  int max_utterance_ms;
  int speech_rms;
};

enum class VadEvent { None, Speech, Endpoint, ForcedEndpoint };

class Vad {
 public:
  explicit Vad(VadConfig config);
  VadEvent Push(const std::int16_t* samples, std::size_t count);
  void Reset();

 private:
  VadConfig config_;
  std::vector<std::int16_t> pending_;
  bool heard_ = false;
  int silence_ms_ = 0;
  int speech_age_ms_ = 0;
};
