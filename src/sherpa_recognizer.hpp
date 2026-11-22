#pragma once

#include "recognizer.hpp"
#include "model_settings.hpp"

#include <memory>
#include <functional>
#include <string>

// Only explicitly supplied hints reach a model supporting native hotwords.
// No dictionary performs post-recognition replacement.
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, bool chinese_only=false);
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir,
    const ModelSettings& settings,std::string* error=nullptr,ModelStartup startup=ModelStartup::Full,
    std::function<void(bool)> final_loaded={});
