#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <string_view>

// Each stream sees exactly the same audio on a different encoder clock. The
// ordinary stream owns the transcript. The others can expose a short suffix
// early when their recent text still matches the displayed phrase.
class LiveDraft {
 public:
  static constexpr std::size_t streams=3;
  bool Update(const std::array<std::wstring,streams>& updates) {
    std::array<bool,streams> changed{};
    for(std::size_t i=0;i<streams;++i) {
      changed[i]=!updates[i].empty() && updates[i]!=latest_[i];
      if(changed[i])latest_[i]=updates[i];
    }
    if(std::none_of(changed.begin(),changed.end(),[](bool value){return value;}))return false;
    const auto previous=shown_;
    const auto& main=latest_[0];
    if(!Starts(shown_,main)) {
      const auto common=std::mismatch(main.begin(),main.end(),shown_.begin(),shown_.end()).first-main.begin();
      // A single disagreement cannot flash a speculative word back and forth.
      if(Starts(main,shown_) || main.size()>shown_.size()+3 ||
         Starts(latest_[1],main.substr(0,common+1)) || Starts(latest_[2],main.substr(0,common+1)))
        shown_=main;
    }
    for(std::size_t i=1;i<streams;++i) {
      if(!changed[i])continue;
      auto proposal=Extend(main,latest_[i]);
      if(!Starts(proposal,shown_) && !main.empty())proposal=Extend(shown_,latest_[i]);
      if(proposal.size()<=shown_.size() || !Starts(proposal,shown_) || proposal.size()<=main.size())continue;
      const auto suffix=proposal.substr(main.size());
      if(!ShortSuffix(suffix))continue;
      // Letter fragments and digits are ambiguous: wait for another stream
      // to confirm them before extending an English term or a number.
      if(std::any_of(suffix.begin(),suffix.end(),[](wchar_t ch){return ch>32 && ch<127;}) &&
         !Starts(Extend(main,latest_[streams-i]),proposal))continue;
      shown_=std::move(proposal);
    }
    return shown_!=previous;
  }
  const std::wstring& text() const {return shown_;}

 private:
  static bool Starts(const std::wstring& text,const std::wstring& prefix) {
    return text.compare(0,prefix.size(),prefix)==0;
  }
  static std::wstring Extend(const std::wstring& anchor,const std::wstring& source) {
    if(Starts(source,anchor))return source;
    constexpr std::size_t context=3;
    if(anchor.size()<context)return anchor;
    const auto tail=std::wstring_view(source).substr(source.size()>16?source.size()-16:0);
    const auto pos=tail.rfind(std::wstring_view(anchor).substr(anchor.size()-context));
    if(pos==std::wstring_view::npos)return anchor;
    return anchor+std::wstring(tail.substr(pos+context));
  }
  static bool ShortSuffix(const std::wstring& text) {
    if(text.size()>15)return false;
    int units=0;
    bool latin=false;
    for(const auto ch:text) {
      const bool word=(ch>=L'A' && ch<=L'Z') || (ch>=L'a' && ch<=L'z') || (ch>=L'0' && ch<=L'9');
      if((word && !latin) || (!word && ch>L' '))++units;
      latin=word;
    }
    return units<=2;
  }
  std::array<std::wstring,streams> latest_{};
  std::wstring shown_;
};
