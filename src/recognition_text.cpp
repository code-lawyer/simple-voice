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
bool SameShortRecognition(const std::string& first,const std::string& second) {
  auto key=[](const std::string& text) {
    auto word=Utf8ToWide(text);
    const auto end=word.find_last_not_of(L" \t\r\n。！？.!?");
    if(end==std::wstring::npos) return std::wstring{};
    const auto begin=word.find_first_not_of(L" \t\r\n");
    word=word.substr(begin,end-begin+1);
    for(auto& ch:word) if(ch>=L'A' && ch<=L'Z') ch+=L'a'-L'A';
    static const wchar_t* const digits[]={L"zero",L"one",L"two",L"three",L"four",L"five",L"six",L"seven",L"eight",L"nine"};
    const std::wstring chinese=L"零一二三四五六七八九";
    for(int i=0;i<10;++i)
      if(word==digits[i] || (word.size()==1 && word[0]==chinese[i])) return std::wstring(1,L'0'+i);
    return word;
  };
  if(first.empty() || second.empty() || HasUnknownSpeech(first) || HasUnknownSpeech(second)) return false;
  const auto a=key(first);
  return !a.empty() && a==key(second);
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
