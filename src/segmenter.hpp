#pragma once

#include <memory>
#include <string>
#include <vector>

struct Token { std::string text; std::string pinyin; };

class Segmenter {
 public:
  explicit Segmenter(const std::string& dict_dir);
  ~Segmenter();

  Segmenter(const Segmenter&) = delete;
  Segmenter& operator=(const Segmenter&) = delete;

  std::vector<Token> Cut(const std::string& text) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
