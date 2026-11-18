#pragma once

#include <algorithm>
#include <cstddef>

inline constexpr std::size_t kRecordingSampleRate = 16000;
inline constexpr int kMaxRecordingSeconds = 10 * 60;
inline constexpr std::size_t kMaxRecordingSamples = kRecordingSampleRate * kMaxRecordingSeconds;

// One shared limit for capture and both sides of the model process boundary.
inline bool FitsRecording(std::size_t received, std::size_t count) {
  return received <= kMaxRecordingSamples && count <= kMaxRecordingSamples - received;
}

// Silence and volume never end a recording. The resource limit only stops
// capture; the session still requires an explicit request to finalize.
class RecordingBudget {
 public:
  std::size_t Take(std::size_t count) {
    const auto accepted = std::min(count, kMaxRecordingSamples - samples_);
    samples_ += accepted;
    return accepted;
  }
  bool full() const { return samples_ == kMaxRecordingSamples; }
  std::size_t samples() const { return samples_; }
 private:
  std::size_t samples_ = 0;
};
