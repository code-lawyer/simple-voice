#include "preferences_window.hpp"
#include "preferences_controls.hpp"
#include "brand_icon.hpp"
#include "candidate_paint.hpp"
#include "product.hpp"
#include "utf8.hpp"
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>

namespace {
using namespace preference;
constexpr int Frame=-1, AllPages=-2;
constexpr int RailWidth=168, CompactHeader=48, NavigationHeight=40;
constexpr int FormWidth=676, ProseWidth=532;
struct PageInfo { const wchar_t* title; wchar_t icon; };
constexpr PageInfo Pages[]={
  {L"输入偏好",0xe765}, {L"我的词语",0xe8f1}, {L"语音设备",0xe720},
  {L"使用诊断",0xe9d9}, {L"本地模型",0xe950}, {L"备份恢复",0xe74e}
};
constexpr int PageCount=static_cast<int>(std::size(Pages));

int Dip(int pixels,UINT dpi) { return MulDiv(pixels,96,static_cast<int>(dpi)); }

int FormWidthFor(int width,bool compact) {
  return std::clamp(width-2*(compact?18:30),1,FormWidth);
}

void ActivateSettings(HWND window) {
  if(IsIconic(window)) ShowWindow(window,SW_RESTORE);
  // A background launch can supply SW_HIDE for the first ShowWindow call.
  // Opening settings is an explicit request to display this top-level window.
  SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_SHOWWINDOW);
  SetForegroundWindow(window);
}

int TextHeight(HDC dc,HWND control,int width,UINT flags=0) {
  const auto length=GetWindowTextLengthW(control);
  if(!length) return 0;
  std::wstring text(length+1,L'\0');
  const int read=GetWindowTextW(control,text.data(),length+1);
  auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
  RECT rect{0,0,std::max(1,width),0};
  DrawTextW(dc,text.data(),read,&rect,DT_CALCRECT|DT_WORDBREAK|DT_EDITCONTROL|flags);
  SelectObject(dc,old);
  return rect.bottom;
}

std::wstring DisplayCaption(std::wstring text,bool show_keys) {
  if(!show_keys && text.size()>=4 && text.compare(text.size()-4,2,L"(&")==0 && text.back()==L')')
    text.resize(text.size()-4);
  return text;
}

void RoundFill(HDC dc,RECT bounds,int diameter,COLORREF fill,COLORREF edge) {
  const int saved=SaveDC(dc);
  SelectObject(dc,GetStockObject(DC_BRUSH)); SetDCBrushColor(dc,fill);
  SelectObject(dc,GetStockObject(DC_PEN)); SetDCPenColor(dc,edge);
  RoundRect(dc,bounds.left,bounds.top,bounds.right,bounds.bottom,diameter,diameter);
  RestoreDC(dc,saved);
}

LRESULT ControlColor(HDC dc,COLORREF text,COLORREF background) {
  SetTextColor(dc,text); SetBkColor(dc,background); SetDCBrushColor(dc,background);
  return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
}

// The HWND keeps the real mnemonic for Windows accessibility and dialog
// navigation. Let the system draw the button face when we hide its suffix.
void DrawSystemButtonFace(const NMCUSTOMDRAW& item,bool checkbox,UINT dpi,RECT& text) {
  HWND button=item.hdr.hwndFrom;
  const bool enabled=IsWindowEnabled(button)!=FALSE;
  const bool pressed=(item.uItemState&CDIS_SELECTED)!=0,hot=(item.uItemState&CDIS_HOT)!=0;
  HTHEME theme=OpenThemeData(button,L"Button");
  UINT classic_state=enabled?0:DFCS_INACTIVE;
  if(pressed) classic_state|=DFCS_PUSHED;
  RECT face=text;
  int part=BP_PUSHBUTTON,state=PBS_NORMAL;
  if(checkbox) {
    const bool checked=SendMessageW(button,BM_GETCHECK,0,0)==BST_CHECKED;
    part=BP_CHECKBOX;
    state=(checked?CBS_CHECKEDNORMAL:CBS_UNCHECKEDNORMAL)+(!enabled?3:pressed?2:hot?1:0);
    classic_state|=DFCS_BUTTONCHECK|(checked?DFCS_CHECKED:0);
    SIZE size{UiPx(13,dpi),UiPx(13,dpi)};
    if(theme) GetThemePartSize(theme,item.hdc,part,state,nullptr,TS_DRAW,&size);
    face.top+=(face.bottom-face.top-size.cy)/2;
    face.bottom=face.top+size.cy; face.right=face.left+size.cx;
    text.left=face.right+UiPx(5,dpi);
  } else {
    const bool is_default=(GetWindowLongPtrW(button,GWL_STYLE)&BS_TYPEMASK)==BS_DEFPUSHBUTTON;
    state=!enabled?PBS_DISABLED:pressed?PBS_PRESSED:hot?PBS_HOT:is_default?PBS_DEFAULTED:PBS_NORMAL;
    classic_state|=DFCS_BUTTONPUSH;
    if(!theme && is_default) {
      FrameRect(item.hdc,&face,GetSysColorBrush(COLOR_WINDOWFRAME)); InflateRect(&face,-1,-1);
    }
  }
  if(!theme || FAILED(DrawThemeBackground(theme,item.hdc,part,state,&face,nullptr)))
    DrawFrameControl(item.hdc,&face,DFC_BUTTON,classic_state);
  if(theme) CloseThemeData(theme);
}
}

PreferencesWindow::~PreferencesWindow() { Close(); }

void PreferencesWindow::Close() {
  review_window_.Close();
  if(hwnd_ && actions_.practice) actions_.practice(2);
  if(hwnd_) DestroyWindow(hwnd_);
  hwnd_=pane_=nullptr;
  controls_.clear(); rows_.clear();
  for(auto object:{font_,heading_,section_,meta_,icons_}) if(object) DeleteObject(object);
  font_=heading_=section_=meta_=icons_=nullptr;
  if(mark_) DestroyIcon(mark_);
  mark_=nullptr;
}

HWND PreferencesWindow::Add(const wchar_t* type,const wchar_t* text,int id,int page,
                            int x,int row,int w,int h,DWORD style,Face face) {
  const bool edit=lstrcmpW(type,L"EDIT")==0;
  const DWORD edge=edit && !(style&ES_READONLY)?WS_EX_CLIENTEDGE:0;
  HWND control=CreateWindowExW(edge,type,text,WS_CHILD|style|(page==Frame?WS_VISIBLE:0),
    0,0,0,0,page==Frame?hwnd_:pane_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
  controls_.push_back({control,page,x,row,w,h,face,lstrcmpW(type,L"COMBOBOX")==0});
  return control;
}

void PreferencesWindow::Open(Lexicon* lexicon,InputPreferences preferences,UiSkin skin,PreferencesActions actions,int page) {
  if(hwnd_) {
    ActivateSettings(hwnd_); Page(page); return;
  }
  UiDpiScope scope;
  lexicon_=lexicon; preferences_=preferences; skin_=skin; actions_=std::move(actions);
  offset_=scroll_=0; hover_=-1; editing_.clear(); have_undo_=false;
  pending_model_feedback_.reset();
  INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
  WNDCLASSW klass{};
  klass.lpfnWndProc=Proc; klass.hInstance=GetModuleHandleW(nullptr); klass.hCursor=LoadCursor(nullptr,IDC_ARROW);
  klass.hIcon=LoadIconW(klass.hInstance,MAKEINTRESOURCEW(IDI_SIMPLE_VOICE));
  klass.lpszClassName=L"SimpleVoicePreferences"; RegisterClassW(&klass);
  hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,klass.lpszClassName,L"轻音输入法 · 设置",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
    CW_USEDEFAULT,CW_USEDEFAULT,840,554,nullptr,nullptr,klass.hInstance,this);
  if(!hwnd_) return;
  dpi_=UiDpi(hwnd_);

  Add(L"STATIC",L"轻音输入法",BrandMark,Frame,0,0,0,0,SS_OWNERDRAW);
  Add(L"STATIC",kProductDisplayName,BrandName,Frame,0,0,0,0,SS_CENTERIMAGE,Face::Section);
  Add(L"STATIC",L"Simple Voice",BrandEnglish,Frame,0,0,0,0,SS_CENTERIMAGE,Face::Meta);
  auto nav=Add(L"LISTBOX",L"设置分类",Navigation,Frame,0,0,0,0,
               WS_TABSTOP|LBS_HASSTRINGS|LBS_OWNERDRAWFIXED|LBS_NOINTEGRALHEIGHT|LBS_NOTIFY);
  auto compact=Add(L"COMBOBOX",L"设置分类",CompactNavigation,Frame,0,0,0,0,WS_TABSTOP|CBS_DROPDOWNLIST);
  for(const auto& entry:Pages) {
    SendMessageW(nav,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(entry.title));
    SendMessageW(compact,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(entry.title));
  }
  SetWindowSubclass(nav,NavigationProc,1,reinterpret_cast<DWORD_PTR>(this));
  Add(L"STATIC",Utf8ToWide("v" SIMPLE_VOICE_VERSION).c_str(),Version,Frame,0,0,0,0,SS_CENTERIMAGE,Face::Meta);

  klass.lpfnWndProc=PaneProc; klass.lpszClassName=L"SimpleVoiceSettingsContent";
  klass.hbrBackground=GetSysColorBrush(COLOR_WINDOW); RegisterClassW(&klass);
  pane_=CreateWindowExW(WS_EX_CONTROLPARENT,klass.lpszClassName,L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_CLIPCHILDREN,
    0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(Content)),klass.hInstance,nullptr);
  if(!pane_) { Close(); return; }
  BuildForm(actions_.models?actions_.models():ModelSettings{});
  Add(L"EDIT",L"",StatusLine,Frame,0,0,0,0,WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL,Face::Detail);
  Add(L"BUTTON",L"保存设置(&A)",SaveOptions,Frame,0,0,0,0,WS_TABSTOP|BS_DEFPUSHBUTTON);
  Add(L"BUTTON",L"关闭",IDCANCEL,Frame,0,0,0,0,WS_TABSTOP);
  UpdateAppearance();
  if(!always_cues_) SendMessageW(hwnd_,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEACCEL),0);
  Devices(); Search(); FitWindow(nullptr); Page(page);
  SetTimer(hwnd_,1,1000,nullptr);
  ActivateSettings(hwnd_);
  SetFocus(Field(hwnd_,compact_?CompactNavigation:Navigation));
}

void PreferencesWindow::BuildForm(const ModelSettings& models) {
  int page=AllPages,row=0;
  auto next=[&](int gap) {
    row=static_cast<int>(rows_.size()); rows_.push_back({page,gap});
  };
  // Negative x anchors to the right. A non-positive width leaves that right inset.
  auto label=[&](int id,const wchar_t* text,int x=0,int w=0,Face face=Face::Body) {
    const DWORD style=wcsstr(text,L"(&")?SS_OWNERDRAW:SS_LEFT;
    return Add(L"STATIC",text,id,page,x,row,w,0,style,face);
  };
  auto button=[&](int id,const wchar_t* text,int x,int w,DWORD style=0) {
    return Add(L"BUTTON",text,id,page,x,row,w,32,WS_TABSTOP|style);
  };
  auto combo=[&](int id,int x,int w,DWORD style=0) {
    return Add(L"COMBOBOX",L"",id,page,x,row,w,190,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL|style);
  };
  auto edit=[&](int id,int x,int w,int h,bool output=false) {
    const DWORD style=output?ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL:ES_AUTOHSCROLL;
    return Add(L"EDIT",L"",id,page,x,row,w,h,WS_TABSTOP|style);
  };
  auto divider=[&](int index) {
    next(18); Add(L"STATIC",L"",200+4*page+index,page,0,row,0,1,SS_OWNERDRAW);
  };
  next(24); label(PageTitle,L"",0,0,Face::Title);

  page=0;
  next(22); label(102,L"输入浮层外观(&S)",0,-145,Face::Section);
  HWND skins=combo(Skin,-132,132,CBS_OWNERDRAWFIXED|CBS_HASSTRINGS);
  for(auto choice:{UiSkin::Titanium,UiSkin::Daylight,UiSkin::DeepSpace})
    SendMessageW(skins,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(SkinLabel(choice)));
  SendMessageW(skins,CB_SETCURSEL,static_cast<int>(skin_),0);
  next(9); Add(L"STATIC",L"候选条预览：拼音 qingyin，首选词为轻音",SkinPreview,page,0,row,0,102,SS_OWNERDRAW);
  next(24); label(120,L"键盘与语音",0,0,Face::Section);
  next(8); button(Learning,L"记住拼音选词习惯(&L)",0,0,BS_AUTOCHECKBOX);
  SendMessageW(Field(hwnd_,Learning),BM_SETCHECK,preferences_.learning?BST_CHECKED:BST_UNCHECKED,0);
  next(2); label(101,L"关闭后暂停读取和学习选词习惯，基础词表和「我的词语」仍可用。",22,0,Face::Detail);
  divider(0);
  next(12); label(121,L"语音快捷键",0,-135); button(HotkeyButton,L"修改快捷键(&K)",-120,120);
  next(1); Add(L"STATIC",actions_.hotkey_label?actions_.hotkey_label().c_str():L"",HotkeyValue,page,0,row,0,24,SS_OWNERDRAW);
  next(20); label(103,L"空格选词，Enter 保留原始输入，单按 Shift 切换中英文。\n方向键编辑拼音，Ctrl + Delete 遗忘选中的学习候选。",0,0,Face::Detail);

  page=1;
  next(22); button(RecentReview,L"最近七天(&R)",-116,116);
  label(104,L"搜索词语或输入码(&F)",0,-126);
  next(6); edit(Query,0,0,28);
  next(8); Add(L"LISTBOX",L"",Terms,page,0,row,0,116,WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT);
  Add(L"STATIC",L"",TermsEmpty,page,0,row,0,116,WS_BORDER|SS_CENTER|SS_CENTERIMAGE,Face::Detail);
  next(7); label(Count,L"",0,-162,Face::Detail);
  button(Previous,L"上一页",-150,70); button(Next,L"下一页",-70,70);
  divider(0);
  next(16); const int captions=row;
  next(6); const int inputs=row;
  row=captions; label(105,L"词语 / 常用语(&W)",0,-210);
  row=inputs; edit(Term,0,-210,28);
  row=captions; label(106,L"输入码(&C)",-198,95);
  row=inputs; edit(Code,-198,98,28); button(AutoCode,L"生成输入码",-91,91);
  SendMessageW(Field(hwnd_,Term),EM_SETLIMITTEXT,256,0); SendMessageW(Field(hwnd_,Code),EM_SETLIMITTEXT,256,0);
  next(12); button(SaveTerm,L"保存词语",0,92);
  button(NewTerm,L"新增",100,62); button(DeleteTerm,L"删除",170,62); button(Undo,L"撤销删除",240,85);
  next(8); button(AddSpeechName,L"加入语音专名",0,126);
  next(10); label(107,L"输入码使用小写英文字母。手动保存的词语在 365 天未使用后清理。",0,0,Face::Detail);

  page=2;
  next(22); label(108,L"麦克风(&M)");
  next(6); combo(preference::Devices,0,-108); button(RefreshDevices,L"刷新设备",-98,98);
  divider(0);
  next(16); label(109,L"停录与写入",0,0,Face::Section);
  next(8); label(119,L"按 Enter 或再次按语音快捷键停录并校准，核对后再按 Enter 写入。停顿时会继续录音。单次上限 10 分钟，到时停录，等待你确认校准。");
  next(8); label(110,L"校准时按 Enter 不会提前写入。按 Esc 取消本次语音，也可以在预览中编辑、恢复识别原文或复制。",0,0,Face::Detail);
  divider(1);
  next(16); label(122,L"本地识别",0,0,Face::Section);
  next(8); label(ChineseOnly,L"录音由你启动，音频只在本机内存中处理。实时草稿由程序管理，可在「本地模型」中选择定稿模型。",0,0,Face::Detail);
  next(12); button(Retry,L"重新准备语音模型",0,158);

  page=3;
  next(22); label(123,L"语音试用",0,0,Face::Section);
  next(8); label(111,L"点击开始，等到「正在听」后说话。再次点击停录，核对后确认试用文字。");
  next(12); button(PracticeToggle,L"开始试用",0,126);
  button(PracticeConfirm,L"确认试用文字",134,106); button(PracticeCancel,L"取消试用",248,80);
  next(8); edit(PracticeText,0,0,56,true); Set(PracticeText,L"点击开始后才会打开麦克风。");
  next(9); label(130,L"日常输入时，按语音快捷键开始，按 Enter 停录并校准。\n核对后按 Enter 写入，按 Esc 取消。",0,0,Face::Detail);
  divider(0);
  next(16); label(124,L"运行信息",0,0,Face::Section);
  next(8); edit(Diagnostics,0,0,75,true);
  next(10); button(CopyDiagnostics,L"复制诊断信息",0,124); button(Guide,L"已了解使用方法",134,138);

  page=4;
  next(22); label(112,L"识别模型(&R)");
  next(6); auto choices=combo(ModelChoice,0,0);
  for(auto model:{SpeechModel::Qwen3,SpeechModel::SenseVoice,SpeechModel::Whisper})
    SendMessageW(choices,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(SpeechModelLabel(model)));
  if(models.model==SpeechModel::ChineseLegacy)
    SendMessageW(choices,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(SpeechModelLabel(models.model)));
  SendMessageW(choices,CB_SETCURSEL,static_cast<int>(models.model),0);
  next(7); label(ModelDescription,L"",0,0,Face::Detail);
  next(10); edit(ModelState,0,0,61,true);
  next(10); button(ModelUse,L"启用所选模型(&A)",0,149,BS_DEFPUSHBUTTON); button(ModelCancel,L"取消任务",159,91);
  divider(0);
  next(12); auto preload=button(ModelPreload,L"切换到轻音时预加载语音模型",0,0,BS_AUTOCHECKBOX);
  SendMessageW(preload,BM_SETCHECK,actions_.preload_on_activate?BST_CHECKED:BST_UNCHECKED,0);
  const auto idle_label=L"闲置 "+std::to_wstring(ModelSettings::kDefaultIdleMinutes)+L" 分钟后释放模型";
  next(4); auto idle=button(ModelIdle,idle_label.c_str(),0,0,BS_AUTOCHECKBOX);
  SendMessageW(idle,BM_SETCHECK,models.idle_minutes?BST_CHECKED:BST_UNCHECKED,0);
  next(12); label(113,L"常用专名(&H)");
  next(6); auto hints=edit(ModelHints,0,0,28); displayed_model_hints_=NormalizeSpeechHints(models.hotwords);
  SetWindowTextW(hints,Utf8ToWide(displayed_model_hints_).c_str());
  SendMessageW(hints,EM_SETLIMITTEXT,2048,0);
  next(6); label(129,L"仅 Qwen3-ASR 使用。多个专名用逗号分隔，可留空。",0,0,Face::Detail);
  next(14); button(ModelLocal,L"使用已有文件…",0,142); button(ModelDefault,L"恢复推荐模型",152,126);

  page=5;
  next(22); label(115,L"备份词语与设置",0,0,Face::Section);
  next(8); label(116,L"将手动保存的词语、快捷键、浮层外观和输入设置保存到你选择的文件夹。");
  next(12); button(DataBackup,L"创建备份…",0,124);
  divider(0);
  next(16); label(125,L"恢复已有备份",0,0,Face::Section);
  next(8); label(126,L"选择轻音备份文件夹。恢复前会自动备份当前数据，恢复失败时会保留原数据。");
  next(12); button(DataRestore,L"从备份恢复…",0,124);
  divider(1);
  next(16); label(127,L"备份范围",0,0,Face::Section);
  next(8); label(128,L"备份包含已确认的个人词语。最近七天的待审材料、语音模型及其配置、录音和拼音学习统计不进入备份。",0,0,Face::Detail);
}

void PreferencesWindow::UpdateAppearance() {
  colors_=UiPalette(UiSkin::Titanium);
  BOOL cues=FALSE; SystemParametersInfoW(SPI_GETKEYBOARDCUES,0,&cues,0); always_cues_=cues!=FALSE;
  const HFONT previous[]={font_,heading_,section_,meta_,icons_};
  font_=UiFont(14,dpi_); heading_=UiFont(24,dpi_,true);
  section_=UiFont(16,dpi_); meta_=UiFont(12,dpi_);
  icons_=CreateFontW(-UiPx(16,dpi_),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe MDL2 Assets");
  for(const auto& c:controls_) {
    const HFONT font=c.face==Face::Title?heading_:c.face==Face::Section?section_:c.face==Face::Meta?meta_:font_;
    SendMessageW(c.hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
    if(c.combo) SendMessageW(c.hwnd,CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),UiPx(22,dpi_));
  }
  for(int id:{Query,Term,Code,ModelHints}) SendMessageW(Field(hwnd_,id),EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(UiPx(6,dpi_),UiPx(6,dpi_)));
  for(auto font:previous) if(font) DeleteObject(font);
  SendMessageW(Field(hwnd_,Navigation),LB_SETITEMHEIGHT,0,UiPx(NavigationHeight,dpi_));
  const auto ink=colors_.text;
  const bool light_mark=GetRValue(ink)*299+GetGValue(ink)*587+GetBValue(ink)*114>128000;
  HICON mark=LoadProductIcon(GetModuleHandleW(nullptr),light_mark?IDI_SOUND_CORE_DARK:IDI_SOUND_CORE_LIGHT,UiPx(28,dpi_));
  if(mark_) DestroyIcon(mark_);
  mark_=mark;
  SendMessageW(Field(hwnd_,Skin),CB_SETITEMHEIGHT,0,UiPx(26,dpi_));
}

void PreferencesWindow::FitWindow(const RECT* suggested) {
  RECT rect{};
  if(suggested) rect=*suggested;
  else {
    RECT current{}; GetWindowRect(hwnd_,&current);
    rect={0,0,UiPx(840,dpi_),UiPx(554,dpi_)};
    const DWORD style=static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_STYLE));
    const DWORD ex=static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_EXSTYLE));
    using AdjustForDpi=BOOL(WINAPI*)(RECT*,DWORD,BOOL,DWORD,UINT);
    static auto adjust=reinterpret_cast<AdjustForDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"AdjustWindowRectExForDpi"));
    if(adjust) adjust(&rect,style,FALSE,ex,dpi_); else AdjustWindowRectEx(&rect,style,FALSE,ex);
    OffsetRect(&rect,current.left-rect.left,current.top-rect.top);
  }
  const Rect wanted{rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top};
  const auto placed=ClampRect(wanted,UiWorkArea(wanted));
  SetWindowPos(hwnd_,nullptr,placed.x,placed.y,placed.w,placed.h,SWP_NOZORDER|SWP_NOACTIVATE);
}

void PreferencesWindow::Layout() {
  if(!pane_ || !font_ || IsIconic(hwnd_)) return;
  RECT client{}; GetClientRect(hwnd_,&client);
  const int width=Dip(client.right,dpi_),height=Dip(client.bottom,dpi_);
  // The compact header also leaves all categories available on a short work area.
  const bool was_compact=compact_;
  compact_=width<728 || height<390;
  rail_=compact_?0:RailWidth;
  const int top=compact_?CompactHeader:0, right=width-rail_;
  const int form_width=FormWidthFor(right,compact_);
  const int pad=(right-form_width)/2;
  const bool save=page_==0 || page_==2;
  const int end=width-pad,close_width=66,save_width=100,gap=8;
  const int status_width=std::max(1,form_width-close_width-(save?save_width+gap:0)-14);
  HDC dc=GetDC(hwnd_);
  const int message_height=TextHeight(dc,Field(hwnd_,StatusLine),UiPx(status_width,dpi_),DT_NOPREFIX);
  ReleaseDC(hwnd_,dc);
  footer_=std::clamp(Dip(message_height,dpi_)+16,48,64);
  auto place=[&](int id,int x,int y,int w,int h) {
    MoveWindow(Field(hwnd_,id),UiPx(x,dpi_),UiPx(y,dpi_),UiPx(w,dpi_),UiPx(h,dpi_),TRUE);
  };
  const bool focus_navigation=GetFocus()==Field(hwnd_,was_compact?CompactNavigation:Navigation);
  ShowWindow(Field(hwnd_,Navigation),compact_?SW_HIDE:SW_SHOWNA);
  ShowWindow(Field(hwnd_,CompactNavigation),compact_?SW_SHOWNA:SW_HIDE);
  ShowWindow(Field(hwnd_,BrandEnglish),compact_?SW_HIDE:SW_SHOWNA);
  ShowWindow(Field(hwnd_,Version),compact_?SW_HIDE:SW_SHOWNA);
  place(BrandMark,compact_?17:20,compact_?10:25,28,28);
  place(BrandName,compact_?56:58,compact_?10:15,105,28);
  if(compact_) place(CompactNavigation,width-150,11,132,220);
  else {
    place(BrandEnglish,58,42,100,19);
    place(Navigation,12,83,rail_-24,PageCount*NavigationHeight);
    place(Version,22,height-37,rail_-40,22);
  }
  if(was_compact!=compact_ && focus_navigation) SetFocus(Field(hwnd_,compact_?CompactNavigation:Navigation));
  const int body_bottom=std::max(top,height-footer_);
  MoveWindow(pane_,UiPx(rail_,dpi_),UiPx(top,dpi_),client.right-UiPx(rail_,dpi_),UiPx(body_bottom-top,dpi_),TRUE);
  ShowWindow(Field(hwnd_,SaveOptions),save?SW_SHOWNA:SW_HIDE);
  const int buttons_y=body_bottom+(footer_-32)/2;
  place(IDCANCEL,end-close_width,buttons_y,close_width,32);
  if(save) place(SaveOptions,end-close_width-gap-save_width,buttons_y,save_width,32);
  place(StatusLine,rail_+pad,body_bottom+6,status_width,footer_-12);
  LayoutForm();
  for(int id:{StatusLine,ModelState,PracticeText,Diagnostics}) FitOutput(id);
  InvalidateRect(hwnd_,nullptr,TRUE);
}

void PreferencesWindow::LayoutForm() {
  RECT client{},outer{}; GetClientRect(pane_,&client); GetWindowRect(pane_,&outer);
  // First measure without a scrollbar. If one is needed, measure the narrower
  // text column once more before positioning controls.
  struct Cell { int left=0,width=0,height=0; };
  std::vector<Cell> cells(controls_.size());
  std::vector<int> heights(rows_.size()),tops(rows_.size());
  HDC dc=GetDC(pane_);
  auto measure=[&](int pixels) {
    const int width=FormWidthFor(Dip(pixels,dpi_),compact_);
    const int inset=(pixels-UiPx(width,dpi_))/2;
    std::fill(heights.begin(),heights.end(),0);
    for(size_t i=0;i<controls_.size();++i) {
      const auto& c=controls_[i];
      if(c.page!=page_ && c.page!=AllPages) continue;
      int x=c.x<0?width+c.x:c.x;
      int w=c.w<=0?width+c.w-x:c.w;
      const int id=GetDlgCtrlID(c.hwnd);
      if(c.page==1) {
        const int first=(width-114)/2;
        if(id==Term || id==105) w=first;
        if(id==Code || id==106) { x=first+12; w=width-100-x; }
      }
      if(c.h==0 && c.w==0 && c.face!=Face::Title) w=std::min(w,ProseWidth);
      Cell& cell=cells[i];
      cell.left=inset+UiPx(x,dpi_); cell.width=std::max(1,UiPx(w,dpi_));
      if(c.h==0) cell.height=TextHeight(dc,c.hwnd,cell.width);
      else if(c.combo) {
        const auto selection=SendMessageW(c.hwnd,CB_GETITEMHEIGHT,static_cast<WPARAM>(-1),0);
        cell.height=(selection==CB_ERR?UiPx(22,dpi_):static_cast<int>(selection))+UiPx(6,dpi_);
      } else cell.height=c.h==1 && !colors_.high_contrast?1:UiPx(c.h,dpi_);
      heights[c.row]=std::max(heights[c.row],cell.height);
    }
    int bottom=0;
    for(size_t i=0;i<rows_.size();++i) {
      if(rows_[i].page!=page_ && rows_[i].page!=AllPages) continue;
      const int gap=compact_ && rows_[i].page==AllPages?14:rows_[i].gap;
      tops[i]=bottom+UiPx(gap,dpi_);
      bottom=tops[i]+heights[i];
    }
    return bottom+UiPx(20,dpi_);
  };
  auto range=[&](int height) {
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,height-1),
                    static_cast<UINT>(client.bottom),scroll_,0};
    SetScrollInfo(pane_,SB_VERT,&info,TRUE);
  };
  const int full_width=outer.right-outer.left;
  range(measure(full_width));
  GetClientRect(pane_,&client);
  if(client.right!=full_width) range(measure(client.right));
  ReleaseDC(pane_,dc);
  scroll_=GetScrollPos(pane_,SB_VERT);
  for(size_t i=0;i<controls_.size();++i) {
    const auto& c=controls_[i];
    if(c.page!=page_ && c.page!=AllPages) continue;
    const auto& cell=cells[i];
    const int y=tops[c.row]+(heights[c.row]-cell.height)/2;
    MoveWindow(c.hwnd,cell.left,y-scroll_,cell.width,c.combo?UiPx(c.h,dpi_):cell.height,TRUE);
  }
}

void PreferencesWindow::FitOutput(int id) {
  HWND output=Field(hwnd_,id);
  if(!output) return;
  if(id==StatusLine) ShowWindow(output,GetWindowTextLengthW(output)?SW_SHOWNA:SW_HIDE);
  auto font=reinterpret_cast<HFONT>(SendMessageW(output,WM_GETFONT,0,0));
  RECT rect{}; GetClientRect(output,&rect);
  if(!font || !rect.bottom) return;
  HDC dc=GetDC(output); auto old=SelectObject(dc,font);
  TEXTMETRICW tm{}; GetTextMetricsW(dc,&tm); SelectObject(dc,old); ReleaseDC(output,dc);
  const int inset=UiPx(id==StatusLine?0:10,dpi_);
  const int top=UiPx(id==StatusLine?2:6,dpi_);
  auto format=[&] {
    GetClientRect(output,&rect); InflateRect(&rect,-inset,-top);
    SendMessageW(output,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&rect));
  };
  format();
  const auto lines=SendMessageW(output,EM_GETLINECOUNT,0,0);
  const bool needs_scroll=lines>std::max(1L,(rect.bottom-rect.top)/std::max(1L,tm.tmHeight));
  if(needs_scroll!=bool(GetWindowLongPtrW(output,GWL_STYLE)&WS_VSCROLL)) {
    ShowScrollBar(output,SB_VERT,needs_scroll);
    format();
  }
  if(id==StatusLine && lines==1) {
    GetClientRect(output,&rect); rect.top=std::max(0L,(rect.bottom-tm.tmHeight)/2);
    SendMessageW(output,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&rect));
  }
  InvalidateRect(output,nullptr,TRUE);
}

void PreferencesWindow::Scroll(int position) {
  if(!pane_) return;
  SetScrollPos(pane_,SB_VERT,position,TRUE);
  const int next=GetScrollPos(pane_,SB_VERT);
  if(next==scroll_) return;
  const int delta=scroll_-next;
  scroll_=next;
  // Move entire child windows, including those crossing the viewport boundary.
  HDWP batch=BeginDeferWindowPos(static_cast<int>(controls_.size()));
  for(const auto& control:controls_) {
    if(!batch) break;
    if(control.page!=page_ && control.page!=AllPages) continue;
    RECT bounds{};
    if(!GetWindowRect(control.hwnd,&bounds)) continue;
    MapWindowPoints(nullptr,pane_,reinterpret_cast<POINT*>(&bounds),2);
    batch=DeferWindowPos(batch,control.hwnd,nullptr,bounds.left,bounds.top+delta,0,0,
                         SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS);
  }
  if(!batch || !EndDeferWindowPos(batch)) LayoutForm();
}

void PreferencesWindow::RevealFocus() {
  HWND focus=GetFocus();
  if(!pane_ || IsIconic(hwnd_) || !focus || !IsChild(pane_,focus)) return;
  RECT rect{},client{}; GetWindowRect(focus,&rect); MapWindowPoints(nullptr,pane_,reinterpret_cast<POINT*>(&rect),2);
  GetClientRect(pane_,&client);
  if(rect.top<0) Scroll(scroll_+rect.top-UiPx(8,dpi_));
  else if(rect.bottom>client.bottom) Scroll(scroll_+rect.bottom-client.bottom+UiPx(8,dpi_));
}

void PreferencesWindow::Page(int page) {
  const auto focus=GetFocus();
  const bool content_focused=IsChild(pane_,focus)!=FALSE;
  page_=std::clamp(page,0,PageCount-1); scroll_=0;
  SendMessageW(Field(hwnd_,Navigation),LB_SETCURSEL,page_,0);
  SendMessageW(Field(hwnd_,CompactNavigation),CB_SETCURSEL,page_,0);
  Set(PageTitle,Pages[page_].title);
  if(page_==1 && lexicon_ && !terms_error_.empty() && IsWindowVisible(hwnd_)) Search();
  for(const auto& c:controls_) {
    if(c.page==Frame) continue;
    bool visible=c.page==AllPages || c.page==page_;
    const int id=GetDlgCtrlID(c.hwnd);
    if(id==TermsEmpty) visible=visible && entries_.empty();
    if(id==Terms) visible=visible && !entries_.empty();
    ShowWindow(c.hwnd,visible?SW_SHOWNA:SW_HIDE);
  }
  const wchar_t* const hints[]={L"保存后应用外观和选词习惯。",L"仅显示你手动保存的词语。",
    L"麦克风设置在下次录音生效。",L"确认的试用文字只显示在本页。",
    L"启用时会下载缺少的文件，验证通过后切换。",L""};
  const wchar_t* hint=page_==1 && !terms_error_.empty()?terms_error_.c_str():hints[page_];
  if(Text(StatusLine)!=hint) SetWindowTextW(Field(hwnd_,StatusLine),hint);
  RefreshPage();
  Layout();
  if(content_focused || (focus && IsChild(hwnd_,focus) && !IsWindowVisible(focus)))
    SetFocus(Field(hwnd_,compact_?CompactNavigation:Navigation));
}

void PreferencesWindow::RefreshPage() {
  if(page_==0 && actions_.hotkey_label) Set(HotkeyValue,actions_.hotkey_label());
  if(page_==3) {
    if(actions_.diagnostics) Set(Diagnostics,actions_.diagnostics());
    UpdatePractice();
  }
  if(page_==4) Models();
  else if(pending_model_feedback_ && pending_model_feedback_->completion.empty()) PollModelFeedback();
}

void PreferencesWindow::PaintFrame(HDC dc) {
  RECT r{}; GetClientRect(hwnd_,&r);
  FillRect(dc,&r,GetSysColorBrush(COLOR_WINDOW));
  if(compact_) {
    UiFill(dc,{0,0,r.right,UiPx(CompactHeader,dpi_)},colors_.background);
    UiFill(dc,{0,UiPx(CompactHeader,dpi_)-1,r.right,UiPx(CompactHeader,dpi_)},colors_.rule);
  } else {
    const int edge=UiPx(rail_,dpi_);
    UiFill(dc,{0,0,edge,r.bottom},colors_.background);
    UiFill(dc,{edge-1,0,edge,r.bottom},colors_.rule);
    UiFill(dc,{UiPx(20,dpi_),r.bottom-UiPx(48,dpi_),edge-UiPx(20,dpi_),r.bottom-UiPx(48,dpi_)+1},colors_.rule);
  }
  const int foot=r.bottom-UiPx(footer_,dpi_);
  UiFill(dc,{UiPx(rail_,dpi_),foot,r.right,foot+1},colors_.rule);
}

bool PreferencesWindow::ShowAccessKeys() const {
  return always_cues_ || !(SendMessageW(hwnd_,WM_QUERYUISTATE,0,0)&UISF_HIDEACCEL);
}

LRESULT PreferencesWindow::DrawItem(const DRAWITEMSTRUCT& item) {
  HDC dc=item.hDC; const RECT bounds=item.rcItem;
  const int saved=SaveDC(dc);
  SetBkMode(dc,TRANSPARENT);
  if(item.CtlID==Navigation && item.itemID<PageCount) {
    const auto& entry=Pages[item.itemID];
    const bool selected=(item.itemState&ODS_SELECTED)!=0;
    const bool hovered=static_cast<int>(item.itemID)==hover_;
    const bool highlighted=selected || (hovered && colors_.high_contrast);
    UiFill(dc,bounds,colors_.background);
    RECT row=bounds; InflateRect(&row,-UiPx(2,dpi_),-UiPx(2,dpi_));
    const auto fill=selected?colors_.selected:colors_.hover;
    if(selected || hovered) RoundFill(dc,row,UiPx(12,dpi_),fill,fill);
    RECT icon=row; icon.left+=UiPx(13,dpi_); icon.right=icon.left+UiPx(21,dpi_);
    SetTextColor(dc,highlighted?colors_.selected_text:colors_.secondary);
    SelectObject(dc,icons_);
    DrawTextW(dc,&entry.icon,1,&icon,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    RECT text=row; text.left=icon.right+UiPx(12,dpi_);
    SelectObject(dc,font_); SetTextColor(dc,highlighted?colors_.selected_text:colors_.text);
    DrawTextW(dc,entry.title,-1,&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    if((item.itemState&ODS_FOCUS) && !(item.itemState&ODS_NOFOCUSRECT)) { InflateRect(&row,-2,-2); DrawFocusRect(dc,&row); }
  } else if(item.CtlID==BrandMark) {
    UiFill(dc,bounds,colors_.background);
    if(mark_) DrawIconEx(dc,bounds.left,bounds.top,mark_,bounds.right-bounds.left,bounds.bottom-bounds.top,0,nullptr,DI_NORMAL);
  } else if(item.CtlID==Skin && item.itemID<3) {
    const auto skin=static_cast<UiSkin>(item.itemID); const auto swatch=UiPalette(skin);
    const bool selected=(item.itemState&ODS_SELECTED)!=0;
    UiFill(dc,bounds,GetSysColor(selected?COLOR_HIGHLIGHT:COLOR_WINDOW));
    RECT text=bounds; text.left+=UiPx(8,dpi_);
    if(!colors_.high_contrast) {
      const int size=UiPx(14,dpi_),y=(bounds.top+bounds.bottom-size)/2;
      const RECT sample{text.left,y,text.left+size,y+size};
      RoundFill(dc,sample,UiPx(5,dpi_),swatch.background,swatch.border);
      UiFill(dc,{sample.left+UiPx(3,dpi_),y+size/2,sample.right-UiPx(3,dpi_),y+size/2+UiPx(2,dpi_)},swatch.accent);
      text.left=sample.right+UiPx(7,dpi_);
    }
    SetTextColor(dc,selected?GetSysColor(COLOR_HIGHLIGHTTEXT):colors_.text); SelectObject(dc,font_);
    DrawTextW(dc,SkinLabel(skin),-1,&text,DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    if((item.itemState&ODS_FOCUS) && !(item.itemState&ODS_NOFOCUSRECT)) DrawFocusRect(dc,&bounds);
  } else if(item.CtlID==HotkeyValue) {
    UiFill(dc,bounds,GetSysColor(COLOR_WINDOW));
    SelectObject(dc,font_); SetTextColor(dc,colors_.text);
    const auto shortcut=Text(HotkeyValue);
    int x=bounds.left;
    for(size_t start=0;start<shortcut.size();) {
      const auto end=shortcut.find(L" + ",start);
      const auto key=shortcut.substr(start,end==std::wstring::npos?end:end-start);
      SIZE size{}; GetTextExtentPoint32W(dc,key.c_str(),static_cast<int>(key.size()),&size);
      RECT cap{x,bounds.top+UiPx(1,dpi_),std::min(bounds.right,x+size.cx+UiPx(14,dpi_)),bounds.bottom-UiPx(1,dpi_)};
      if(cap.right<=cap.left) break;
      RoundFill(dc,cap,UiPx(7,dpi_),colors_.background,colors_.high_contrast?colors_.border:colors_.rule);
      DrawTextW(dc,key.c_str(),-1,&cap,DT_SINGLELINE|DT_VCENTER|DT_CENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
      if(end==std::wstring::npos) break;
      RECT plus{cap.right,bounds.top,cap.right+UiPx(16,dpi_),bounds.bottom};
      DrawTextW(dc,L"+",1,&plus,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
      x=plus.right; start=end+3;
    }
  } else if(item.CtlID==SkinPreview) {
    const int choice=static_cast<int>(SendMessageW(Field(hwnd_,Skin),CB_GETCURSEL,0,0));
    const auto skin=static_cast<UiSkin>(std::clamp(choice,0,2));
    const auto palette=UiPalette(skin);
    UiFill(dc,bounds,GetSysColor(COLOR_WINDOW));
    const COLORREF backdrop=palette.high_contrast?palette.background:palette.hover;
    RoundFill(dc,bounds,UiPx(16,dpi_),backdrop,palette.high_contrast?palette.border:backdrop);
    RECT legend=bounds; legend.right-=UiPx(15,dpi_); legend.top+=UiPx(10,dpi_);
    SelectObject(dc,meta_); SetTextColor(dc,palette.secondary);
    DrawTextW(dc,L"候选预览",-1,&legend,DT_RIGHT|DT_SINGLELINE|DT_NOPREFIX);
    static const std::vector<std::string> words={"轻音","清音","轻吟","清吟","青音"};
    const int width=bounds.right-bounds.left, gutter=UiPx(6,dpi_), menu=UiPx(52,dpi_);
    const int available=std::max(1,width-UiPx(24,dpi_)-menu-2*gutter);
    const int item_width=std::min(UiPx(64,dpi_),available);
    const int count=std::min(static_cast<int>(words.size()),available/item_width);
    const int h=UiPx(40,dpi_), w=2*gutter+menu+count*item_width;
    const int x=bounds.left+std::min(UiPx(18,dpi_),(width-w)/2), y=bounds.bottom-UiPx(15,dpi_)-h;
    RECT composing{x,y-UiPx(31,dpi_),x+w,y-UiPx(5,dpi_)};
    SelectObject(dc,font_); SetTextColor(dc,palette.text);
    constexpr wchar_t pinyin[]=L"qingyin";
    DrawTextW(dc,pinyin,-1,&composing,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    SIZE word{}; GetTextExtentPoint32W(dc,pinyin,static_cast<int>(std::size(pinyin)-1),&word);
    UiFill(dc,{x,y-UiPx(7,dpi_),x+word.cx,y-UiPx(7,dpi_)+1},palette.accent);
    std::vector<RECT> items;
    for(int i=0;i<count;++i) {
      const int left=gutter+i*item_width;
      items.push_back({left,gutter,left+item_width,h-gutter});
    }
    // The popup normally gets its radius from its HWND. Clip its shared painter
    // to the same outline here, without creating an input window or taking focus.
    BeginPath(dc); RoundRect(dc,x,y,x+w,y+h,UiPx(palette.radius*2,dpi_),UiPx(palette.radius*2,dpi_)); EndPath(dc);
    SelectClipPath(dc,RGN_AND); OffsetViewportOrgEx(dc,x,y,nullptr);
    PaintCandidateStrip(dc,{0,0,w,h},skin,dpi_,items,words,0,-1);
  } else if(item.CtlID>=200 && item.CtlID<224) {
    UiFill(dc,bounds,colors_.high_contrast?colors_.rule:colors_.hover);
  } else if(item.CtlType==ODT_STATIC) {
    // Keep the full mnemonic in the HWND for native dialog navigation and accessibility.
    UiFill(dc,bounds,GetSysColor(COLOR_WINDOW)); SetTextColor(dc,colors_.text);
    SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(item.hwndItem,WM_GETFONT,0,0)));
    RECT text=bounds;
    const auto caption=DisplayCaption(Text(item.CtlID),ShowAccessKeys());
    DrawTextW(dc,caption.c_str(),-1,&text,DT_WORDBREAK|DT_EDITCONTROL);
  }
  RestoreDC(dc,saved);
  return TRUE;
}

LRESULT PreferencesWindow::DrawButton(const NMCUSTOMDRAW& item) {
  if(item.dwDrawStage!=CDDS_PREPAINT) return CDRF_DODEFAULT;
  const int id=static_cast<int>(item.hdr.idFrom);
  HWND button=item.hdr.hwndFrom;
  const bool primary=id==SaveOptions || id==SaveTerm || id==ModelUse || id==PracticeToggle || id==DataBackup;
  const bool accented=primary && !colors_.high_contrast && IsWindowEnabled(button);
  const bool mnemonic=id==SaveOptions || id==ModelUse || id==Learning || id==HotkeyButton;
  const bool show_keys=ShowAccessKeys();
  if(!accented && (!mnemonic || show_keys)) return CDRF_DODEFAULT;
  HDC dc=item.hdc; const int saved=SaveDC(dc);
  UiFill(dc,item.rc,GetSysColor(COLOR_WINDOW));
  RECT text=item.rc;
  COLORREF ink=IsWindowEnabled(button)?GetSysColor(COLOR_BTNTEXT):GetSysColor(COLOR_GRAYTEXT);
  if(accented) {
    const bool pressed=(item.uItemState&CDIS_SELECTED)!=0, hot=(item.uItemState&CDIS_HOT)!=0;
    const auto fill=pressed?colors_.text:hot?colors_.selected_text:colors_.accent;
    RoundFill(dc,item.rc,UiPx(10,dpi_),fill,fill); ink=RGB(255,255,255);
  } else {
    DrawSystemButtonFace(item,id==Learning,dpi_,text);
    if(id==Learning && IsWindowEnabled(button)) ink=colors_.text;
  }
  SetBkMode(dc,TRANSPARENT); SetTextColor(dc,ink); SelectObject(dc,font_);
  const auto state=SendMessageW(button,WM_QUERYUISTATE,0,0);
  const auto caption=DisplayCaption(Text(id),show_keys);
  const UINT align=id==Learning?DT_LEFT:DT_CENTER;
  DrawTextW(dc,caption.c_str(),-1,&text,DT_SINGLELINE|align|DT_VCENTER|(show_keys?0:DT_HIDEPREFIX));
  if(GetFocus()==button && !(state&UISF_HIDEFOCUS)) { InflateRect(&text,-UiPx(4,dpi_),-UiPx(4,dpi_)); DrawFocusRect(dc,&text); }
  RestoreDC(dc,saved);
  return CDRF_SKIPDEFAULT;
}

bool PreferencesWindow::Translate(MSG* message) {
  if(review_window_.Translate(message)) return true;
  if(!hwnd_ || (message->hwnd!=hwnd_ && !IsChild(hwnd_,message->hwnd))) return false;
  if(message->message==WM_SYSCHAR || (message->message==WM_SYSKEYDOWN && message->wParam==VK_MENU))
    SendMessageW(hwnd_,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEACCEL),0);
  else if(!always_cues_ && message->message==WM_LBUTTONDOWN)
    SendMessageW(hwnd_,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEACCEL),0);
  if(message->message==WM_KEYDOWN) {
    if(message->wParam==VK_ESCAPE) {
      for(int id:{Skin,CompactNavigation,ModelChoice,preference::Devices})
        if(SendMessageW(Field(hwnd_,id),CB_GETDROPPEDSTATE,0,0)) return false;
      Close(); return true;
    }
    if(message->wParam==VK_TAB && (GetKeyState(VK_CONTROL)&0x8000)) {
      Page((page_+((GetKeyState(VK_SHIFT)&0x8000)?PageCount-1:1))%PageCount);
      SetFocus(Field(hwnd_,compact_?CompactNavigation:Navigation)); return true;
    }
    if(message->wParam==VK_RETURN) {
      const auto code=SendMessageW(message->hwnd,WM_GETDLGCODE,VK_RETURN,reinterpret_cast<LPARAM>(message));
      if(!(code&(DLGC_WANTALLKEYS|DLGC_DEFPUSHBUTTON|DLGC_UNDEFPUSHBUTTON))) {
        const int id=LOWORD(SendMessageW(hwnd_,DM_GETDEFID,0,0));
        auto action=Field(hwnd_,id);
        if(action && IsWindowEnabled(action)) SendMessageW(action,BM_CLICK,0,0);
        return true;
      }
    }
  }
  const bool handled=IsDialogMessageW(hwnd_,message)!=FALSE;
  if(handled && hwnd_ && (message->message==WM_KEYDOWN || message->message==WM_SYSCHAR)) RevealFocus();
  return handled;
}

LRESULT CALLBACK PreferencesWindow::PaneProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  switch(message) {
    case WM_COMMAND: case WM_NOTIFY: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_NEXTDLGCTL:
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORLISTBOX:
    case WM_VSCROLL: case WM_MOUSEWHEEL:
      return SendMessageW(GetParent(hwnd),message,wp,lp);
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}

LRESULT CALLBACK PreferencesWindow::NavigationProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR ref) {
  auto* self=reinterpret_cast<PreferencesWindow*>(ref);
  if(message==WM_MOUSEMOVE || message==WM_MOUSELEAVE) {
    int hover=-1;
    if(message==WM_MOUSEMOVE) {
      const auto hit=SendMessageW(hwnd,LB_ITEMFROMPOINT,0,lp);
      if(!HIWORD(hit)) hover=LOWORD(hit);
      TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0}; TrackMouseEvent(&track);
    }
    if(hover!=self->hover_) { self->hover_=hover; InvalidateRect(hwnd,nullptr,FALSE); }
  }
  if(message==WM_NCDESTROY) RemoveWindowSubclass(hwnd,NavigationProc,id);
  return DefSubclassProc(hwnd,message,wp,lp);
}

LRESULT CALLBACK PreferencesWindow::Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  auto* self=reinterpret_cast<PreferencesWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    self=static_cast<PreferencesWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
  }
  if(!self) return DefWindowProcW(hwnd,message,wp,lp);
  switch(message) {
    case WM_NEXTDLGCTL: {
      HWND next=lp?reinterpret_cast<HWND>(wp):GetNextDlgTabItem(hwnd,GetFocus(),wp!=0);
      if(next && IsChild(hwnd,next)) { SetFocus(next); self->RevealFocus(); }
      return 0;
    }
    case DM_GETDEFID:
      return MAKELONG(self->page_==1?SaveTerm:self->page_==4?ModelUse:self->page_>=3?IDCANCEL:SaveOptions,DC_HASDEFID);
    case WM_VSCROLL: {
      SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(self->pane_,SB_VERT,&info);
      int next=self->scroll_;
      switch(LOWORD(wp)) {
        case SB_TOP: next=0; break; case SB_BOTTOM: next=info.nMax; break;
        case SB_LINEUP: next-=UiPx(28,self->dpi_); break; case SB_LINEDOWN: next+=UiPx(28,self->dpi_); break;
        case SB_PAGEUP: next-=info.nPage; break; case SB_PAGEDOWN: next+=info.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: next=info.nTrackPos; break;
      }
      self->Scroll(next); return 0;
    }
    case WM_MOUSEWHEEL: self->Scroll(self->scroll_-static_cast<short>(HIWORD(wp))*UiPx(80,self->dpi_)/WHEEL_DELTA); return 0;
    case WM_COMMAND: self->Command(LOWORD(wp),HIWORD(wp)); return 0;
    case WM_MEASUREITEM: {
      auto* item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
      if(item->CtlID==Skin || item->CtlID==Navigation) {
        item->itemHeight=UiPx(item->CtlID==Skin?26:NavigationHeight,self->dpi_); return TRUE;
      }
      break;
    }
    case WM_DRAWITEM: return self->DrawItem(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
    case WM_NOTIFY:
      if(reinterpret_cast<NMHDR*>(lp)->code==NM_CUSTOMDRAW) return self->DrawButton(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
      break;
    case WM_TIMER:
      if(wp==1 && !IsIconic(hwnd)) self->RefreshPage();
      return 0;
    case WM_SIZE:
      self->Layout(); self->RevealFocus(); return 0;
    case WM_UPDATEUISTATE: {
      const auto result=DefWindowProcW(hwnd,message,wp,lp);
      if(HIWORD(wp)&UISF_HIDEACCEL) RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
      return result;
    }
    case WM_GETMINMAXINFO: {
      auto* info=reinterpret_cast<MINMAXINFO*>(lp);
      info->ptMinTrackSize={UiPx(420,self->dpi_),UiPx(310,self->dpi_)}; return 0;
    }
    case WM_DPICHANGED:
      self->dpi_=HIWORD(wp); self->UpdateAppearance(); self->FitWindow(reinterpret_cast<RECT*>(lp));
      self->Layout(); self->RevealFocus(); return 0;
    case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE: case WM_THEMECHANGED:
      if(self->font_) {
        self->UpdateAppearance(); self->Layout(); self->RevealFocus();
        RedrawWindow(hwnd,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
      }
      break;
    case WM_CTLCOLORLISTBOX:
      if(GetDlgCtrlID(reinterpret_cast<HWND>(lp))==Navigation)
        return ControlColor(reinterpret_cast<HDC>(wp),self->colors_.text,self->colors_.background);
      break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
      HWND control=reinterpret_cast<HWND>(lp); const int id=GetDlgCtrlID(control);
      const bool rail=id==BrandName || id==BrandEnglish || id==Version || id==BrandMark;
      const bool output=id==PracticeText || id==ModelState || id==Diagnostics;
      const auto field=std::find_if(self->controls_.begin(),self->controls_.end(),[control](const Control& c){return c.hwnd==control;});
      const bool secondary=field!=self->controls_.end() && (field->face==Face::Detail || field->face==Face::Meta);
      const COLORREF ink=!IsWindowEnabled(control)?GetSysColor(COLOR_GRAYTEXT):secondary?self->colors_.secondary:self->colors_.text;
      return ControlColor(reinterpret_cast<HDC>(wp),ink,rail || output?self->colors_.background:GetSysColor(COLOR_WINDOW));
    }
    case WM_ERASEBKGND: self->PaintFrame(reinterpret_cast<HDC>(wp)); return 1;
    case WM_CLOSE: self->Close(); return 0;
    case WM_NCDESTROY:
      self->actions_.input_code = {};
      self->hwnd_=nullptr;
      break;
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}
