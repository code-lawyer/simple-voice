#pragma once
#include <algorithm>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <cmath>

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

// A second decode can guess a word in clicks or fan noise. Require sustained
// energy and a few frames with a repeating vocal pitch before retrying.
inline bool ShortVoicedRegion(const std::vector<std::int16_t>& pcm, std::size_t& first, std::size_t& end) {
  constexpr auto frame = SpeechPause::frame;
  constexpr auto rate = SpeechPause::rate;
  if (pcm.size() > rate*5/2) return false;
  first=pcm.size(); end=0;
  unsigned active=0, voiced=0;
  for (std::size_t pos=0;pos+frame<=pcm.size();pos+=frame) {
    const auto* samples=pcm.data()+pos;
    if (SpeechPause::Quiet(samples,frame)) continue;
    first=std::min(first,pos); end=pos+frame; ++active;
    if (voiced>=2) continue;
    double mean=0;
    for(std::size_t i=0;i<frame;++i) mean+=samples[i];
    mean/=frame;
    for (std::size_t lag=32;lag<=160;lag+=2) {
      double a=0,b=0,cross=0;
      for (std::size_t i=lag;i<frame;i+=2) {
        const auto x=samples[i]-mean,y=samples[i-lag]-mean;
        a+=x*x; b+=y*y; cross+=x*y;
      }
      if (a>0 && b>0 && cross>0 && cross*cross>0.45*a*b) { ++voiced; break; }
    }
  }
  return active>=3 && voiced>=2 && end-first<=rate;
}
// Each sample belongs to exactly one segment. Short bounded segments avoid
// long-form ASR omissions; cuts prefer the quietest 20 ms in the last 5 s.
inline std::vector<AudioSegment> SegmentAudio(const std::vector<std::int16_t>& samples) {
  constexpr std::size_t rate=16000,limit=15*rate,window=rate/50,step=rate/100;
  std::vector<AudioSegment> result;
  for(std::size_t start=0;start<samples.size();) {
    std::size_t end=std::min(start+limit,samples.size()); bool forced=false;
    if(end<samples.size()) {
      // At the duration cap prefer the middle of an actual pause to a
      // single low-energy frame inside a digit, acronym or name.
      std::size_t quiet=0,longest=0,center=0;
      for(std::size_t pos=start+10*rate;pos+window<=end;pos+=window) {
        quiet=SpeechPause::Quiet(samples.data()+pos,window)?quiet+window:0;
        if(quiet>=rate*12/100 && quiet>=longest) { longest=quiet; center=pos+window-quiet/2; }
      }
      if(center) {
        result.push_back({start,center-start,false}); start=center; continue;
      }
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
