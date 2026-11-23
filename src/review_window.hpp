#pragma once
#include "lexicon.hpp"
#include "ui_theme.hpp"
#include <commctrl.h>
#include <functional>
#include <map>

namespace review_control {
enum { Collect=301,Filter,Query,List,Example,Word,Code,Speech,FromExample,Save,Skip,Clear,
       Prev,Next,Feedback,Status,ExampleLabel,WordLabel,CodeLabel,QueryLabel,Refresh,PageInfo,Close,Empty,FilterLabel };
}

class WordReviewWindow {
 public:
  using Code = std::function<std::string(const std::string&)>;
  using Save = std::function<std::wstring(const std::vector<ReviewChoice>&)>;
  ~WordReviewWindow();
  void Open(HWND owner,Lexicon& db,Code code,Save save,std::function<bool()> failed);
  void Close();
  bool Translate(MSG* msg);
 private:
  HWND hwnd_=nullptr,list_=nullptr;
  HFONT font_=nullptr;
  UINT dpi_=96;
  Lexicon* db_=nullptr;
  Code code_;
  Save save_;
  std::function<bool()> failed_;
  int offset_=0,active_=-1;
  bool updating_=false,laying_out_=false;
  std::vector<ReviewWord> rows_;
  struct Selection { ReviewChoice choice; bool checked=false; };
  std::map<std::string,Selection> selections_;
  void Layout();
  void Scroll(int position);
  void RevealFocusedControl();
  void UpdateColors();
  void Reload();
  bool PruneSelections(std::int64_t now);
  void Detail();
  void Changed();
  void Controls();
  void Command(int id,int event);
  void Notify(const NMLISTVIEW& event);
  void Set(int id,const std::wstring& str);
  std::wstring Text(int id) const;
  ReviewChoice Choice(const ReviewWord& row);
  static LRESULT CALLBACK Proc(HWND,UINT,WPARAM,LPARAM);
};
