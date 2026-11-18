#include "preferences_window.hpp"
#include "preferences_controls.hpp"
#include "utf8.hpp"
#include "recognition_text.hpp"
#include "product.hpp"
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ctime>
#include <stdexcept>
#include <shobjidl.h>

namespace {
using namespace preference;
constexpr int TermsPerPage=100;
std::wstring ChooseFolder(HWND owner,const wchar_t* title) {
  const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  IFileOpenDialog* dialog=nullptr;std::wstring result;
  if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
    dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_DONTADDTORECENT);
    dialog->SetTitle(title);
    if(SUCCEEDED(dialog->Show(owner))) {
      IShellItem* item=nullptr;
      if(SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){result=path;CoTaskMemFree(path);}
        item->Release();
      }
    }
    dialog->Release();
  }
  if(SUCCEEDED(initialized))CoUninitialize();return result;
}
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

void PreferencesWindow::ReloadInputs(InputPreferences preferences,UiSkin skin) {
  preferences_=preferences;skin_=skin;have_undo_=false;editing_.clear();offset_=0;
  if(!hwnd_) return;
  changing_=true;
  SendMessageW(Field(hwnd_,Terms),LB_SETCURSEL,static_cast<WPARAM>(-1),0);
  Set(Query,L""); Set(Term,L""); Set(Code,L"");
  changing_=false;
  SendMessageW(Field(hwnd_,Learning),BM_SETCHECK,preferences.learning?BST_CHECKED:BST_UNCHECKED,0);
  SendMessageW(Field(hwnd_,Skin),CB_SETCURSEL,static_cast<int>(skin),0);
  Devices();Search();
  InvalidateRect(Field(hwnd_,SkinPreview),nullptr,FALSE);
}
std::wstring PreferencesWindow::Text(int id) const { return WindowText(Field(hwnd_,id)); }
void PreferencesWindow::Set(int id,const std::wstring& text) {
  // Keep selection and scroll position while polling unchanged diagnostic text.
  if(Text(id)==text) return;
  HWND control=Field(hwnd_,id);
  SetWindowTextW(control,text.c_str());
  if(id==StatusLine) Layout();
  else if(id==ModelState || id==PracticeText || id==Diagnostics) FitOutput(id);
  else if((id==ModelDescription && page_==4) || (id==Count && page_==1)) LayoutForm();
  if(id==HotkeyValue) InvalidateRect(control,nullptr,TRUE);
}
void PreferencesWindow::Status(const std::wstring& text) { Set(StatusLine,text); }
ModelSettings PreferencesWindow::SelectedModel() const {
  // Only the engine's validated settings own local paths. A failed folder
  // selection must not poison a later activation from this still-open window.
  auto next=actions_.models?actions_.models():ModelSettings{};
  next.model=static_cast<SpeechModel>(std::clamp<int>(static_cast<int>(SendMessageW(Field(hwnd_,ModelChoice),CB_GETCURSEL,0,0)),0,3));
  next.hotwords=WideToUtf8(Text(ModelHints));return next;
}
void PreferencesWindow::Models() {
  const auto selected=SelectedModel();
  const auto task=actions_.model_task?actions_.model_task():ModelTaskState::Idle;
  const bool busy=task==ModelTaskState::Running;
  const auto focus=GetFocus();
  Set(ModelDescription,SpeechModelDescription(selected.model));
  for(int id:{ModelChoice,ModelUse,ModelLocal,ModelDefault})EnableWindow(Field(hwnd_,id),!busy);
  EnableWindow(Field(hwnd_,ModelCancel),task!=ModelTaskState::Idle);
  EnableWindow(Field(hwnd_,ModelHints),!busy && selected.model==SpeechModel::Qwen3);
  if(page_==4 && focus && IsChild(hwnd_,focus) && !IsWindowEnabled(focus))
    SetFocus(Field(hwnd_,busy?ModelCancel:ModelUse));
  Set(ModelUse,busy?L"正在处理…":L"启用所选模型(&A)");
  if(actions_.model_status) {
    const auto active=actions_.models?actions_.models():ModelSettings{};
    Set(ModelState,std::wstring(L"当前模型：")+SpeechModelLabel(active.model)+L"\r\n"+actions_.model_status(selected.model));
  }
  PollModelFeedback();
}
void PreferencesWindow::PollModelFeedback() {
  if(!pending_model_feedback_) return;
  auto& feedback=*pending_model_feedback_;
  if(feedback.completion.empty()) {
    if(!actions_.model_status || !actions_.model_task || actions_.model_task()!=ModelTaskState::Idle) return;
    feedback.completion=std::wstring(SpeechModelLabel(feedback.model))+L"："+actions_.model_status(feedback.model);
  }
  if(page_==4) {
    auto completion=std::move(feedback.completion);
    pending_model_feedback_.reset();
    Status(completion);
  }
}
const AudioDevice* PreferencesWindow::SelectedDevice() const {
  const auto index=SendMessageW(Field(hwnd_,preference::Devices),CB_GETCURSEL,0,0);
  return index>=0 && index<static_cast<LRESULT>(devices_.size())?&devices_[index]:nullptr;
}
void PreferencesWindow::Devices(bool preserve_selection) {
  const auto selected=preserve_selection?SelectedDevice():nullptr;
  const auto wanted=selected?selected->id:preferences_.microphone;
  devices_=InputDevices();
  if(std::none_of(devices_.begin(),devices_.end(),[&](const auto& device){return device.id==wanted;}))
    devices_.push_back({wanted,L"已选麦克风当前不可用 · 请重新选择",false});
  const auto combo=Field(hwnd_,preference::Devices);
  SendMessageW(combo,CB_RESETCONTENT,0,0);
  for(size_t i=0;i<devices_.size();++i) {
    SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(devices_[i].name.c_str()));
    if(devices_[i].id==wanted) SendMessageW(combo,CB_SETCURSEL,i,0);
  }
}
const Entry* PreferencesWindow::SelectedTerm() const {
  const auto index=SendMessageW(Field(hwnd_,Terms),LB_GETCURSEL,0,0);
  return index>=0 && index<static_cast<LRESULT>(entries_.size())?&entries_[index]:nullptr;
}
void PreferencesWindow::Search() {
  const auto list=Field(hwnd_,Terms);
  const auto focus=GetFocus();
  const bool had_error=!terms_error_.empty();
  const auto query=WideToUtf8(Text(Query));
  int total=0;
  entries_.clear(); terms_error_.clear();
  if(!lexicon_) {
    terms_error_=L"个人词库不可用。请检查本地数据目录并重启轻音输入法。";
  } else {
    try {
      total=lexicon_->Count();
      const int last_offset=std::max(0,total-1)/TermsPerPage*TermsPerPage;
      offset_=std::min(offset_,last_offset);
      for(;;) {
        entries_=lexicon_->Search(query,TermsPerPage+1,offset_);
        if(!entries_.empty() || offset_==0) break;
        offset_-=TermsPerPage;  // The last match on this page may just have been deleted.
      }
    } catch(...) {
      terms_error_=L"暂时无法读取个人词语。请稍后重新搜索或重新打开此页。";
    }
  }
  const bool readable=terms_error_.empty();
  const bool more=entries_.size()>TermsPerPage;
  if(more) entries_.resize(TermsPerPage);
  changing_=true;
  SendMessageW(list,LB_RESETCONTENT,0,0);
  for(const auto& entry:entries_) {
    const auto label=Utf8ToWide(entry.text+"    ·    "+entry.pinyin);
    const auto index=SendMessageW(list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    if(entry.text==editing_) SendMessageW(list,LB_SETCURSEL,index,0);
  }
  const bool selected=SelectedTerm()!=nullptr;
  if(readable && !selected) editing_.clear();
  if(!readable) {
    Set(Count,L"");
    Set(TermsEmpty,lexicon_?L"暂时无法读取个人词语。":L"个人词库不可用。");
    if(page_==1) Status(terms_error_);
  } else {
    Set(Count,entries_.empty()?(query.empty()?L"还没有保存词语":L"没有找到匹配词语"):
        (query.empty()?L"显示 ":L"匹配结果 ")+std::to_wstring(offset_+1)+L"–"+std::to_wstring(offset_+entries_.size())+L"，词库共 "+std::to_wstring(total)+L" 条");
    Set(TermsEmpty,query.empty()?L"在下方填写词语和输入码，即可添加第一个词语。":L"试试更短的词语或输入码。");
    if(had_error && page_==1) Status(L"词语列表已刷新。");
  }
  ShowWindow(list,page_==1 && !entries_.empty()?SW_SHOWNA:SW_HIDE);
  ShowWindow(Field(hwnd_,TermsEmpty),page_==1 && entries_.empty()?SW_SHOWNA:SW_HIDE);
  EnableWindow(Field(hwnd_,Previous),readable && offset_>0); EnableWindow(Field(hwnd_,Next),more);
  EnableWindow(Field(hwnd_,Undo),have_undo_); EnableWindow(Field(hwnd_,DeleteTerm),selected);
  EnableWindow(Field(hwnd_,SaveTerm),readable); changing_=false;
  if(page_==1 && focus && IsChild(pane_,focus) && (!IsWindowVisible(focus) || !IsWindowEnabled(focus))) {
    SetFocus(entries_.empty()?Field(hwnd_,Query):list);
    RevealFocus();
  }
}
void PreferencesWindow::RefreshTerms(const wchar_t* success,const wchar_t* export_failure) {
  Search();
  std::wstring result=!actions_.terms_changed || actions_.terms_changed()?success:export_failure;
  if(!terms_error_.empty()) result+=L"\r\n"+terms_error_;
  Status(result);
}
void PreferencesWindow::Command(int id,int notification) {
  try {
  if((id==Navigation && notification==LBN_SELCHANGE) || (id==CompactNavigation && notification==CBN_SELCHANGE)) {
    const int page=static_cast<int>(SendMessageW(Field(hwnd_,id),id==Navigation?LB_GETCURSEL:CB_GETCURSEL,0,0));
    if(page>=0) Page(page);
    return;
  }
  if(id==Skin && notification==CBN_SELCHANGE) {
    InvalidateRect(Field(hwnd_,SkinPreview),nullptr,FALSE);
    return;
  }
  if(id>=PracticeToggle && id<=PracticeCancel) {
    if(actions_.practice)Status(actions_.practice(id-PracticeToggle));
    if(actions_.practice_status)Set(PracticeText,actions_.practice_status());return;
  }
  if(id==ModelChoice && notification==CBN_SELCHANGE) {Models();return;}
  if(id==ModelIdle && notification==BN_CLICKED) {
    const auto checkbox=Field(hwnd_,ModelIdle);
    const bool enabled=SendMessageW(checkbox,BM_GETCHECK,0,0)==BST_CHECKED;
    if(actions_.save_model_idle_release && actions_.save_model_idle_release(enabled)) {
      Status(enabled?L"已开启模型闲置释放。":L"已关闭模型闲置释放。");
    } else {
      const auto saved=actions_.models?actions_.models():ModelSettings{};
      SendMessageW(checkbox,BM_SETCHECK,saved.idle_minutes>0?BST_CHECKED:BST_UNCHECKED,0);
      Status(L"闲置释放设置保存失败。请检查本地数据目录后重试。");
    }
    return;
  }
  if(id==ModelCancel) {
    pending_model_feedback_.reset();
    if(actions_.cancel_model_operation) actions_.cancel_model_operation();
    Status(L"已请求取消，模型选择保持不变。"); Models(); return;
  }
  if(id==ModelUse || id==ModelLocal || id==ModelDefault) {
    auto next=SelectedModel();
    if(id==ModelDefault) {next.model=SpeechModel::Qwen3;next.directories[0].clear();next.hotwords.clear();}
    if(id==ModelLocal) {
      const auto folder=ChooseFolder(hwnd_,L"选择此模型完整的本地文件夹");if(folder.empty())return;
      next.directories[static_cast<int>(next.model)]=folder;
    }
    if(actions_.use_model) {
      pending_model_feedback_.reset();
      const auto result=actions_.use_model(next);
      if(result.pending) pending_model_feedback_=ModelFeedback{next.model,{}};
      Status(result.message);
      SendMessageW(Field(hwnd_,ModelChoice),CB_SETCURSEL,static_cast<int>(next.model),0);
      Set(ModelHints,Utf8ToWide(next.hotwords));Models();
    }
    return;
  }
  if(id==DataBackup || id==DataRestore) {
    const auto folder=ChooseFolder(hwnd_,id==DataBackup?L"选择保存备份的位置":L"选择轻音备份文件夹");
    if(!folder.empty()) {
      if(id==DataBackup && actions_.backup)Status(actions_.backup(folder));
      if(id==DataRestore && actions_.restore)Status(actions_.restore(folder));
    }
    return;
  }
  if(changing_) return;
    if(id==IDCANCEL) { Close(); return; }
    if(id==Query && notification==EN_CHANGE) { offset_=0; Search(); return; }
    if(id==Terms && notification==LBN_SELCHANGE) {
      const auto selected=SelectedTerm();
      editing_=selected?selected->text:std::string();
      if(selected) { Set(Term,Utf8ToWide(selected->text)); Set(Code,Utf8ToWide(selected->pinyin)); }
      EnableWindow(Field(hwnd_,DeleteTerm),selected!=nullptr);
      return;
    }
    if(notification!=BN_CLICKED && notification!=CBN_SELCHANGE) return;
    if(id==HotkeyButton && actions_.hotkey) actions_.hotkey();
    else if(id==RefreshDevices) { Devices(true); Status(L"设备列表已刷新。选择设备后点击“保存设置”。"); }
    else if(id==Retry && actions_.retry_models) actions_.retry_models();
    else if(id==Previous || id==Next) { offset_=std::max(0,offset_+(id==Next?TermsPerPage:-TermsPerPage)); Search(); }
    else if(id==NewTerm) {
      SendMessageW(Field(hwnd_,Terms),LB_SETCURSEL,static_cast<WPARAM>(-1),0);
      editing_.clear(); Set(Term,L""); Set(Code,L""); SetFocus(Field(hwnd_,Term)); EnableWindow(Field(hwnd_,DeleteTerm),FALSE);
    }
    else if(id==AutoCode && actions_.input_code) { Set(Code,Utf8ToWide(actions_.input_code(WideToUtf8(Text(Term))))); Status(L"请核对输入码；姓名、多音字和缩写可手动调整。"); }
    else if(id==SaveTerm && lexicon_) {
      if(!terms_error_.empty()) { Status(terms_error_); return; }
      const auto selected=SelectedTerm();
      const auto text=WideToUtf8(Text(Term));
      lexicon_->SaveTerm(text,WideToUtf8(Text(Code)),std::time(nullptr),selected?selected->text:std::string());
      editing_=text;
      RefreshTerms(L"词语已保存。下次开始拼音组合时可使用。",L"词语已保存，但候选尚未刷新。请关闭设置后重试保存。");
    } else if(id==DeleteTerm && lexicon_) {
      const auto selected=SelectedTerm();
      if(!selected) return;
      Entry deletion;
      if(!lexicon_->Delete(selected->text,&deletion)) {
        Search();
        std::wstring message=L"这条词语已不在词库中。";
        if(!terms_error_.empty()) message+=L"\r\n"+terms_error_;
        Status(message); return;
      }
      deleted_=std::move(deletion); have_undo_=true;
      editing_.clear(); Set(Term,L""); Set(Code,L"");
      RefreshTerms(L"已删除。可撤销本次删除；普通语音不会自动收回这个词。",L"已删除，但候选刷新失败。请稍后重试保存词语。");
    } else if(id==Undo && lexicon_ && have_undo_) {
      if(!lexicon_->RestoreDeletedTerm(deleted_)) {
        Search();
        std::wstring message=L"同名词语已存在。请先将现有词语改名，再撤销删除。";
        if(!terms_error_.empty()) message+=L"\r\n"+terms_error_;
        Status(message); return;
      }
      have_undo_=false;
      RefreshTerms(L"已恢复删除的词语。",L"词语已恢复，候选刷新失败，请稍后重试。");
    } else if(id==CopyDiagnostics && actions_.diagnostics) Status(CopyUserText(hwnd_,actions_.diagnostics())?L"已复制诊断信息，不包含录音或输入正文。":L"复制失败，请稍后重试。");
    else if(id==Guide) {
      auto next=preferences_; next.guide_dismissed=true;
      if(actions_.save && actions_.save(next)) { preferences_=next; Status(L"已记住。你可以随时回来查看使用方法。"); }
      else Status(L"未能保存，请稍后重试。");
    } else if(id==SaveOptions) {
      InputPreferences next=preferences_;
      next.learning=SendMessageW(Field(hwnd_,Learning),BM_GETCHECK,0,0)==BST_CHECKED;
      Devices(true);
      const auto device=SelectedDevice();
      if(device && device->available) next.microphone=device->id;
      else if(page_==2) {
        Status(L"已选麦克风当前不可用。请重新选择设备后保存。");
        SetFocus(Field(hwnd_,preference::Devices)); RevealFocus(); return;
      }
      if(!actions_.save || !actions_.save(next)) { Status(L"保存失败。请检查本地数据目录是否可写，再重试。"); return; }
      preferences_=next;
      const auto skin=static_cast<UiSkin>(SendMessageW(Field(hwnd_,Skin),CB_GETCURSEL,0,0));
      if(actions_.skin && !actions_.skin(skin)) { Status(L"输入设置已保存，外观保存失败。请重试。"); return; }
      skin_=skin; Status(L"已保存。关闭设置后即可继续输入。");
    }
  } catch(const std::invalid_argument& error) { Status(Utf8ToWide(error.what())); }
    catch(...) { Status(L"操作未完成，请稍后重试。原有内容会保留。"); }
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
