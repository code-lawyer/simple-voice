#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum class ModelStartup { Full, DraftFirst };

class Recognizer {
 public:
  virtual ~Recognizer() = default;
  // True when the displayable draft changed.
  virtual bool AcceptAudio(const std::int16_t* samples, std::size_t count) = 0;
  virtual std::string Partial() const = 0;
  virtual std::string Finalize() = 0;
  virtual void Reset() = 0;
  virtual void Cancel() {}
  virtual bool Healthy() const { return true; }
  virtual bool FinalReady() const { return Healthy(); }
  virtual std::size_t PeakMemoryBytes() const { return 0; }
  virtual std::string TakeWarning() { return {}; }
  virtual std::string Original() const { return Partial(); }
};
