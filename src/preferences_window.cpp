#include "preferences_window.hpp"
#include "utf8.hpp"
#include "recognition_text.hpp"
#include "product.hpp"
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ctime>
#include <stdexcept>

namespace {
enum { Tabs=10,StatusLine=11,Learning=20,HotkeyButton=21,Skin=22,SaveOptions=23,
  Query=30,Terms=31,Term=32,Code=33,AutoCode=34,SaveTerm=35,NewTerm=36,DeleteTerm=37,Undo=38,Previous=39,Next=40,Count=41,
  Devices=50,RefreshDevices=51,Pause=52,Retry=53,Diagnostics=54,CopyDiagnostics=55,Guide=56,ChineseOnly=57 };
std::wstring WindowText(HWND hwnd) {
  const int length=GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length)+1,L'\0');
  const int read=GetWindowTextW(hwnd,text.data(),length+1); text.resize(std::max(0,read)); return text;
}
}

std::vector<AudioDevice> InputDevices() {
  std::vector<AudioDevice> result{{L"",L"跟随系统默认麦克风"}};
  const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  IMMDeviceEnumerator* enumerator=nullptr; IMMDeviceCollection* collection=nullptr;
  if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))) {
    if(SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&collection))) {
      UINT count=0; collection->GetCount(&count);
      for(UINT i=0;i<count;++i) {
        IMMDevice* device=nullptr;
        if(FAILED(collection->Item(i,&device))) continue;
        LPWSTR id=nullptr; IPropertyStore* properties=nullptr;
        if(SUCCEEDED(device->GetId(&id))) {
          std::wstring name=L"麦克风";
          if(SUCCEEDED(device->OpenPropertyStore(STGM_READ,&properties))) {
            PROPVARIANT value; PropVariantInit(&value);
            if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value)) && value.vt==VT_LPWSTR) name=value.pwszVal;
            PropVariantClear(&value); properties->Release();
          }
          result.push_back({id,name}); CoTaskMemFree(id);
        }
        device->Release();
      }
      collection->Release();
    }
    enumerator->Release();
  }
  if(SUCCEEDED(initialized)) CoUninitialize();
  return result;
}

bool CopyUserText(HWND owner,const std::wstring& text) {
  if(text.empty()) return false;
  HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
  if(!data) return false;
  auto* memory=GlobalLock(data);
  if(!memory) { GlobalFree(data); return false; }
  memcpy(memory,text.c_str(),(text.size()+1)*sizeof(wchar_t)); GlobalUnlock(data);
  if(!OpenClipboard(owner)) { GlobalFree(data); return false; }
  bool ok=EmptyClipboard()!=0 && SetClipboardData(CF_UNICODETEXT,data)!=nullptr;
  CloseClipboard(); if(!ok) GlobalFree(data); return ok;
}

PreferencesWindow::~PreferencesWindow() { Close(); }
void PreferencesWindow::Close() {
  if(hwnd_) DestroyWindow(hwnd_); hwnd_=nullptr; controls_.clear();
  if(font_) DeleteObject(font_); font_=nullptr;
  if(heading_) DeleteObject(heading_); heading_=nullptr;
  if(brush_) DeleteObject(brush_); brush_=nullptr;
}
HWND PreferencesWindow::Add(const wchar_t* type,const wchar_t* text,int id,int page,int x,int y,int w,int h,DWORD style) {
  const bool edit=lstrcmpW(type,L"EDIT")==0;
  HWND control=CreateWindowExW(edit?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,
    0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
  controls_.push_back({control,page,x,y,w,h}); return control;
}
void PreferencesWindow::Open(Lexicon* lexicon,InputPreferences preferences,UiSkin skin,PreferencesActions actions,int page) {
  if(hwnd_) { Page(page); SetForegroundWindow(hwnd_); return; }
  UiDpiScope scope;
  lexicon_=lexicon; preferences_=preferences; skin_=skin; actions_=std::move(actions);
  offset_=0; editing_.clear(); have_undo_=false;
  INITCOMMONCONTROLSEX common{sizeof(common),ICC_TAB_CLASSES}; InitCommonControlsEx(&common);
  WNDCLASSW klass{}; klass.lpfnWndProc=Proc; klass.hInstance=GetModuleHandleW(nullptr);
  klass.lpszClassName=L"SimpleVoicePreferences"; klass.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassW(&klass);
  hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,klass.lpszClassName,L"轻音输入法 · 设置",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_VSCROLL,
    CW_USEDEFAULT,CW_USEDEFAULT,700,650,nullptr,nullptr,klass.hInstance,this);
  if(!hwnd_) return;
  dpi_=UiDpi(hwnd_);
  Add(L"STATIC",L"轻音输入法",12,-1,24,20,630,32);
  Add(L"STATIC",L"Simple Voice  ·  本地识别，确认后写入",13,-1,24,60,630,24);
  HWND tabs=Add(WC_TABCONTROLW,L"",Tabs,-1,24,98,632,34,WS_TABSTOP);
  const wchar_t* titles[]={L"输入与外观",L"我的词语",L"语音与设备",L"使用与诊断"};
  for(int i=0;i<4;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(titles[i]); SendMessageW(tabs,TCM_INSERTITEMW,i,reinterpret_cast<LPARAM>(&item)); }
  Add(L"BUTTON",L"记住拼音选词习惯(&L)",Learning,0,28,162,570,30,WS_TABSTOP|BS_AUTOCHECKBOX);
  SendDlgItemMessageW(hwnd_,Learning,BM_SETCHECK,preferences.learning?BST_CHECKED:BST_UNCHECKED,0);
  Add(L"STATIC",L"关闭后不读取或新增后台选词偏好。基础词表和“我的词语”仍可用。",101,0,28,200,608,48);
  Add(L"BUTTON",L"修改语音快捷键(&K)",HotkeyButton,0,28,270,230,34,WS_TABSTOP);
  Add(L"STATIC",L"输入浮层外观(&S)",102,0,28,336,200,24);
  HWND skins=Add(L"COMBOBOX",L"",Skin,0,28,368,290,160,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
  for(auto choice:{UiSkin::Titanium,UiSkin::Daylight,UiSkin::DeepSpace}) SendMessageW(skins,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(SkinLabel(choice)));
  SendMessageW(skins,CB_SETCURSEL,static_cast<int>(skin),0);
  Add(L"STATIC",L"空格选词 · Enter 保留原始输入 · 单按 Shift 切换中英文\n组合内可用方向键编辑；Ctrl + Delete 遗忘选中的学习候选。",103,0,28,434,608,58);
  Add(L"STATIC",L"搜索词语或输入码(&F)",104,1,28,150,600,22);
  Add(L"EDIT",L"",Query,1,28,178,608,30,WS_TABSTOP|ES_AUTOHSCROLL);
  Add(L"LISTBOX",L"",Terms,1,28,218,608,148,WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT);
  Add(L"STATIC",L"",Count,1,28,374,338,22);
  Add(L"BUTTON",L"上一页",Previous,1,382,370,116,30,WS_TABSTOP);
  Add(L"BUTTON",L"下一页",Next,1,510,370,126,30,WS_TABSTOP);
  Add(L"STATIC",L"词语 / 常用语(&W)",105,1,28,408,250,22);
  Add(L"EDIT",L"",Term,1,28,436,300,30,WS_TABSTOP|ES_AUTOHSCROLL);
  Add(L"STATIC",L"输入码(&C)",106,1,340,408,290,22);
  Add(L"EDIT",L"",Code,1,340,436,180,30,WS_TABSTOP|ES_AUTOHSCROLL);
  SendDlgItemMessageW(hwnd_,Term,EM_SETLIMITTEXT,256,0); SendDlgItemMessageW(hwnd_,Code,EM_SETLIMITTEXT,256,0);
  Add(L"BUTTON",L"生成输入码",AutoCode,1,530,436,106,30,WS_TABSTOP);
  Add(L"BUTTON",L"保存词语",SaveTerm,1,28,480,114,32,WS_TABSTOP);
  Add(L"BUTTON",L"新增",NewTerm,1,154,480,102,32,WS_TABSTOP);
  Add(L"BUTTON",L"删除",DeleteTerm,1,268,480,102,32,WS_TABSTOP);
  Add(L"BUTTON",L"撤销删除",Undo,1,382,480,126,32,WS_TABSTOP);
  Add(L"STATIC",L"只显示你主动保存的内容。365 天未使用会删除；输入码使用小写英文字母。",107,1,28,522,608,44);
  Add(L"STATIC",L"麦克风(&M)",108,2,28,160,600,24);
  Add(L"COMBOBOX",L"",::Devices,2,28,194,470,240,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
  Add(L"BUTTON",L"刷新设备",RefreshDevices,2,510,194,126,32,WS_TABSTOP);
  Add(L"STATIC",L"停顿多久后开始校准(&P)",109,2,28,260,608,24);
  HWND pauses=Add(L"COMBOBOX",L"",Pause,2,28,294,290,170,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
  const wchar_t* pause_names[]={L"0.5 秒 · 快速",L"0.8 秒 · 默认",L"1.2 秒 · 从容",L"2 秒 · 长句"};
  const int pauses_ms[]={500,800,1200,2000};
  for(int i=0;i<4;++i) { SendMessageW(pauses,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(pause_names[i])); if(preferences.pause_ms==pauses_ms[i]) SendMessageW(pauses,CB_SETCURSEL,i,0); }
  if(SendMessageW(pauses,CB_GETCURSEL,0,0)==CB_ERR) SendMessageW(pauses,CB_SETCURSEL,1,0);
  Add(L"BUTTON",L"中文优先（不适合英语或中英混说）",ChineseOnly,2,28,344,608,30,WS_TABSTOP|BS_AUTOCHECKBOX);
  SendDlgItemMessageW(hwnd_,ChineseOnly,BM_SETCHECK,preferences.chinese_only?BST_CHECKED:BST_UNCHECKED,0);
  Add(L"STATIC",L"录音只在你启动语音时开启，不上传、不保存音频。\n停顿后先校准并显示最终文本，确认无误后按 Enter 写入。\n识别不准确时，可在预览中编辑、恢复识别原文或复制。",110,2,28,392,608,92);
  Add(L"BUTTON",L"重新准备语音模型",Retry,2,28,510,242,34,WS_TABSTOP);
  Add(L"STATIC",L"1  在可输入文字的位置，按语音快捷键开始。\n2  等待“正在听”后说话；停顿后自动校准。\n3  核对最终预览；Enter 写入，Esc 取消。\n\n录音中按 Enter 会先停录预览，再按一次才写入。\n右键预览可编辑、复制或主动保存个人词语。",111,3,28,156,608,174);
  Add(L"EDIT",L"",Diagnostics,3,28,348,608,134,WS_TABSTOP|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL);
  Add(L"BUTTON",L"复制诊断信息",CopyDiagnostics,3,28,496,202,34,WS_TABSTOP);
  Add(L"BUTTON",L"已了解使用方法",Guide,3,246,496,202,34,WS_TABSTOP);
  Add(L"STATIC",L"",StatusLine,-1,28,576,608,46);
  Add(L"BUTTON",L"保存设置(&A)",SaveOptions,-1,392,632,142,34,WS_TABSTOP|BS_DEFPUSHBUTTON);
  Add(L"BUTTON",L"关闭",IDCANCEL,-1,546,632,90,34,WS_TABSTOP);
  Devices(); Search(); Layout(); Page(page);
  if(actions_.diagnostics) Set(Diagnostics,actions_.diagnostics());
  SetTimer(hwnd_,1,1000,nullptr);
  ShowWindow(hwnd_,SW_SHOWNORMAL); SetForegroundWindow(hwnd_); SetFocus(tabs);
}
void PreferencesWindow::Layout() {
  if(font_) DeleteObject(font_); if(heading_) DeleteObject(heading_); if(brush_) DeleteObject(brush_);
  font_=UiFont(14,dpi_); heading_=UiFont(23,dpi_,true); brush_=CreateSolidBrush(UiPalette(UiSkin::Titanium).background);
  RECT desired{0,0,UiPx(680,dpi_),UiPx(690,dpi_)};
  AdjustWindowRectEx(&desired,static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_STYLE)),FALSE,static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_EXSTYLE)));
  RECT position{}; GetWindowRect(hwnd_,&position);
  const auto work=UiWorkArea({position.left,position.top,1,1});
  const auto placed=ClampRect({position.left,position.top,desired.right-desired.left,desired.bottom-desired.top},work);
  SetWindowPos(hwnd_,nullptr,placed.x,placed.y,placed.w,placed.h,SWP_NOZORDER|SWP_NOACTIVATE);
  RECT client{}; GetClientRect(hwnd_,&client);
  SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,UiPx(690,dpi_)-1,static_cast<UINT>(client.bottom),scroll_,0};
  SetScrollInfo(hwnd_,SB_VERT,&info,TRUE); scroll_=GetScrollPos(hwnd_,SB_VERT);
  for(const auto& control:controls_) {
    SendMessageW(control.hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(GetDlgCtrlID(control.hwnd)==12?heading_:font_),FALSE);
    MoveWindow(control.hwnd,UiPx(control.x,dpi_),UiPx(control.y,dpi_)-scroll_,UiPx(control.w,dpi_),UiPx(control.h,dpi_),TRUE);
  }
  InvalidateRect(hwnd_,nullptr,TRUE);
}
void PreferencesWindow::Scroll(int position) {
  SetScrollPos(hwnd_,SB_VERT,position,TRUE); scroll_=GetScrollPos(hwnd_,SB_VERT);
  for(const auto& control:controls_) MoveWindow(control.hwnd,UiPx(control.x,dpi_),UiPx(control.y,dpi_)-scroll_,UiPx(control.w,dpi_),UiPx(control.h,dpi_),TRUE);
}
void PreferencesWindow::Page(int page) {
  page_=std::clamp(page,0,3); TabCtrl_SetCurSel(GetDlgItem(hwnd_,Tabs),page_);
  Scroll(0);
  for(const auto& control:controls_) ShowWindow(control.hwnd,control.page==-1 || control.page==page_?SW_SHOW:SW_HIDE);
  ShowWindow(GetDlgItem(hwnd_,SaveOptions),page_==0 || page_==2?SW_SHOW:SW_HIDE);
  Status(page_==1?L"基础词表和选词统计由系统维护，不需要逐条清理。":L"设置保存后生效；麦克风和停顿时长在下一次录音使用。");
}
std::wstring PreferencesWindow::Text(int id) const { return WindowText(GetDlgItem(hwnd_,id)); }
void PreferencesWindow::Set(int id,const std::wstring& text) { SetDlgItemTextW(hwnd_,id,text.c_str()); }
void PreferencesWindow::Status(const std::wstring& text) { Set(StatusLine,text); }
void PreferencesWindow::Devices() {
  devices_=InputDevices();
  if(!preferences_.microphone.empty() && std::none_of(devices_.begin(),devices_.end(),[&](const auto& d){return d.id==preferences_.microphone;}))
    devices_.push_back({preferences_.microphone,L"已选麦克风当前不可用 · 请重新选择"});
  SendDlgItemMessageW(hwnd_,::Devices,CB_RESETCONTENT,0,0);
  for(size_t i=0;i<devices_.size();++i) {
    SendDlgItemMessageW(hwnd_,::Devices,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(devices_[i].name.c_str()));
    if(devices_[i].id==preferences_.microphone) SendDlgItemMessageW(hwnd_,::Devices,CB_SETCURSEL,i,0);
  }
}
void PreferencesWindow::Search() {
  changing_=true; entries_.clear();
  SendDlgItemMessageW(hwnd_,Terms,LB_RESETCONTENT,0,0);
  try {
    if(lexicon_) entries_=lexicon_->Search(WideToUtf8(Text(Query)),101,offset_);
    const bool more=entries_.size()>100; if(more) entries_.resize(100);
    for(const auto& entry:entries_) {
      const auto label=Utf8ToWide(entry.text+"    ·    "+entry.pinyin);
      SendDlgItemMessageW(hwnd_,Terms,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    }
    Set(Count,entries_.empty()?(Text(Query).empty()?L"还没有保存词语":L"没有找到匹配词语"):
        (Text(Query).empty()?L"显示 ":L"匹配结果 ")+std::to_wstring(offset_+1)+L"–"+std::to_wstring(offset_+entries_.size())+L"，词库共 "+std::to_wstring(lexicon_->Count())+L" 条");
    EnableWindow(GetDlgItem(hwnd_,Previous),offset_>0); EnableWindow(GetDlgItem(hwnd_,Next),more);
  } catch(...) { Status(L"暂时无法读取个人词语。请稍后重试；基础输入仍可用。"); }
  EnableWindow(GetDlgItem(hwnd_,Undo),have_undo_); EnableWindow(GetDlgItem(hwnd_,DeleteTerm),FALSE);
  EnableWindow(GetDlgItem(hwnd_,SaveTerm),lexicon_!=nullptr); changing_=false;
}
void PreferencesWindow::Command(int id,int notification) {
  if(changing_) return;
  try {
    if(id==IDCANCEL) { Close(); return; }
    if(id==Query && notification==EN_CHANGE) { offset_=0; Search(); return; }
    if(id==Terms && notification==LBN_SELCHANGE) {
      const auto selected=SendDlgItemMessageW(hwnd_,Terms,LB_GETCURSEL,0,0);
      if(selected>=0 && selected<static_cast<LRESULT>(entries_.size())) {
        const auto& entry=entries_[selected]; editing_=entry.text;
        Set(Term,Utf8ToWide(entry.text)); Set(Code,Utf8ToWide(entry.pinyin)); EnableWindow(GetDlgItem(hwnd_,DeleteTerm),TRUE);
      } return;
    }
    if(notification!=BN_CLICKED && notification!=CBN_SELCHANGE) return;
    if(id==HotkeyButton && actions_.hotkey) actions_.hotkey();
    else if(id==RefreshDevices) { Devices(); Status(L"设备列表已刷新。选择设备后点击“保存设置”。"); }
    else if(id==Retry && actions_.retry_models) actions_.retry_models();
    else if(id==Previous || id==Next) { offset_=std::max(0,offset_+(id==Next?100:-100)); Search(); }
    else if(id==NewTerm) { editing_.clear(); Set(Term,L""); Set(Code,L""); SetFocus(GetDlgItem(hwnd_,Term)); EnableWindow(GetDlgItem(hwnd_,DeleteTerm),FALSE); }
    else if(id==AutoCode && actions_.input_code) { Set(Code,Utf8ToWide(actions_.input_code(WideToUtf8(Text(Term))))); Status(L"请核对输入码；姓名、多音字和缩写可手动调整。"); }
    else if(id==SaveTerm && lexicon_) {
      lexicon_->SaveTerm(WideToUtf8(Text(Term)),WideToUtf8(Text(Code)),std::time(nullptr),editing_);
      editing_=WideToUtf8(Text(Term)); Search();
      Status(!actions_.terms_changed || actions_.terms_changed()?L"词语已保存。下次开始拼音组合时可使用。":L"词语已保存，但候选尚未刷新。请关闭设置后重试保存。");
    } else if(id==DeleteTerm && lexicon_ && !editing_.empty()) {
      auto found=lexicon_->Search(editing_,10000);
      for(const auto& row:found) if(row.text==editing_) { deleted_=row; have_undo_=lexicon_->Delete(editing_); break; }
      editing_.clear(); Set(Term,L""); Set(Code,L""); Search();
      const bool exported=!actions_.terms_changed || actions_.terms_changed();
      Status(exported?L"已删除。可撤销本次删除；普通语音不会自动收回这个词。":L"已删除，但候选刷新失败。请稍后重试保存词语。");
    } else if(id==Undo && lexicon_ && have_undo_) {
      lexicon_->SaveTerm(deleted_.text,deleted_.pinyin,std::time(nullptr)); have_undo_=false; Search();
      Status(!actions_.terms_changed || actions_.terms_changed()?L"已恢复删除的词语。":L"词语已恢复，候选刷新失败，请稍后重试。");
    } else if(id==CopyDiagnostics && actions_.diagnostics) Status(CopyUserText(hwnd_,actions_.diagnostics())?L"已复制诊断信息，不包含录音或输入正文。":L"复制失败，请稍后重试。");
    else if(id==Guide) {
      auto next=preferences_; next.guide_dismissed=true;
      if(actions_.save && actions_.save(next)) { preferences_=next; Status(L"已记住。你可以随时回来查看使用方法。"); }
      else Status(L"未能保存，请稍后重试。");
    } else if(id==SaveOptions) {
      InputPreferences next=preferences_;
      next.chinese_only=SendDlgItemMessageW(hwnd_,ChineseOnly,BM_GETCHECK,0,0)==BST_CHECKED;
      next.learning=SendDlgItemMessageW(hwnd_,Learning,BM_GETCHECK,0,0)==BST_CHECKED;
      const auto device=SendDlgItemMessageW(hwnd_,::Devices,CB_GETCURSEL,0,0);
      if(device>=0 && device<static_cast<LRESULT>(devices_.size())) next.microphone=devices_[device].id;
      const int values[]={500,800,1200,2000};
      const int pause=static_cast<int>(SendDlgItemMessageW(hwnd_,Pause,CB_GETCURSEL,0,0)); next.pause_ms=values[std::clamp(pause,0,3)];
      if(!actions_.save || !actions_.save(next)) { Status(L"保存失败。请检查本地数据目录是否可写，再重试。"); return; }
      preferences_=next;
      const auto skin=static_cast<UiSkin>(SendDlgItemMessageW(hwnd_,Skin,CB_GETCURSEL,0,0));
      if(actions_.skin && !actions_.skin(skin)) { Status(L"输入设置已保存，外观保存失败。请重试。"); return; }
      skin_=skin; Layout(); Status(L"已保存。关闭设置后即可继续输入。");
    }
  } catch(const std::invalid_argument& error) { Status(Utf8ToWide(error.what())); }
    catch(...) { Status(L"操作未完成，请稍后重试。原有内容会保留。"); }
}
bool PreferencesWindow::Translate(MSG* message) {
  if(!hwnd_ || (message->hwnd!=hwnd_ && !IsChild(hwnd_,message->hwnd))) return false;
  if(message->message==WM_KEYDOWN && message->wParam==VK_ESCAPE) { Close(); return true; }
  if(message->message==WM_KEYDOWN && message->wParam==VK_TAB && (GetKeyState(VK_CONTROL)&0x8000)) {
    Page((page_+((GetKeyState(VK_SHIFT)&0x8000)?3:1))%4); SetFocus(GetDlgItem(hwnd_,Tabs)); return true;
  }
  const bool handled=IsDialogMessageW(hwnd_,message)!=FALSE;
  if(handled && message->message==WM_KEYDOWN && message->wParam==VK_TAB) {
    HWND focus=GetFocus();
    if(focus && IsChild(hwnd_,focus)) {
      RECT rect{},client{}; GetWindowRect(focus,&rect); MapWindowPoints(nullptr,hwnd_,reinterpret_cast<POINT*>(&rect),2); GetClientRect(hwnd_,&client);
      if(rect.top<0) Scroll(scroll_+rect.top-UiPx(8,dpi_));
      else if(rect.bottom>client.bottom) Scroll(scroll_+rect.bottom-client.bottom+UiPx(8,dpi_));
    }
  }
  return handled;
}
LRESULT CALLBACK PreferencesWindow::Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  auto* self=reinterpret_cast<PreferencesWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if(message==WM_NCCREATE) { self=static_cast<PreferencesWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); self->hwnd_=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
  if(self) {
    if(message==DM_GETDEFID) return MAKELONG(self->page_==1?SaveTerm:self->page_==3?IDCANCEL:SaveOptions,DC_HASDEFID);
    if(message==WM_VSCROLL) {
      SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(hwnd,SB_VERT,&info); int next=self->scroll_;
      switch(LOWORD(wp)) { case SB_TOP: next=0; break; case SB_BOTTOM: next=info.nMax; break;
        case SB_LINEUP: next-=UiPx(28,self->dpi_); break; case SB_LINEDOWN: next+=UiPx(28,self->dpi_); break;
        case SB_PAGEUP: next-=info.nPage; break; case SB_PAGEDOWN: next+=info.nPage; break;
        case SB_THUMBTRACK: next=info.nTrackPos; break; }
      self->Scroll(next); return 0;
    }
    if(message==WM_MOUSEWHEEL) { self->Scroll(self->scroll_-static_cast<short>(HIWORD(wp))*UiPx(84,self->dpi_)/WHEEL_DELTA); return 0; }
    if(message==WM_COMMAND) { self->Command(LOWORD(wp),HIWORD(wp)); return 0; }
    if(message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->idFrom==Tabs && reinterpret_cast<NMHDR*>(lp)->code==TCN_SELCHANGE) { self->Page(TabCtrl_GetCurSel(GetDlgItem(hwnd,Tabs))); return 0; }
    if(message==WM_TIMER && self->actions_.diagnostics) { self->Set(Diagnostics,self->actions_.diagnostics()); return 0; }
    if(message==WM_DPICHANGED) { self->dpi_=HIWORD(wp); self->Layout(); return 0; }
    if(message==WM_SETTINGCHANGE) { self->Layout(); return 0; }
    if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORDLG) {
      auto dc=reinterpret_cast<HDC>(wp); auto palette=UiPalette(UiSkin::Titanium);
      SetTextColor(dc,palette.text); SetBkColor(dc,palette.background); return reinterpret_cast<LRESULT>(self->brush_);
    }
    if(message==WM_ERASEBKGND) { RECT rect{}; GetClientRect(hwnd,&rect); FillRect(reinterpret_cast<HDC>(wp),&rect,self->brush_?self->brush_:GetSysColorBrush(COLOR_WINDOW)); return 1; }
    if(message==WM_CLOSE) { self->Close(); return 0; }
    if(message==WM_NCDESTROY) self->hwnd_=nullptr;
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}

PreviewEditor::~PreviewEditor() { Close(); }
void PreviewEditor::Close() { if(hwnd_) DestroyWindow(hwnd_); hwnd_=nullptr; if(font_) DeleteObject(font_); font_=nullptr; }
void PreviewEditor::Open(const std::string& current,const std::string& original,std::function<void(std::string)> save,bool insert_uncertain) {
  if(hwnd_) { SetForegroundWindow(hwnd_); return; }
  UiDpiScope scope; original_=original; on_save_=std::move(save);
  WNDCLASSW klass{}; klass.lpfnWndProc=Proc; klass.hInstance=GetModuleHandleW(nullptr); klass.lpszClassName=L"SimpleVoicePreviewEditor";
  klass.hbrBackground=GetSysColorBrush(COLOR_WINDOW); klass.hCursor=LoadCursor(nullptr,IDC_ARROW); RegisterClassW(&klass);
  hwnd_=CreateWindowExW(WS_EX_CONTROLPARENT,klass.lpszClassName,L"编辑语音预览",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,
    CW_USEDEFAULT,CW_USEDEFAULT,700,440,nullptr,nullptr,klass.hInstance,this);
  if(!hwnd_) return;
  dpi_=UiDpi(hwnd_);
  SetWindowPos(hwnd_,nullptr,0,0,UiPx(640,dpi_),UiPx(400,dpi_),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
  auto add=[&](const wchar_t* type,const wchar_t* text,int id,DWORD style) { return CreateWindowExW(type==std::wstring(L"EDIT")?WS_EX_CLIENTEDGE:0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,0,0,hwnd_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),klass.hInstance,nullptr); };
  edit_=add(L"EDIT",Utf8ToWide(current).c_str(),10,WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL);
  SendMessageW(edit_,EM_SETLIMITTEXT,16000,0);
  status_=add(L"STATIC",insert_uncertain?L"上次写入结果待核实，请先核对正文。保存预览不会确认写入结果。":L"保存后回到输入位置，按 Enter 写入。Ctrl + Enter 保存，Esc 取消编辑。",11,0);
  restore_=add(L"BUTTON",L"恢复识别原文(&R)",12,WS_TABSTOP);
  copy_=add(L"BUTTON",L"复制(&C)",13,WS_TABSTOP);
  save_=add(L"BUTTON",L"保存预览(&S)",IDOK,WS_TABSTOP|BS_DEFPUSHBUTTON);
  Layout(); ShowWindow(hwnd_,SW_SHOWNORMAL); SetForegroundWindow(hwnd_); SetFocus(edit_);
}
void PreviewEditor::Layout() {
  if(!edit_) return;
  if(font_) DeleteObject(font_); font_=UiFont(15,dpi_);
  RECT rect{}; GetClientRect(hwnd_,&rect); int pad=UiPx(20,dpi_),button=UiPx(34,dpi_),bottom=rect.bottom-pad;
  for(HWND control:{edit_,status_,restore_,copy_,save_}) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
  MoveWindow(edit_,pad,pad,std::max(1L,rect.right-2*pad),std::max(1L,rect.bottom-UiPx(132,dpi_)),TRUE);
  MoveWindow(status_,pad,rect.bottom-UiPx(100,dpi_),rect.right-2*pad,UiPx(44,dpi_),TRUE);
  MoveWindow(restore_,pad,bottom-button,UiPx(170,dpi_),button,TRUE);
  MoveWindow(copy_,pad+UiPx(182,dpi_),bottom-button,UiPx(86,dpi_),button,TRUE);
  MoveWindow(save_,rect.right-pad-UiPx(140,dpi_),bottom-button,UiPx(140,dpi_),button,TRUE);
}
bool PreviewEditor::Translate(MSG* message) {
  if(!hwnd_ || (message->hwnd!=hwnd_ && !IsChild(hwnd_,message->hwnd))) return false;
  if(message->message==WM_KEYDOWN && message->wParam==VK_ESCAPE) { Close(); return true; }
  if(message->message==WM_KEYDOWN && message->wParam==VK_RETURN && (GetKeyState(VK_CONTROL)&0x8000)) { SendMessageW(hwnd_,WM_COMMAND,IDOK,0); return true; }
  return IsDialogMessageW(hwnd_,message)!=FALSE;
}
LRESULT CALLBACK PreviewEditor::Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  auto* self=reinterpret_cast<PreviewEditor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if(message==WM_NCCREATE) { self=static_cast<PreviewEditor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); self->hwnd_=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self)); }
  if(self) {
    if(message==WM_NEXTDLGCTL) {
      // Multiline EDIT forwards Tab here. DefWindowProc is not a dialog proc.
      const HWND next=lp?reinterpret_cast<HWND>(wp):GetNextDlgTabItem(hwnd,GetFocus(),wp!=0);
      if(next && IsChild(hwnd,next)) SetFocus(next);
      return 0;
    }
    if(message==WM_COMMAND) {
      switch(LOWORD(wp)) {
        case 12: SetWindowTextW(self->edit_,Utf8ToWide(self->original_).c_str()); SetWindowTextW(self->status_,L"已恢复识别原文。核对后保存，再回到输入位置确认写入。"); break;
        case 13: SetWindowTextW(self->status_,CopyUserText(hwnd,WindowText(self->edit_))?L"已复制预览文字。":L"复制失败，请稍后重试。"); break;
        case IDOK: {
          auto text=WideToUtf8(WindowText(self->edit_));
          if(text.empty() || HasUnknownSpeech(text)) { SetWindowTextW(self->status_,L"请补全无法识别的内容，再保存预览。"); break; }
          if(self->on_save_) self->on_save_(text); self->Close(); break;
        }
        case IDCANCEL: self->Close(); break;
      } return 0;
    }
    if(message==WM_SIZE) { self->Layout(); return 0; }
    if(message==WM_GETMINMAXINFO) { auto* info=reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize={UiPx(560,self->dpi_),UiPx(330,self->dpi_)}; return 0; }
    if(message==WM_DPICHANGED) { self->dpi_=HIWORD(wp); const auto* rect=reinterpret_cast<RECT*>(lp); SetWindowPos(hwnd,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOZORDER|SWP_NOACTIVATE); self->Layout(); return 0; }
    if(message==WM_CLOSE) { self->Close(); return 0; }
    if(message==WM_NCDESTROY) self->hwnd_=nullptr;
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}
