#pragma once
#include "lexicon.hpp"
#include <string>

// Portable explicit terms and input preferences only. Rime's live learning
// database and model directories are deliberately outside this format.
std::wstring CreateDataBackup(Lexicon& lexicon,const std::wstring& data,const std::wstring& parent);
void RestoreDataBackup(Lexicon& lexicon,const std::wstring& data,const std::wstring& backup);
// Completes rollback after an interrupted restore before settings are loaded.
bool RecoverDataRestore(Lexicon& lexicon,const std::wstring& data);
