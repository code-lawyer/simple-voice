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
    received_=true; last_frame_=now;
    double energy=0;
    for(std::size_t i=0;i<count;++i) { const double sample=samples[i]; energy+=sample*sample; }
    const double rms=std::sqrt(energy/count);
    zero_samples_=energy==0 ? zero_samples_+count : 0;
    quiet_samples_=rms<80 ? quiet_samples_+count : 0;
    level_=std::clamp(static_cast<int>(rms/32768.0*400),0,100);
  }
  const char* Error(std::uint64_t now) const {
    if(now-last_frame_>=5000) return received_?"麦克风采样已中断。请检查连接后重试。":"麦克风没有返回采样。请检查访问权限和设备后重试。";
    return nullptr;
  }
  // Live silent samples are a hint, not a device failure or a stop command.
  bool quiet() const { return zero_samples_>=16000*8 || quiet_samples_>=16000*12; }
  bool received() const { return received_; }
  int level() const { return level_; }
 private:
  std::uint64_t last_frame_,zero_samples_=0,quiet_samples_=0;
  bool received_=false;
  int level_=0;
};
