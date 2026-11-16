#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class Recognizer {
 public:
  virtual ~Recognizer() = default;
  virtual void AcceptAudio(const std::int16_t* samples, std::size_t count) = 0;
  virtual std::string Partial() const = 0;
  virtual std::string Finalize() = 0;
  virtual void Reset() = 0;
  virtual std::string TakeWarning() { return {}; }
  virtual std::string Original() const { return Partial(); }
};
