#include "vad.hpp"

#include <cmath>

Vad::Vad(VadConfig config) : config_(config) {}

void Vad::Reset() {
  pending_.clear();
  heard_ = false;
  silence_ms_ = 0;
  elapsed_ms_ = 0;
}

VadEvent Vad::Push(const std::int16_t* samples, std::size_t count) {
  if (samples != nullptr && count > 0) {
    pending_.insert(pending_.end(), samples, samples + count);
  }
  const int frame = config_.sample_rate * 20 / 1000;
  if (frame <= 0) return VadEvent::None;

  bool saw_speech = false;
  bool endpoint = false;
  bool forced = false;
  std::size_t used = 0;
  while (pending_.size() - used >= static_cast<std::size_t>(frame)) {
    double energy = 0;
    for (int i = 0; i < frame; ++i) {
      const double sample = pending_[used + static_cast<std::size_t>(i)];
      energy += sample * sample;
    }
    used += static_cast<std::size_t>(frame);
    const double rms = std::sqrt(energy / frame);
    const bool speech = rms >= config_.speech_rms;
    if (speech) {
      heard_ = true;
      silence_ms_ = 0;
      saw_speech = true;
    }
    elapsed_ms_ += 20;
    if (elapsed_ms_ >= config_.max_utterance_ms) forced = true;
    if (!heard_) continue;
    if (!speech) silence_ms_ += 20;
    if (silence_ms_ >= config_.silence_ms) endpoint = true;
  }
  if (used > 0) pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
  if (forced) return VadEvent::ForcedEndpoint;
  if (endpoint) return VadEvent::Endpoint;
  if (saw_speech) return VadEvent::Speech;
  return VadEvent::None;
}
