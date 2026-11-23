#include "recognition_text.hpp"
#include "utf8.hpp"
#include <cwctype>
#include <string_view>
#include <vector>

bool HasUnknownSpeech(const std::string& text) {
  return text.find("<unk>")!=std::string::npos || text.find("< unk >")!=std::string::npos;
}
namespace {
std::wstring Content(const std::wstring& text) {
  std::wstring result;
  const std::wstring punctuation=L"，。！？；：、,.!?;:\"“”‘’（）()[]【】…—";
  for(wchar_t ch:text) {
    if(iswspace(ch) || punctuation.find(ch)!=std::wstring::npos) continue;
    result.push_back(ch);
  }
  return result;
}
bool Digit(wchar_t ch) { return ch>=L'0' && ch<=L'9'; }
bool AsciiWord(wchar_t ch) {
  return Digit(ch) || (ch>=L'A' && ch<=L'Z') || (ch>=L'a' && ch<=L'z') || ch==L'_';
}
// Preserve whole identifiers and numbers in order. Punctuation between digits
// belongs to the number, while an ordinary sentence-ending period does not.
std::vector<std::wstring> ProtectedWords(const std::wstring& text) {
  std::vector<std::wstring> words;
  for(size_t i=0;i<text.size();) {
    const bool leading_decimal=(text[i]==L'.' || text[i]==L'．') && i+1<text.size() && Digit(text[i+1]);
    if(!AsciiWord(text[i]) && !leading_decimal) { ++i; continue; }
    const auto first=i++;
    while(i<text.size()) {
      if(AsciiWord(text[i])) { ++i; continue; }
      if(i+1>=text.size() || !AsciiWord(text[i+1])) break;
      const wchar_t ch=text[i];
      const bool numeric=Digit(text[i-1]) && Digit(text[i+1]);
      if(std::wstring_view(L".:'-/").find(ch)==std::wstring_view::npos &&
         !(numeric && std::wstring_view(L",，．：").find(ch)!=std::wstring_view::npos)) break;
      ++i;
    }
    words.emplace_back(text.substr(first,i-first));
  }
  return words;
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
  if(formatted.empty() || HasUnknownSpeech(raw)) return raw;
  const auto before=Utf8ToWide(raw),after=Utf8ToWide(formatted);
  if(Content(before)!=Content(after) || ProtectedWords(before)!=ProtectedWords(after)) return raw;
  return formatted;
}
