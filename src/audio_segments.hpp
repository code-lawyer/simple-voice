#pragma once
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <limits>

struct AudioSegment { std::size_t offset,count; bool forced; };
// Positions are relative to the current uncommitted Qwen segment. Analyze each
// complete 20 ms frame once, independent of microphone packet boundaries.
class SpeechPause {
 public:
  static constexpr std::size_t rate=16000,frame=rate/50,tail=rate/5;

  static bool Quiet(const std::int16_t* data,std::size_t count) {
    std::int64_t energy=0;
    for(std::size_t i=0;i<count;++i) {
      const int v=data[i];
      if(v>200 || v< -200) return false;
      energy+=v*v;
    }
    return energy<=static_cast<std::int64_t>(count)*45*45;
  }

  std::size_t Scan(const std::vector<std::int16_t>& pcm,std::size_t confirmed_speech) {
    while(cursor_+frame<=std::min(pcm.size(),15*rate)) {
      if(!Quiet(pcm.data()+cursor_,frame)) {
        // A resumed phrase confirms the earlier pause. Give short phrases and
        // brief hesitations their full joint context until the actual stop.
        if(!pending_cut_ && last_sound_>=6*rate && cursor_-last_sound_>=rate*3/5) {
          pending_cut_=last_sound_+tail;
          resumed_at_=cursor_;
        }
        last_sound_=cursor_+frame;
      }
      cursor_+=frame;
    }
    return pending_cut_ && confirmed_speech>=resumed_at_ ? pending_cut_ : 0;
  }

  std::size_t PendingCut() const { return pending_cut_; }
  std::size_t PreviewEnd() const {
    return !pending_cut_ && last_sound_ && last_sound_+tail>=rate && cursor_>=last_sound_+tail ? last_sound_+tail : 0;
  }
  bool QuietRemainder(const std::vector<std::int16_t>& pcm) const {
    return cursor_==pcm.size() || Quiet(pcm.data()+cursor_,pcm.size()-cursor_);
  }
  bool OnlyQuiet(const std::vector<std::int16_t>& pcm) const { return !last_sound_ && QuietRemainder(pcm); }
 private:
  std::size_t cursor_=0,last_sound_=0,pending_cut_=0,resumed_at_=0;
};
// Each sample belongs to exactly one segment. Short bounded segments avoid
// long-form ASR omissions; cuts prefer the quietest 20 ms in the last 5 s.
inline std::vector<AudioSegment> SegmentAudio(const std::vector<std::int16_t>& samples) {
  constexpr std::size_t rate=16000,limit=15*rate,window=rate/50,step=rate/100;
  std::vector<AudioSegment> result;
  for(std::size_t start=0;start<samples.size();) {
    std::size_t end=std::min(start+limit,samples.size()); bool forced=false;
    if(end<samples.size()) {
      double best=std::numeric_limits<double>::max(); std::size_t cut=end;
      for(std::size_t candidate=start+10*rate;candidate+window<=end;candidate+=step) {
        double energy=0;
        for(std::size_t i=candidate;i<candidate+window;++i) { const double value=samples[i]; energy+=value*value; }
        if(energy<=best) { best=energy; cut=candidate+window/2; }
      }
      end=cut; forced=best/window>80*80;
    }
    result.push_back({start,end-start,forced}); start=end;
  }
  return result;
}
