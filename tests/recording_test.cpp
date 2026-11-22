#include "harness.hpp"
#include "recording.hpp"
#include "capture_buffer.hpp"
#include "live_draft.hpp"
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

void ColdBufferPreservesOrderAndAcceptedPrefix() {
  LiveDraft draft;
  CHECK(draft.Update({L"现在",L"",L""}) && draft.text()==L"现在");
  CHECK(draft.Update({L"",L"现在开始",L""}) && draft.text()==L"现在开始");
  CHECK(!draft.Update({L"现在开",L"",L""}) && draft.text()==L"现在开始");
  CHECK(!draft.Update({L"现在开始",L"现在开始 A",L""}) && draft.text()==L"现在开始");
  CHECK(draft.Update({L"",L"",L"现在开始 A"}) && draft.text()==L"现在开始 A");
  CaptureBuffer buffer;
  std::vector<std::int16_t> published,block;
  const std::int16_t first[]{31,32,33}, second[]{41,42};
  CHECK(buffer.Write(first,3).samples==3);
  CHECK(buffer.Read(block) && block==std::vector<std::int16_t>(first,first+3));
  CHECK(buffer.Write(second,2).samples==2);
  std::vector<std::int16_t> tail(CaptureBuffer::capacity);
  for(std::size_t i=0;i<tail.size();++i)tail[i]=static_cast<std::int16_t>(i%30000);
  const auto result=buffer.Write(tail.data(),tail.size());
  CHECK(result.full && result.samples==CaptureBuffer::capacity-2);
  CHECK(buffer.Write(first,3).samples==0);
  while(buffer.Read(block))published.insert(published.end(),block.begin(),block.end());
  std::vector<std::int16_t> expected(second,second+2);
  expected.insert(expected.end(),tail.begin(),tail.end()-2);
  CHECK(published==expected);
  CaptureBuffer discarded;
  CHECK(discarded.Write(first,3).samples==3);
  discarded.Discard();
  CHECK(discarded.discarded() && !discarded.Read(block));
}
