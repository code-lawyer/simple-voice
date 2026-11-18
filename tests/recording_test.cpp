#include "harness.hpp"
#include "recording.hpp"
#include "resample.hpp"
#include "session.hpp"
#include <limits>
#include <vector>

void LongRecordingStopsOnlyAtTheSharedLimit() {
  RecordingBudget budget;
  Session session;session.Hotkey();session.OnDraft("未校准的长段草稿");
  for(int second=0;second<kMaxRecordingSeconds;++second) {
    CHECK(budget.Take(kRecordingSampleRate)==kRecordingSampleRate);
    if(budget.full())session.OnCaptureLimit();
    else CHECK(session.phase()==Phase::Listening && session.mic_open());
  }
  CHECK(budget.full() && budget.samples()==kMaxRecordingSamples);
  CHECK(session.phase()==Phase::Stopped && !session.mic_open());
  CHECK(session.box_text()=="未校准的长段草稿");
  std::string text;CHECK(!session.TakeCommit(&text));
  CHECK(budget.Take(320)==0);
}

void RecordingLimitClipsPacketsAndRejectsOverflow() {
  RecordingBudget budget;
  CHECK(budget.Take(kMaxRecordingSamples-17)==kMaxRecordingSamples-17);
  CHECK(budget.Take(320)==17);
  CHECK(budget.samples()==kMaxRecordingSamples);
  CHECK(FitsRecording(0,16000*62));
  CHECK(FitsRecording(kMaxRecordingSamples-17,17));
  CHECK(!FitsRecording(kMaxRecordingSamples-17,18));
  CHECK(!FitsRecording(kMaxRecordingSamples,1));
  CHECK(!FitsRecording(1,std::numeric_limits<std::size_t>::max()));
  CHECK(!FitsRecording(std::numeric_limits<std::size_t>::max(),1));
  RecordingBudget restarted;CHECK(restarted.Take(320)==320 && !restarted.full());
}

void Resample48kTo16kKeepsDuration() {
  std::vector<std::int16_t> input(48000,0);
  CHECK(ResampleTo16k(input.data(),input.size(),48000).size()==16000);
}
