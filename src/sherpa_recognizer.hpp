#pragma once

#include "recognizer.hpp"

#include <memory>
#include <string>

class Segmenter;
class Lexicon;

// model_dir holds streaming/, final/, and punct/. Segmenter and Lexicon are
// borrowed and must outlive the recognizer. Finalize cuts and applies them.
// A null segmenter or lexicon returns the punctuated text unchanged.
std::unique_ptr<Recognizer> MakeSherpaRecognizer(const std::string& model_dir, Segmenter* segmenter,
                                                 Lexicon* lexicon);
