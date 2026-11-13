#include "resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

std::vector<std::int16_t> ResampleTo16k(const std::int16_t* samples, std::size_t count, int source_rate) {
  if (samples == nullptr || count == 0 || source_rate <= 0) return {};
  const std::size_t out_count = static_cast<std::size_t>(static_cast<std::uint64_t>(count) * 16000 / static_cast<std::uint64_t>(source_rate));
  std::vector<std::int16_t> out(out_count);
  if (out_count == 0) return out;
  for (std::size_t i = 0; i < out_count; ++i) {
    const double position = static_cast<double>(i) * static_cast<double>(source_rate) / 16000.0;
    const std::size_t left = std::min(static_cast<std::size_t>(position), count - 1);
    const std::size_t right = std::min(left + 1, count - 1);
    const double fraction = position - static_cast<double>(left);
    double mixed = static_cast<double>(samples[left]) * (1.0 - fraction) + static_cast<double>(samples[right]) * fraction;
    if (mixed > 32767.0) mixed = 32767.0;
    if (mixed < -32768.0) mixed = -32768.0;
    out[i] = static_cast<std::int16_t>(std::lround(mixed));
  }
  return out;
}
