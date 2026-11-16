#pragma once

#include "recognizer.hpp"

#include <memory>
#include <string>

// ASR formatting preserves recognized content. Personal dictionaries are never
// passed to the recognizer or used for post-recognition replacement.
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, bool chinese_only=false);
