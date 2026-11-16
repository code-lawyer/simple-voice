#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <limits>

struct AudioSegment { std::size_t offset,count; bool forced; };
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
