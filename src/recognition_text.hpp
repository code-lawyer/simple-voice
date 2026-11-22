#pragma once
#include <string>

bool HasUnknownSpeech(const std::string& text);
bool SameShortRecognition(const std::string& first, const std::string& second);
// Punctuation is allowed to add separators, never replace letters/numbers.
std::string PreserveRecognizedContent(const std::string& raw, const std::string& formatted);
