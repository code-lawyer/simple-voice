#pragma once
#include "model_settings.hpp"
#include <atomic>
#include <string>

// Uses only the packaged, pinned catalog. No arbitrary command or URL is accepted.
bool DownloadModel(const std::wstring& program_directory,const std::wstring& data_directory,
                   SpeechModel model,const std::atomic<bool>& cancelled,std::wstring& message);
std::wstring ModelDownloadStatus(const std::wstring& data_directory);
