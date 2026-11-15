#include "resample.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

Resampler16k::Resampler16k(int rate) : rate_(rate), phase_step_(1) {
  if (rate <= 0 || rate > 384000) throw std::invalid_argument("unsupported sample rate");
  phase_step_ = std::gcd(rate, 16000);
  if (rate == 16000) return;
  constexpr double pi = 3.14159265358979323846;
  const double cutoff = 0.94 * std::min(1.0, 16000.0 / rate);
  kernels_.resize(16000 / phase_step_);
  for (std::size_t phase = 0; phase < kernels_.size(); ++phase) {
    const double fraction = static_cast<double>(phase * phase_step_) / 16000;
    double sum = 0;
    for (int tap = 0; tap < 2 * kHalf; ++tap) {
      const double x = tap - kHalf + 1 - fraction;
      const double sinc = std::abs(x) < 1e-12 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
      const double weight = sinc * (0.5 + 0.5 * std::cos(pi * x / kHalf));
      kernels_[phase][tap] = weight; sum += weight;
    }
    for (auto& weight : kernels_[phase]) weight /= sum;
  }
}
void Resampler16k::Reset() {
  total_ = next_ = base_ = 0; first_ = last_ = 0; flushed_ = false; buffer_.clear();
}
std::vector<std::int16_t> Resampler16k::Process(const std::int16_t* samples, std::size_t count) {
  if (!samples || !count) return {};
  if (flushed_) throw std::logic_error("resampler must be reset after flush");
  if (rate_ == 16000) { total_ += count; return {samples, samples + count}; }
  if (total_ == 0) first_ = samples[0];
  last_ = samples[count - 1]; total_ += count;
  buffer_.insert(buffer_.end(), samples, samples + count);
  return Generate(false);
}
std::vector<std::int16_t> Resampler16k::Flush() {
  if (flushed_) return {};
  flushed_ = true;
  return rate_ == 16000 ? std::vector<std::int16_t>{} : Generate(true);
}
std::vector<std::int16_t> Resampler16k::Generate(bool final) {
  std::vector<std::int16_t> result;
  const auto target = total_ * 16000 / static_cast<std::uint64_t>(rate_);
  while (next_ < target) {
    const auto numerator = next_ * static_cast<std::uint64_t>(rate_);
    const auto center = static_cast<std::int64_t>(numerator / 16000);
    if (!final && center + kHalf >= static_cast<std::int64_t>(total_)) break;
    const auto& weights = kernels_[(numerator % 16000) / phase_step_];
    double value = 0;
    for (int tap = 0; tap < 2 * kHalf; ++tap) {
      const auto index = center + tap - kHalf + 1;
      const auto sample = index < 0 ? first_ : index >= static_cast<std::int64_t>(total_) ? last_ :
          buffer_[static_cast<std::size_t>(index - static_cast<std::int64_t>(base_))];
      value += sample * weights[tap];
    }
    result.push_back(static_cast<std::int16_t>(std::lround(std::clamp(value, -32768.0, 32767.0))));
    ++next_;
  }
  const auto oldest = std::max<std::int64_t>(0,
      static_cast<std::int64_t>(next_ * rate_ / 16000) - kHalf + 1);
  const auto remove = std::min<std::uint64_t>(buffer_.size(), static_cast<std::uint64_t>(oldest) - base_);
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(remove)); base_ += remove;
  return result;
}
std::vector<std::int16_t> ResampleTo16k(const std::int16_t* samples, std::size_t count, int rate) {
  if (!samples || !count || rate <= 0 || rate > 384000) return {};
  Resampler16k resampler(rate);
  auto output = resampler.Process(samples, count);
  const auto tail = resampler.Flush();
  output.insert(output.end(), tail.begin(), tail.end());
  return output;
}
