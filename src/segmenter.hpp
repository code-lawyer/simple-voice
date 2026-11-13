#pragma once

#include "lexicon.hpp"

#include <memory>
#include <string>
#include <vector>

class Segmenter {
 public:
  explicit Segmenter(const std::string& dict_dir);
  ~Segmenter();

  Segmenter(const Segmenter&) = delete;
  Segmenter& operator=(const Segmenter&) = delete;

  std::vector<Token> Cut(const std::string& text) const;
  // Keep a 2–8 code-point word together on the next Cut. Single characters
  // and longer strings are ignored. Replacement still waits for a second commit.
  void Remember(const std::string& text);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
