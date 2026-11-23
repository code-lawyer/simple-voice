#include "review_window.hpp"
#include "utf8.hpp"
#include <ctime>
#include <stdexcept>
#include <windowsx.h>

namespace {
using namespace review_control;
constexpr int PageSize=40;
std::wstring Reasons(int reasons) {
  std::wstring text;
  if(reasons&UserEdited) text=L"你在写入前改过";
  for(const auto& [flag,label]:{std::pair{DraftDiffers,L"草稿与定稿写法不同"},
                                std::pair{ShortRecognitionRetried,L"短录音经重试得到"}})
    if(reasons&flag) {if(!text.empty()) text+=L" · ";text+=label;}
  return text;
}
}
using namespace review_control;
WordReviewWindow::~WordReviewWindow() { Close(); }
void WordReviewWindow::Close() { if(hwnd_) DestroyWindow(hwnd_); }
std::wstring WordReviewWindow::Text(int id) const {
  HWND field=GetDlgItem(hwnd_,id);
  std::wstring str(GetWindowTextLengthW(field)+1,L'\0');
  str.resize(GetWindowTextW(field,str.data(),static_cast<int>(str.size()))); return str;
}
void WordReviewWindow::Set(int id,const std::wstring& str) {
  if(Text(id)!=str) SetDlgItemTextW(hwnd_,id,str.c_str());
}
void WordReviewWindow::UpdateColors() {
  if(!list_) return;
  ListView_SetBkColor(list_,GetSysColor(COLOR_WINDOW));
  ListView_SetTextBkColor(list_,GetSysColor(COLOR_WINDOW));
  ListView_SetTextColor(list_,GetSysColor(COLOR_WINDOWTEXT));
  RedrawWindow(hwnd_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
}
void WordReviewWindow::Open(HWND owner,Lexicon& db,Code code,Save save,std::function<bool()> failed) {
  if(hwnd_) {
    ShowWindow(hwnd_,IsIconic(hwnd_)?SW_RESTORE:SW_SHOW);
    SetForegroundWindow(hwnd_);
    Reload();
    return;
  }
  UiDpiScope dpi_context;
  db_=&db; code_=std::move(code); save_=std::move(save); failed_=std::move(failed);
  INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&cc);
  WNDCLASSW wc{}; wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandleW(nullptr);
  wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.lpszClassName=L"SimpleVoiceWordReview";
  wc.hbrBackground=GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&wc);
  hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,wc.lpszClassName,L"轻音输入法 · 最近七天",
      WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN|WS_VSCROLL,CW_USEDEFAULT,CW_USEDEFAULT,760,660,owner,nullptr,wc.hInstance,this);
  if(!hwnd_) return;
  dpi_=UiDpi(hwnd_);
  auto add=[&](int id,const wchar_t* type,const wchar_t* caption,DWORD style) {
    const bool edit=std::wstring_view(type)==L"EDIT";
    return CreateWindowExW(edit?WS_EX_CLIENTEDGE:0,type,caption,WS_VISIBLE|WS_CHILD|style,0,0,0,0,
      hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),wc.hInstance,nullptr);
  };
  add(Collect,L"BUTTON",L"收集最近七天的候选词(&T)",BS_AUTOCHECKBOX|WS_TABSTOP);
  add(Clear,L"BUTTON",L"清空待审记录",WS_TABSTOP);
  add(Status,L"STATIC",L"",SS_LEFT);
  add(FilterLabel,L"STATIC",L"筛选(&L)",SS_LEFTNOWORDWRAP);
  HWND filter=add(Filter,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
  for(auto s:{L"常用词",L"待核对",L"全部候选"}) SendMessageW(filter,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s));
  SendMessageW(filter,CB_SETCURSEL,0,0);
  add(QueryLabel,L"STATIC",L"搜索(&F)",SS_LEFTNOWORDWRAP);add(Query,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
  add(Refresh,L"BUTTON",L"刷新",WS_TABSTOP);
  list_=add(List,WC_LISTVIEWW,L"待审词语",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER);
  ListView_SetExtendedListViewStyle(list_,LVS_EX_CHECKBOXES|LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
  UpdateColors();
  const wchar_t* headings[]={L"词语",L"次数",L"来源",L"待核对原因"};
  for(int i=0;i<4;++i) {
    LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=const_cast<wchar_t*>(headings[i]);col.cx=100;
    SendMessageW(list_,LVM_INSERTCOLUMNW,i,reinterpret_cast<LPARAM>(&col));
  }
  add(Empty,L"STATIC",L"",WS_BORDER|SS_CENTER|SS_CENTERIMAGE|SS_NOPREFIX);
  add(PageInfo,L"STATIC",L"",SS_LEFT);add(Prev,L"BUTTON",L"上一页",WS_TABSTOP);add(Next,L"BUTTON",L"下一页",WS_TABSTOP);
  add(ExampleLabel,L"STATIC",L"出现时的短例(&E)",0);
  add(Example,L"EDIT",L"",ES_AUTOHSCROLL|ES_READONLY|ES_NOHIDESEL|WS_TABSTOP);
  add(FromExample,L"BUTTON",L"用选区补词",WS_TABSTOP);
  add(WordLabel,L"STATIC",L"保存的词语(&W)",0); add(Word,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
  add(CodeLabel,L"STATIC",L"拼音输入码(&P)",0);add(review_control::Code,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP);
  add(Speech,L"BUTTON",L"这个词也用于语音识别（Qwen3）",BS_AUTOCHECKBOX|WS_TABSTOP);
  add(Feedback,L"STATIC",L"",SS_LEFT);add(review_control::Save,L"BUTTON",L"保存已勾选",BS_DEFPUSHBUTTON|WS_TABSTOP);
  add(Skip,L"BUTTON",L"略过已勾选",WS_TABSTOP);add(review_control::Close,L"BUTTON",L"关闭",WS_TABSTOP);
  SendDlgItemMessageW(hwnd_,Word,EM_SETLIMITTEXT,128,0);
  SendDlgItemMessageW(hwnd_,Query,EM_SETLIMITTEXT,128,0);
  SendDlgItemMessageW(hwnd_,review_control::Code,EM_SETLIMITTEXT,256,0);
  RECT parent{};GetWindowRect(owner,&parent);
  const auto work=UiWorkArea({int(parent.left),int(parent.top),int(parent.right-parent.left),int(parent.bottom-parent.top)});
  const auto width=std::min(work.w,UiPx(750,dpi_)),height=std::min(work.h,UiPx(625,dpi_));
  const auto left=std::clamp(int((parent.left+parent.right-width)/2),work.x,work.x+work.w-width);
  const auto top=std::clamp(int((parent.top+parent.bottom-height)/2),work.y,work.y+work.h-height);
  SetWindowPos(hwnd_,nullptr,left,top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
  Layout();ShowWindow(hwnd_,SW_SHOWNORMAL);SetForegroundWindow(hwnd_);
  try { Reload(); }
  catch(const std::exception& error) { Set(Feedback,Utf8ToWide(error.what())); }
  SetFocus(GetDlgItem(hwnd_,Button_GetCheck(GetDlgItem(hwnd_,Collect))==BST_CHECKED?Filter:Collect));
}
void WordReviewWindow::Layout() {
  if(!list_ || laying_out_) return;
  laying_out_=true;
  RECT r{};GetClientRect(hwnd_,&r);
  const int visible=std::max(1,MulDiv(r.bottom,96,dpi_));
  const int h=std::max(432,visible);
  SCROLLINFO scroll{sizeof(scroll),SIF_RANGE|SIF_PAGE,0,h-1,static_cast<UINT>(visible)};
  SetScrollInfo(hwnd_,SB_VERT,&scroll,TRUE);
  const int offset=GetScrollPos(hwnd_,SB_VERT);
  GetClientRect(hwnd_,&r);
  const int w=MulDiv(r.right,96,dpi_);
  const int margin=16,right=w-margin,footer=h-47,bottom=footer-219;
  const int listTop=115,listBottom=std::max(listTop+42,bottom);
  auto move=[&](int id,int x,int y,int width,int height) {
    MoveWindow(GetDlgItem(hwnd_,id),UiPx(x,dpi_),UiPx(y-offset,dpi_),UiPx(std::max(1,width),dpi_),UiPx(height,dpi_),TRUE);
  };
  if(!font_) font_=UiFont(14,dpi_);
  for(int id=Collect;id<=FilterLabel;++id) SendDlgItemMessageW(hwnd_,id,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
  for(int id:{Query,Example,Word,review_control::Code})
    SendDlgItemMessageW(hwnd_,id,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(UiPx(5,dpi_),UiPx(5,dpi_)));
  move(Collect,margin,10,w-148,27);move(Clear,right-110,8,110,29);move(Status,margin,42,w-2*margin,23);
  move(FilterLabel,margin,79,56,23);move(Filter,margin+56,74,101,145);
  move(QueryLabel,margin+168,79,57,23);move(Query,margin+226,73,right-74-(margin+226),28);move(Refresh,right-65,73,65,28);
  move(List,margin,listTop,w-2*margin,listBottom-listTop);
  move(Empty,margin,listTop,w-2*margin,listBottom-listTop);
  const int room=w-2*margin-6;int sizes[]={room*36/100,room*11/100,room*19/100,room*34/100-12};
  for(int i=0;i<4;++i) ListView_SetColumnWidth(list_,i,UiPx(std::max(38,sizes[i]),dpi_));
  move(Prev,right-155,listBottom+4,74,27);move(Next,right-74,listBottom+4,74,27);
  move(PageInfo,margin,listBottom+7,w-210,22);
  int y=listBottom+36;
  move(ExampleLabel,margin,y,w-2*margin,20);move(Example,margin,y+21,w-146,28);move(FromExample,right-105,y+21,105,28);
  const int col=(w-2*margin-12)/2;
  move(WordLabel,margin,y+54,col,20);move(CodeLabel,margin+col+12,y+54,col,20);
  move(Word,margin,y+75,col,28);move(review_control::Code,margin+col+12,y+75,col,28);
  move(Speech,margin,y+109,w-2*margin,28);move(Feedback,margin,y+142,w-2*margin,39);
  move(review_control::Save,margin,footer,135,33);move(Skip,margin+144,footer,110,33);move(review_control::Close,right-74,footer,74,33);
  laying_out_=false;
}
void WordReviewWindow::Scroll(int position) {
  const int before=GetScrollPos(hwnd_,SB_VERT);
  SetScrollPos(hwnd_,SB_VERT,position,TRUE);
  if(GetScrollPos(hwnd_,SB_VERT)!=before) Layout();
}
void WordReviewWindow::RevealFocusedControl() {
  HWND target=GetFocus();
  if(!IsChild(hwnd_,target)) return;
  RECT control{},client{};
  GetWindowRect(target,&control);
  MapWindowPoints(nullptr,hwnd_,reinterpret_cast<POINT*>(&control),2);
  GetClientRect(hwnd_,&client);
  const int delta=control.top<0?control.top:std::max(0L,control.bottom-client.bottom);
  if(delta) Scroll(GetScrollPos(hwnd_,SB_VERT)+MulDiv(delta,96,dpi_)+(delta<0?-4:4));
}
ReviewChoice WordReviewWindow::Choice(const ReviewWord& row) {
  auto found=selections_.find(row.text);
  if(found!=selections_.end()) return found->second.choice;
  return {row.text,row.text,code_?code_(row.text):std::string{},false};
}
bool WordReviewWindow::PruneSelections(std::int64_t now) {
  bool removed=false;
  for(auto it=selections_.begin();it!=selections_.end();) {
    if(db_->ReviewCandidateExists(it->first,now)) ++it;
    else {it=selections_.erase(it); removed=true;}
  }
  return removed;
}
void WordReviewWindow::Reload() {
  const auto now=std::time(nullptr);
  const std::string selected=active_>=0?rows_[active_].text:std::string{};
  const bool expired=PruneSelections(now);
  auto filter=static_cast<ReviewFilter>(SendDlgItemMessageW(hwnd_,Filter,CB_GETCURSEL,0,0));
  auto data=db_->ReviewWords(filter,WideToUtf8(Text(Query)),PageSize+1,offset_,now);
  while(data.empty() && offset_>0) { offset_=std::max(0,offset_-PageSize);
    data=db_->ReviewWords(filter,WideToUtf8(Text(Query)),PageSize+1,offset_,now); }
  const auto settings=db_->ReviewSettings(now);
  KillTimer(hwnd_,1);
  if(settings.since) {
    const auto seconds=std::clamp<std::int64_t>(settings.since+kReviewWindow-now,1,kReviewWindow);
    SetTimer(hwnd_,1,static_cast<UINT>(seconds*1000),nullptr);
  }
  updating_=true;active_=-1;
  Button_SetCheck(GetDlgItem(hwnd_,Collect),settings.enabled?BST_CHECKED:BST_UNCHECKED);
  const int days=settings.since?std::clamp<int>((now-settings.since)/86400+1,1,7):0;
  const std::wstring state=settings.enabled?L"正在本机收集。":L"收集已关闭。";
  Set(Status,state+(days?L"已有最近 "+std::to_wstring(days)+L" 天内的记录，逐笔保留七天。":
      (settings.enabled?std::wstring(L"尚无实际写入记录。"):std::wstring(L"开启后从下一次实际写入开始统计。"))));
  const bool more=data.size()>PageSize;if(more)data.resize(PageSize);
  rows_=std::move(data);ListView_DeleteAllItems(list_);
  Set(PageInfo,offset_||more?L"第 "+std::to_wstring(offset_/PageSize+1)+L" 页 · "+
      std::to_wstring(offset_+1)+L"–"+std::to_wstring(offset_+rows_.size()):
      L"共 "+std::to_wstring(rows_.size())+L" 个候选");
  int selected_index=0;
  for(int i=0;i<static_cast<int>(rows_.size());++i) {
    const auto& row=rows_[i];auto term=Utf8ToWide(row.text);
    LVITEMW cell{};cell.mask=LVIF_TEXT;cell.iItem=i;cell.pszText=term.data();
    SendMessageW(list_,LVM_INSERTITEMW,0,reinterpret_cast<LPARAM>(&cell));
    std::wstring fields[]={std::to_wstring(row.count),row.sources==3?L"语音、拼音":row.sources==2?L"语音":L"拼音",Reasons(row.reasons)};
    for(int k=0;k<3;++k){cell.iSubItem=k+1;cell.pszText=fields[k].data();SendMessageW(list_,LVM_SETITEMTEXTW,i,reinterpret_cast<LPARAM>(&cell));}
    const auto saved=selections_.find(row.text);
    ListView_SetCheckState(list_,i,saved!=selections_.end() && saved->second.checked);
    if(row.text==selected) selected_index=i;
  }
  EnableWindow(GetDlgItem(hwnd_,Prev),offset_>0);EnableWindow(GetDlgItem(hwnd_,Next),more);
  if(!rows_.empty()) {
    active_=selected_index;
    ListView_SetItemState(list_,active_,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    ListView_EnsureVisible(list_,active_,FALSE);
  }
  const wchar_t* empty_text=L"最近七天暂无候选词。";
  if(!Text(Query).empty()) empty_text=L"没有匹配的词，请尝试其他写法。";
  else if(settings.since && filter==ReviewFilter::Frequent) empty_text=L"暂无重复用词，可切换「全部候选」。";
  else if(settings.since && filter==ReviewFilter::Uncertain) empty_text=L"最近七天暂无待核对的词。";
  Set(Empty,empty_text);
  ShowWindow(list_,rows_.empty()?SW_HIDE:SW_SHOWNA);
  ShowWindow(GetDlgItem(hwnd_,Empty),rows_.empty()?SW_SHOWNA:SW_HIDE);
  Set(Feedback,rows_.empty()?L"":L"勾选要保存的词。可以先改字，或选取短例中的词组。");
  if(expired) Set(Feedback,L"过期或已处理的勾选已移除，其余修改已保留。");
  if(failed_ && failed_()) Set(Feedback,L"本次运行有输入未能统计，请检查本地词库文件及写入权限。");
  updating_=false; Detail();
}
void WordReviewWindow::Controls() {
  const auto count=std::count_if(selections_.begin(),selections_.end(),[](const auto& entry){return entry.second.checked;});
  for(auto id:{review_control::Save,Skip}) EnableWindow(GetDlgItem(hwnd_,id),count>0);
  Set(review_control::Save,L"加入拼音词库 ("+std::to_wstring(count)+L")");
}
void WordReviewWindow::Detail() {
  updating_=true;
  const bool has=active_>=0 && active_<static_cast<int>(rows_.size());
  ReviewChoice item;
  if(has) item=Choice(rows_[active_]);
  Set(Example,has?Utf8ToWide(rows_[active_].example):L"");Set(Word,Utf8ToWide(item.word));
  Set(review_control::Code,Utf8ToWide(item.code));Button_SetCheck(GetDlgItem(hwnd_,Speech),item.speech?BST_CHECKED:BST_UNCHECKED);
  for(int id:{Example,Word,review_control::Code,Speech,FromExample})EnableWindow(GetDlgItem(hwnd_,id),has);
  updating_=false;Controls();
}
void WordReviewWindow::Changed() {
  if(updating_ || active_<0) return;
  const auto& row=rows_[active_];
  ReviewChoice next{row.text,WideToUtf8(Text(Word)),WideToUtf8(Text(review_control::Code)),Button_GetCheck(GetDlgItem(hwnd_,Speech))==BST_CHECKED};
  auto& entry=selections_[row.text];
  next.from_example=entry.choice.from_example;
  entry={std::move(next),true};
  updating_=true;ListView_SetCheckState(list_,active_,TRUE);updating_=false;
  Controls();
}
void WordReviewWindow::Notify(const NMLISTVIEW& e) {
  if(updating_ || e.iItem<0 || e.iItem>=static_cast<int>(rows_.size()) || !(e.uChanged&LVIF_STATE)) return;
  const auto& row=rows_[e.iItem];
  if((e.uOldState^e.uNewState)&LVIS_STATEIMAGEMASK) {
    const bool checked=((e.uNewState&LVIS_STATEIMAGEMASK)>>12)==2;
    selections_.insert_or_assign(row.text,Selection{Choice(row),checked});
    Controls();
  }
  if((e.uNewState&LVIS_SELECTED) && !(e.uOldState&LVIS_SELECTED)) {active_=e.iItem;Detail();}
}
void WordReviewWindow::Command(int id,int event) {
  if(updating_)return;
  if(id==review_control::Close || id==IDCANCEL) {Close();return;}
  if((id==Filter && event==CBN_SELCHANGE)||(id==Query && event==EN_CHANGE)){offset_=0;Reload();return;}
  if(id==Word && event==EN_CHANGE) {
    updating_=true;Set(review_control::Code,Utf8ToWide(code_?code_(WideToUtf8(Text(Word))):""));updating_=false;Changed();return;
  }
  if(id==review_control::Code && event==EN_CHANGE){Changed();return;}
  if(event!=BN_CLICKED)return;
  if(id==Speech){Changed();return;}
  if(id==FromExample && active_>=0) {
    DWORD a=0,b=0;SendDlgItemMessageW(hwnd_,Example,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b));
    if(a==b){Set(Feedback,L"先在短例中选择要保存的文字。");SetFocus(GetDlgItem(hwnd_,Example));return;}
    auto& source=rows_[active_];
    auto choice=Choice(source);
    choice.from_example=true;
    selections_.insert_or_assign(source.text,Selection{std::move(choice),true});
    Set(Word,Text(Example).substr(a,b-a));Changed();
    Set(Feedback,L"选区已勾选为补充词。保存后原候选仍可继续审阅。");return;
  }
  if(id==Collect){db_->SetReviewEnabled(Button_GetCheck(GetDlgItem(hwnd_,Collect))==BST_CHECKED);Reload();return;}
  if(id==Clear) {
    if(MessageBoxW(hwnd_,L"清空最近七天待审的词语、次数和短例？已保存的个人词语会保留。",L"清空待审记录",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES)return;
    db_->ClearReview();selections_.clear();offset_=0;Reload();return;
  }
  if(id==Refresh || id==Prev || id==Next) {if(id!=Refresh)offset_=std::max(0,offset_+(id==Next?PageSize:-PageSize));Reload();return;}
  if(id==review_control::Save || id==Skip) {
    if(PruneSelections(std::time(nullptr))) {
      Reload();Set(Feedback,L"有勾选已过期或已处理，其余选择已保留。请确认后保存。");return;
    }
    std::vector<ReviewChoice> choices;std::vector<std::string> words;
    for(const auto& [word,entry]:selections_) if(entry.checked) {
      words.push_back(word);choices.push_back(entry.choice);
    }
    if(words.empty()) return;
    std::wstring message;
    if(id==Skip) {db_->SkipReview(words);message=L"已略过所选记录。今后再次使用时会重新计数。";}
    else message=save_(choices);
    for(const auto& word:words) selections_.erase(word);
    Reload();Set(Feedback,message);
  }
}
bool WordReviewWindow::Translate(MSG* msg) {
  if(!hwnd_ || (msg->hwnd!=hwnd_ && !IsChild(hwnd_,msg->hwnd))) return false;
  if(msg->message==WM_KEYDOWN && msg->wParam==VK_ESCAPE) {
    if(SendDlgItemMessageW(hwnd_,Filter,CB_GETDROPPEDSTATE,0,0)) SendDlgItemMessageW(hwnd_,Filter,CB_SHOWDROPDOWN,FALSE,0);
    else Close();
    return true;
  }
  const bool handled=IsDialogMessageW(hwnd_,msg)!=FALSE;
  if(handled && hwnd_) RevealFocusedControl();
  return handled;
}
LRESULT CALLBACK WordReviewWindow::Proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
  auto* self=reinterpret_cast<WordReviewWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if(msg==WM_NCCREATE){self=static_cast<WordReviewWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
  if(self) try {
    switch(msg) {
      case WM_COMMAND:self->Command(LOWORD(wp),HIWORD(wp));return 0;
      case WM_NOTIFY:if(reinterpret_cast<NMHDR*>(lp)->idFrom==List && reinterpret_cast<NMHDR*>(lp)->code==LVN_ITEMCHANGED) self->Notify(*reinterpret_cast<NMLISTVIEW*>(lp));return 0;
      case WM_SIZE:self->Layout();return 0;
      case WM_TIMER:if(wp==1) self->Reload();return 0;
      case WM_VSCROLL:{
        SCROLLINFO info{sizeof(info),SIF_ALL};GetScrollInfo(hwnd,SB_VERT,&info);
        int pos=info.nPos;
        switch(LOWORD(wp)){
          case SB_LINEUP:pos-=20;break;case SB_LINEDOWN:pos+=20;break;
          case SB_PAGEUP:pos-=info.nPage;break;case SB_PAGEDOWN:pos+=info.nPage;break;
          case SB_TOP:pos=0;break;case SB_BOTTOM:pos=info.nMax;break;
          case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=info.nTrackPos;break;
        }
        self->Scroll(pos);return 0;}
      case WM_MOUSEWHEEL:self->Scroll(GetScrollPos(hwnd,SB_VERT)-GET_WHEEL_DELTA_WPARAM(wp)*40/WHEEL_DELTA);return 0;
      case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={UiPx(408,self->dpi_),UiPx(300,self->dpi_)};return 0;
      case WM_DPICHANGED:{self->dpi_=HIWORD(wp);auto old=self->font_;self->font_=nullptr;
        auto* r=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);self->Layout();if(old)DeleteObject(old);return 0;}
      case WM_SYSCOLORCHANGE:case WM_SETTINGCHANGE:self->UpdateColors();break;
      case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:{auto dc=reinterpret_cast<HDC>(wp);
        SetTextColor(dc,GetSysColor(IsWindowEnabled(reinterpret_cast<HWND>(lp))?COLOR_WINDOWTEXT:COLOR_GRAYTEXT));
        SetBkColor(dc,GetSysColor(COLOR_WINDOW));return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));}
      case WM_NEXTDLGCTL:{auto next=lp?reinterpret_cast<HWND>(wp):GetNextDlgTabItem(hwnd,GetFocus(),wp!=0);if(next)SetFocus(next);return 0;}
      case WM_CLOSE:self->Close();return 0;
      case WM_NCDESTROY:self->hwnd_=self->list_=nullptr;self->active_=-1;self->offset_=0;self->updating_=false;
        self->rows_.clear();self->selections_.clear();
        self->code_={};self->save_={};self->failed_={};self->db_=nullptr;
        if(self->font_)DeleteObject(self->font_);self->font_=nullptr;break;
    }
  } catch(const std::exception& error) {self->updating_=false;self->Set(Feedback,Utf8ToWide(error.what()));return 0;}
  return DefWindowProcW(hwnd,msg,wp,lp);
}
