#include "recognition_text.hpp"
#include "utf8.hpp"
#include <cwctype>
#include <vector>

bool HasUnknownSpeech(const std::string& text) {
  return text.find("<unk>")!=std::string::npos || text.find("< unk >")!=std::string::npos;
}
namespace {
std::wstring Content(const std::string& text) {
  std::wstring result;
  const std::wstring punctuation=L"，。！？；：、,.!?;:\"“”‘’（）()[]【】…—";
  for(wchar_t ch:Utf8ToWide(text)) {
    if(iswspace(ch) || punctuation.find(ch)!=std::wstring::npos) continue;
    result.push_back(ch);
  }
  return result;
}
std::vector<std::string> AsciiWords(const std::string& text) {
  std::vector<std::string> result; std::string current;
  for(unsigned char ch:text) {
    if((ch>='a'&&ch<='z') || (ch>='A'&&ch<='Z') || (ch>='0'&&ch<='9') || ch=='_') current+=char(ch);
    else if(!current.empty()) { result.push_back(current); current.clear(); }
  }
  if(!current.empty()) result.push_back(current);
  return result;
}
}
std::string PreserveRecognizedContent(const std::string& raw,const std::string& formatted) {
  if(formatted.empty() || HasUnknownSpeech(raw) || Content(raw)!=Content(formatted) || AsciiWords(raw)!=AsciiWords(formatted)) return raw;
  // Numeric punctuation is semantically significant (1.5, 1,000, 10:30).
  // Keep a raw numeric run intact, even if a formatter changes its separators.
  std::string run;
  auto flush=[&] { const bool ok=run.empty() || formatted.find(run)!=std::string::npos; run.clear(); return ok; };
  for(unsigned char ch:raw) {
    if((ch>='0'&&ch<='9') || (!run.empty() && (ch=='.'||ch==','||ch==':'||ch=='/'||ch=='-'||ch=='%'))) run+=char(ch);
    else if(!flush()) return raw;
  }
  if(!flush()) return raw;
  // A formatter must not split identifiers, insert spaces in English words, or
  // remove apostrophes/hyphens inside them. Case is already checked above.
  for(unsigned char ch:raw) {
    if((ch>='a'&&ch<='z') || (ch>='A'&&ch<='Z') || (ch>='0'&&ch<='9') ||
       (!run.empty() && (ch=='_'||ch=='\''||ch=='-'||ch=='.'||ch=='/'||ch==':'))) run+=char(ch);
    else if(!flush()) return raw;
  }
  if(!flush()) return raw;
  return formatted;
}
