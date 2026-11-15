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

void InitialSilenceAlsoStopsAtSixtySeconds() {
  Vad vad(VadConfig{16000, 500, 60000, 800});
  std::vector<std::int16_t> frame(320, 0);
  for (int i = 0; i < 2999; ++i) CHECK(vad.Push(frame.data(), frame.size()) == VadEvent::None);
  CHECK(vad.Push(frame.data(), frame.size()) == VadEvent::ForcedEndpoint);
}

void InitialSilenceCountsTowardSpeechLimitAndResetClearsIt() {
  Vad vad(VadConfig{16000, 500, 60000, 800});
  std::vector<std::int16_t> silent(16000 * 59, 0);
  std::vector<std::int16_t> speech(16000, 2000);
  CHECK(vad.Push(silent.data(), silent.size()) == VadEvent::None);
  CHECK(vad.Push(speech.data(), speech.size()) == VadEvent::ForcedEndpoint);
  vad.Reset();
  CHECK(vad.Push(speech.data(), speech.size()) == VadEvent::Speech);
  std::vector<std::int16_t> pause(8000, 0);
  CHECK(vad.Push(pause.data(), pause.size()) == VadEvent::Endpoint);
}

void Resample48kTo16kKeepsDuration() {
  std::vector<std::int16_t> input(48000, 0);
  auto out = ResampleTo16k(input.data(), input.size(), 48000);
  CHECK(out.size() == 16000);
}
