#pragma once

#include "recognizer.hpp"

#include <memory>
#include <string>

class Segmenter;
class Lexicon;

// ASR formatting preserves recognized content. Personal dictionaries are never
// used for unconstrained post-recognition replacement. The legacy arguments
// remain source-compatible with isolated diagnostic tools.
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, Segmenter* segmenter,
                                               Lexicon* lexicon, bool chinese_only=false);
