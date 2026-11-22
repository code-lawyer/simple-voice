#pragma once
#include "recognizer.hpp"
#include "model_settings.hpp"
#include <memory>
#include <string>
#include <atomic>

// Calls use private inherited pipes, never the desktop's public TSF pipe.
std::unique_ptr<Recognizer> MakeWorkerRecognizer(
    const std::string& model_root,const ModelSettings& settings,std::string* error=nullptr,
    const std::wstring& worker_executable=L"",unsigned load_timeout_ms=120000,
    const std::atomic<bool>* cancel_loading=nullptr,ModelStartup startup=ModelStartup::Full);
