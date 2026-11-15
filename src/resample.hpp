#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Keep one instance for the entire capture. Flush emits the filter's short
// lookahead tail; Reset starts a fresh stream.
class Resampler16k {
 public:
  explicit Resampler16k(int source_rate);
  std::vector<std::int16_t> Process(const std::int16_t* samples, std::size_t count);
  std::vector<std::int16_t> Flush();
  void Reset();
 private:
  static constexpr int kHalf = 32;
  int rate_, phase_step_;
  std::uint64_t total_ = 0, next_ = 0, base_ = 0;
  std::int16_t first_ = 0, last_ = 0;
  bool flushed_ = false;
  std::vector<std::int16_t> buffer_;
  std::vector<std::array<double, kHalf * 2>> kernels_;
  std::vector<std::int16_t> Generate(bool final);
};
std::vector<std::int16_t> ResampleTo16k(const std::int16_t* samples, std::size_t count, int source_rate);
