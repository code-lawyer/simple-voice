#pragma once

#include "recording.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

// One utterance, with a single capture writer and one decoder. Closing keeps
// the accepted prefix for finalization. Cancelling wakes the decoder and drops it.
class CaptureBuffer {
 public:
  static constexpr std::size_t capacity = 12 * kRecordingSampleRate;
  struct WriteResult { std::size_t samples; bool full; };

  WriteResult Write(const std::int16_t* samples, std::size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || !count) return {0, false};
    const auto accepted = std::min(count, capacity - queued_);
    blocks_.emplace_back(samples, samples + accepted);
    queued_ += accepted;
    const bool full = queued_ == capacity;
    if (full) closed_ = true;
    ready_.notify_one();
    return {accepted, full};
  }

  bool Read(std::vector<std::int16_t>& block) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return closed_ || !blocks_.empty(); });
    if (blocks_.empty()) return false;
    block = std::move(blocks_.front());
    blocks_.pop_front();
    queued_ -= block.size();
    return true;
  }

  void Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    ready_.notify_one();
  }

  void Discard() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = discarded_ = true;
    blocks_.clear();
    queued_ = 0;
    ready_.notify_one();
  }

  bool discarded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return discarded_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::vector<std::int16_t>> blocks_;
  std::size_t queued_ = 0;
  bool closed_ = false, discarded_ = false;
};
