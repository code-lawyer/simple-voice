#include "review_collector.hpp"
#include "utf8.hpp"
#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <string_view>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <windows.h>

namespace {
constexpr std::size_t kMaxQueuedInputs=512;
constexpr std::size_t kMaxQueuedBytes=2*1024*1024;
struct Piece { std::wstring text,key; std::size_t start,end; int reason=0; };
bool Han(wchar_t c) { return c>=0x3400 && c<=0x9fff; }
bool Latin(wchar_t c) { return (c>=L'a'&&c<=L'z') || (c>=L'A'&&c<=L'Z'); }
bool Digit(wchar_t c) { return c>=L'0'&&c<=L'9'; }
bool WordChar(wchar_t c) { return Han(c)||Latin(c)||Digit(c)||c==L'+'||c==L'#'||c==L'-'||c==L'_'; }
bool Number(const std::wstring& word) {
  return std::all_of(word.begin(),word.end(),[](wchar_t ch){
    return Digit(ch)||std::wstring_view(L"零〇一二三四五六七八九十百千万亿两点").find(ch)!=std::wstring_view::npos;
  });
}
std::wstring ComparisonText(const std::wstring& text) {
  std::wstring result;
  for(auto ch:text) {
    if(!WordChar(ch)) continue;
    const auto digit=std::wstring_view(L"零一二三四五六七八九").find(ch);
    if(digit!=std::wstring_view::npos) result+=wchar_t(L'0'+digit);
    else if(ch==L'〇') result+=L'0';
    else result+=ch>=L'A' && ch<=L'Z'?wchar_t(ch+32):ch;
  }
  return result;
}
std::vector<Piece> Pieces(const Segmenter& segmenter,const std::wstring& text) {
  std::vector<Piece> result;
  std::size_t end=0;
  for(const auto& token:segmenter.Cut(WideToUtf8(text))) {
    auto word=Utf8ToWide(token.text);
    const auto pos=text.find(word,end);
    if(pos==text.npos || word.empty()) continue;
    end=pos+word.size();
    if(word.size()>40 || !std::all_of(word.begin(),word.end(),WordChar) ||
       !std::any_of(word.begin(),word.end(),[](auto c){return Han(c)||Latin(c);})) continue;
    auto key=word;
    for(auto& c:key) if(c>=L'A'&&c<=L'Z') c+=L'a'-L'A';
    result.push_back({std::move(word),std::move(key),pos,end});
  }
  return result;
}
// Align the two word sequences. Only a localized replacement with observable
// text on both sides is evidence of a different spelling. A draft often lacks
// the entire last clause, which by itself says nothing about individual words.
void MarkChanges(std::vector<Piece>& written,const std::vector<Piece>& before,int reason) {
  if(before.empty() || written.empty()) return;
  auto first_word=written.begin(),last_word=written.end();
  auto first_old=before.begin(),last_old=before.end();
  while(first_word!=last_word && first_old!=last_old && first_word->key==first_old->key) {
    first_word->reason|=first_old->reason;
    ++first_word; ++first_old;
  }
  while(first_word!=last_word && first_old!=last_old && (last_word-1)->key==(last_old-1)->key) {
    --last_word; --last_old;
    last_word->reason|=last_old->reason;
  }
  const std::size_t new_count=last_word-first_word,old_count=last_old-first_old;
  if(!new_count) return;
  // A ten-minute utterance may contain hundreds of unchanged words around a single name.
  // Only the unmatched interval needs a quadratic alignment.
  if(new_count>400 || old_count>400) {
    std::map<std::wstring,int> agreed_reasons;
    for(auto old=first_old;old!=last_old;++old) {
      auto [entry,inserted]=agreed_reasons.emplace(old->key,old->reason);
      if(!inserted) entry->second&=old->reason;
    }
    for(auto word=first_word;word!=last_word;++word) {
      const auto entry=agreed_reasons.find(word->key);
      if(entry!=agreed_reasons.end()) word->reason|=entry->second;
      else if(reason==UserEdited && !Number(word->text)) word->reason|=reason;
    }
    return;
  }
  const auto cols=old_count+1;
  std::vector<unsigned short> lcs((new_count+1)*cols,0);
  for(std::size_t i=new_count;i-->0;) for(std::size_t j=old_count;j-->0;) {
    auto& score=lcs[i*cols+j];
    score=first_word[i].key==first_old[j].key?1+lcs[(i+1)*cols+j+1]:
        std::max(lcs[(i+1)*cols+j],lcs[i*cols+j+1]);
  }
  // Unrelated multiword drafts give no evidence about which particular name
  // was unstable. A one- or two-piece spelling can be compared on its own.
  const bool has_context=first_word!=written.begin() || last_word!=written.end();
  if(reason==DraftDiffers && !has_context && !lcs.front() && (new_count>2 || old_count>2)) return;
  std::size_t i=0,j=0,first=0,old=0;
  auto mark=[&] {
    if(i==first || (reason==DraftDiffers && (j==old || i-first>4 || j-old>4))) return;
    int flags=reason;
    if(reason==UserEdited && i-first<=4 && j-old<=4)
      for(auto k=old;k<j;++k) flags|=first_old[k].reason;
    for(auto k=first;k<i;++k) if(!Number(first_word[k].text)) first_word[k].reason|=flags;
  };
  while(i<new_count && j<old_count) {
    if(first_word[i].key==first_old[j].key) {
      mark(); first_word[i].reason|=first_old[j].reason; first=++i; old=++j;
    }
    else if(lcs[(i+1)*cols+j]>=lcs[i*cols+j+1]) ++i;
    else ++j;
  }
  i=new_count; j=old_count; mark();
}
std::string Example(const std::wstring& text,std::size_t start,std::size_t end) {
  auto left=start>6?start-6:0;
  auto right=std::min(text.size(),end+6);
  if(left && text[left]>=0xdc00 && text[left]<=0xdfff) --left;
  if(right<text.size() && text[right]>=0xdc00 && text[right]<=0xdfff) ++right;
  auto sample=text.substr(left,right-left);
  for(auto& c:sample) if(c<L' ') c=L' ';
  return WideToUtf8(sample);
}
}

std::vector<ReviewWord> ExtractReviewWords(const Segmenter& segmenter,WordSource source,
    const std::string& written,const std::string& recognized,const std::string& draft,bool short_retry) {
  const auto text=Utf8ToWide(written);
  auto pieces=Pieces(segmenter,text);
  if(source==WordSource::Voice && !recognized.empty()) {
    const auto original=Utf8ToWide(recognized);
    const bool same=ComparisonText(original)==ComparisonText(text);
    auto recognized_pieces=same?pieces:Pieces(segmenter,original);
    if(short_retry) for(auto& p:recognized_pieces) p.reason|=ShortRecognitionRetried;
    if(!draft.empty() && ComparisonText(Utf8ToWide(draft))!=ComparisonText(original))
      MarkChanges(recognized_pieces,Pieces(segmenter,Utf8ToWide(draft)),DraftDiffers);
    if(same) pieces=std::move(recognized_pieces);
    else MarkChanges(pieces,recognized_pieces,UserEdited);
  }
  std::map<std::wstring,ReviewWord> words;
  std::set<std::pair<std::size_t,std::size_t>> occurrences;
  auto add=[&](std::size_t start,std::size_t end,int reason) {
    const auto key=text.substr(start,end-start);
    auto& word=words[key]; word.text=WideToUtf8(key);
    if(occurrences.emplace(start,end).second) ++word.count;
    if(word.example.empty() || reason) word.example=Example(text,start,end);
    word.reasons|=reason;
  };
  for(const auto& p:pieces) add(p.start,p.end,p.reason);
  // Keep a complete technical name when the dictionary tokenizes its
  // punctuation separately (for example GPT-4o or C++).
  std::size_t piece=0;
  for(std::size_t start=0;start<text.size();) {
    if(!Latin(text[start]) && !Digit(text[start])) {++start;continue;}
    auto end=start+1;
    while(end<text.size() && !Han(text[end]) && WordChar(text[end])) ++end;
    while(piece<pieces.size() && pieces[piece].end<=start) ++piece;
    int reason=0;
    for(auto j=piece;j<pieces.size() && pieces[j].start<end;++j) reason|=pieces[j].reason;
    if(end-start<=40 && std::any_of(text.begin()+start,text.begin()+end,Latin)) add(start,end,reason);
    start=end;
  }
  // A selected pinyin candidate provides a real word boundary even if Jieba
  // splits its spelling. Common compound endings also occur in spoken names.
  std::set<std::pair<std::size_t,std::size_t>> compounds;
  if(source==WordSource::Pinyin && pieces.size()>1 && text.size()<=12 &&
     std::all_of(text.begin(),text.end(),WordChar) && pieces.front().start==0 && pieces.back().end==text.size())
    compounds.emplace(0,pieces.size()-1);
  for(std::size_t i=1;i<pieces.size();++i) {
    const auto& p=pieces[i];
    if(p.text!=L"输入法" && p.text!=L"公司" && p.text!=L"系统" && p.text!=L"项目" && p.text!=L"模型") continue;
    if(pieces[i-1].end==p.start && p.end-pieces[i-1].start<=12)
      compounds.emplace(i-1,i);
    if(i>=2 && pieces[i-2].end==pieces[i-1].start && pieces[i-1].text.size()==1 &&
       pieces[i-1].end==p.start && p.end-pieces[i-2].start<=12) compounds.emplace(i-2,i);
  }
  for(std::size_t first=0;first<pieces.size();) {
    const auto& p=pieces[first];
    auto last=first;
    while(p.reason && last+1<pieces.size() && pieces[last+1].reason==p.reason &&
          pieces[last].end==pieces[last+1].start && pieces[last+1].end-p.start<=12) ++last;
    if(last>first) compounds.emplace(first,last);
    first=last+1;
  }
  for(auto [first,last]:compounds) {
    int reason=0;
    for(auto i=first;i<=last;++i) reason|=pieces[i].reason;
    add(pieces[first].start,pieces[last].end,reason);
  }
  std::vector<ReviewWord> result;
  for(auto& it:words) result.push_back(std::move(it.second));
  return result;
}

ReviewCollector::ReviewCollector(Lexicon& lexicon,std::string dictionary)
    :lexicon_(lexicon),dictionary_(std::move(dictionary)) {}
ReviewCollector::~ReviewCollector() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_=true;
    shutdown_deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(2);
  }
  ready_.notify_one();
  if(worker_.joinable()) worker_.join();
}
void ReviewCollector::Submit(WordSource source,const std::string& text,const std::string& original,
                             const std::string& draft,bool short_retry) {
  const auto epoch=lexicon_.ReviewEpoch();
  if(!epoch || text.empty()) return;
  const auto receipt=std::chrono::system_clock::now();
  const auto bytes=text.size()+original.size()+draft.size();
  std::lock_guard<std::mutex> lock(mutex_);
  if(stopping_) return;
  if(pending_.size()>=kMaxQueuedInputs || bytes>kMaxQueuedBytes-pending_bytes_) {
    failed_=true;
    return;
  }
  try {
    if(!worker_.joinable()) worker_=std::thread(&ReviewCollector::Run,this);
    pending_.push_back({source,{text,original,draft},receipt,epoch,short_retry});
    pending_bytes_+=bytes;
  } catch(const std::exception&) {
    failed_=true;
    return;
  }
  ready_.notify_one();
}

bool ReviewCollector::StoreWhenAvailable(const Input& input,const std::vector<ReviewWord>& words) {
  auto backoff=std::chrono::milliseconds(50);
  while(input.epoch==lexicon_.ReviewEpoch() &&
        std::chrono::system_clock::now()-input.receipt < std::chrono::seconds(kReviewWindow)) {
    try {
      lexicon_.RecordReview(words,input.source,input.receipt,input.epoch);
      return true;
    } catch(const LexiconBusy&) {
      std::unique_lock<std::mutex> lock(mutex_);
      if(stopping_ && std::chrono::steady_clock::now()>=shutdown_deadline_) return false;
      if(stopping_) ready_.wait_until(lock,std::min(shutdown_deadline_,std::chrono::steady_clock::now()+backoff));
      else ready_.wait_for(lock,backoff,[&]{return stopping_;});
      backoff=std::min(backoff*2,std::chrono::milliseconds(800));
    }
  }
  return true;
}

void ReviewCollector::Run() {
  std::unique_ptr<Segmenter> segmenter;
  for(;;) {
    Input job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if(segmenter && !ready_.wait_for(lock,std::chrono::minutes(1),[&]{return stopping_ || !pending_.empty();})) {
        lock.unlock(); segmenter.reset(); continue;
      }
      ready_.wait(lock,[&]{return stopping_ || !pending_.empty();});
      if(pending_.empty()) return;
      job=std::move(pending_.front());
      pending_.pop_front();
      pending_bytes_-=job.text.written.size()+job.text.recognized.size()+job.text.draft.size();
    }
    if(job.epoch!=lexicon_.ReviewEpoch()) continue;
    try {
      if(!segmenter) segmenter=std::make_unique<Segmenter>(dictionary_);
      std::vector<ReviewWord> words;
      {
        // Release the three full texts before waiting for a busy database.
        const auto text=std::move(job.text);
        words=ExtractReviewWords(*segmenter,job.source,text.written,text.recognized,text.draft,job.short_retry);
      }
      if(!StoreWhenAvailable(job,words)) { failed_=true; return; }
    } catch(...) {
      failed_=true;
      std::lock_guard<std::mutex> lock(mutex_);
      if(stopping_) return;
    }
  }
}
