#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>

// Audio arrival and signal are separate: a stream of zeroes is still a live
// device, but it must never be presented as successfully recognized speech.
class CaptureHealth {
 public:
  explicit CaptureHealth(std::uint64_t started):last_frame_(started) {}
  void Push(const std::int16_t* samples,std::size_t count,std::uint64_t now) {
    if(!samples || !count) return;
    received_=true; last_frame_=now; samples_+=count;
    double energy=0;
    for(std::size_t i=0;i<count;++i) { const double sample=samples[i]; energy+=sample*sample; if(samples[i]!=0) nonzero_=true; }
    const double rms=std::sqrt(energy/count);
    signal_=signal_ || rms>=80;
    level_=std::clamp(static_cast<int>(rms/32768.0*400),0,100);
  }
  const char* Error(std::uint64_t now) const {
    if(now-last_frame_>=5000) return received_?"麦克风采样已中断。请检查连接后重试。":"麦克风没有返回采样。请检查访问权限和设备后重试。";
    return nullptr;
  }
  // Live silent samples are a hint, not a device failure or a stop command.
  bool quiet() const { return (samples_>=16000*8 && !nonzero_) || (samples_>=16000*12 && !signal_); }
  bool received() const { return received_; }
  int level() const { return level_; }
 private:
  std::uint64_t last_frame_,samples_=0;
  bool received_=false,nonzero_=false,signal_=false;
  int level_=0;
};
