#include "harness.hpp"
#include "resample.hpp"
#include "vad.hpp"

#include <vector>

void SilenceAfterSpeechEndpointsAt500ms() {
  Vad vad(VadConfig{16000, 500, 60000, 800});
  std::vector<std::int16_t> loud(16000 / 2, 10000);
  CHECK(vad.Push(loud.data(), loud.size()) == VadEvent::Speech);
  std::vector<std::int16_t> quiet(16000 / 2, 0);
  CHECK(vad.Push(quiet.data(), quiet.size()) == VadEvent::Endpoint);
}

void SixtySecondsForcesEndpoint() {
  Vad vad(VadConfig{16000, 500, 60000, 800});
  std::vector<std::int16_t> loud(16000 * 60, 10000);
  CHECK(vad.Push(loud.data(), loud.size()) == VadEvent::ForcedEndpoint);
}

void Resample48kTo16kKeepsDuration() {
  std::vector<std::int16_t> input(48000, 0);
  auto out = ResampleTo16k(input.data(), input.size(), 48000);
  CHECK(out.size() == 16000);
}
