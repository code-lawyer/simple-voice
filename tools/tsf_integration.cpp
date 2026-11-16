// Exercise the real service against Windows TSF and a disposable ITextStoreACP.
// Keys are dispatched directly to the production sink; no global input or
// registration changes are made. The compile-time QA reader is isolated.
#include "../src/tsf_service.cpp"
#include <textstor.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <olectl.h>
#include "brand_icon.hpp"

HINSTANCE VoiceImeModule() { return GetModuleHandleW(nullptr); }

namespace {
int checks = 0;
void Check(bool ok, const char* name) {
  if (!ok) throw std::runtime_error(name);
  std::printf("PASS %s\n", name);
  ++checks;
}
void Hr(HRESULT result, const char* name) {
  if (FAILED(result)) {
    std::fprintf(stderr, "%s HRESULT=%08lx\n", name, static_cast<unsigned long>(result));
    throw std::runtime_error(name);
  }
}

class IndicatorSink final : public ITfLangBarItemSink {
 public:
  ULONG refs=1; DWORD flags=0; int updates=0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
    if(!out)return E_POINTER;
    *out=nullptr;
    if(iid!=IID_IUnknown && iid!=IID_ITfLangBarItemSink)return E_NOINTERFACE;
    *out=static_cast<ITfLangBarItemSink*>(this);AddRef();return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
  ULONG STDMETHODCALLTYPE Release() override {return --refs;}
  HRESULT STDMETHODCALLTYPE OnUpdate(DWORD next) override {flags|=next;++updates;return S_OK;}
};
std::wstring IndicatorText(ITfLangBarItemButton* item) {
  BSTR text=nullptr;Hr(item->GetText(&text),"read input mode text");
  const std::wstring value(text,SysStringLen(text));SysFreeString(text);return value;
}
void CheckIndicatorContract() {
  IndicatorSink sink;
  auto* item=new InputIndicator(GetModuleHandleW(nullptr),nullptr,nullptr,InputMode::Chinese);
  DWORD cookie=0,other=0;
  Hr(item->AdviseSink(IID_ITfLangBarItemSink,&sink,&cookie),"advise indicator updates");
  Check(item->AdviseSink(IID_ITfLangBarItemSink,&sink,&other)==CONNECT_E_ADVISELIMIT,"indicator refuses duplicate sink without losing observer");
  item->SetMode(InputMode::English);
  Check(sink.updates==1 && (sink.flags&(TF_LBI_ICON|TF_LBI_TEXT|TF_LBI_TOOLTIP))==(TF_LBI_ICON|TF_LBI_TEXT|TF_LBI_TOOLTIP),"mode change invalidates icon, text and accessible tooltip");
  item->SetMode(InputMode::English);
  Check(sink.updates==1,"unchanged input mode does not repaint repeatedly");
  Check(item->UnadviseSink(cookie+1)==CONNECT_E_NOCONNECTION,"wrong sink cookie cannot disconnect observer");
  Hr(item->UnadviseSink(cookie),"unadvise indicator");
  Check(sink.refs==1,"unadvise releases observer");
  Hr(item->AdviseSink(IID_ITfLangBarItemSink,&sink,&other),"re-advise indicator");
  Check(cookie!=other,"old sink cookie cannot disconnect a new subscription");
  item->Detach();
  Check(sink.refs==1 && item->OnClick(TF_LBI_CLK_LEFT,{},nullptr)==S_FALSE,"detached shell item releases observer and cannot call former service");
  item->Release();
  for(int resource:{IDI_SIMPLE_VOICE,IDI_SOUND_CORE_DARK,IDI_SOUND_CORE_LIGHT,IDI_MODE_CHINESE,IDI_MODE_ENGLISH,IDI_MODE_CHINESE_LIGHT,IDI_MODE_ENGLISH_LIGHT}) {
    bool valid=true;
    for(int pixels:{16,20,24,32,40,48}) {
      const HICON icon=LoadProductIcon(GetModuleHandleW(nullptr),resource,pixels);
      ICONINFO info{};BITMAP bitmap{};
      valid=icon && GetIconInfo(icon,&info) && GetObjectW(info.hbmColor,sizeof(bitmap),&bitmap) &&
          bitmap.bmWidth==pixels && bitmap.bmHeight==pixels && bitmap.bmBitsPixel==32 && valid;
      if(info.hbmColor)DeleteObject(info.hbmColor);if(info.hbmMask)DeleteObject(info.hbmMask);
      if(icon)DestroyIcon(icon);
    }
    Check(valid,"embedded icon family supplies all six Windows sizes in 32-bit color");
  }
}

class TextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink {
 public:
  explicit TextStore(bool readonly = false) : readonly_(readonly) {
    hwnd_ = CreateWindowExW(0,L"STATIC",L"VoiceIme TSF test document",WS_POPUP,
                             100,100,700,400,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
  }
  ~TextStore() { if (sink_) sink_->Release(); if (hwnd_) DestroyWindow(hwnd_); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_ITextStoreACP) *out = static_cast<ITextStoreACP*>(this);
    if (iid == IID_ITfContextOwnerCompositionSink) *out = static_cast<ITfContextOwnerCompositionSink*>(this);
    if (!*out) return E_NOINTERFACE;
    AddRef(); return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { const auto n=--refs_; if (!n) delete this; return n; }
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid,IUnknown* object,DWORD) override {
    if (iid != IID_ITextStoreACPSink) return E_INVALIDARG;
    if (sink_) sink_->Release();
    sink_=nullptr;
    return object->QueryInterface(IID_ITextStoreACPSink,reinterpret_cast<void**>(&sink_));
  }
  HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown*) override { if(sink_)sink_->Release();sink_=nullptr;return S_OK; }
  HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags,HRESULT* result) override {
    if (!sink_) return E_FAIL;
    if (lock_) { *result=TS_E_SYNCHRONOUS; return S_OK; }
    lock_=flags;
    *result=sink_->OnLockGranted(flags);
    lock_=0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override {
    status->dwDynamicFlags=readonly_ ? TS_SD_READONLY : 0;
    status->dwStaticFlags=0; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE QueryInsert(LONG start,LONG end,ULONG,LONG* out_start,LONG* out_end) override {
    if (start<0 || end<start || end>static_cast<LONG>(text.size()))return TS_E_INVALIDPOS;
    *out_start=start; *out_end=end; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetSelection(ULONG,ULONG count,TS_SELECTION_ACP* out,ULONG* fetched) override {
    *fetched=count ? 1 : 0; if(count)*out=selection_; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetSelection(ULONG count,const TS_SELECTION_ACP* value) override {
    if(count)selection_=*value; return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetText(LONG start,LONG end,WCHAR* plain,ULONG requested,ULONG* got,
      TS_RUNINFO* runs,ULONG runs_requested,ULONG* run_count,LONG* next) override {
    if (end == -1) end=static_cast<LONG>(text.size());
    if (start<0 || end<start || end>static_cast<LONG>(text.size())) return TS_E_INVALIDPOS;
    const ULONG size=std::min(requested,static_cast<ULONG>(end-start));
    if(plain && size)std::copy_n(text.data()+start,size,plain);
    if(got)*got=size;
    if(run_count)*run_count=runs_requested && size ? 1 : 0;
    if(runs_requested && size)runs[0]={size,TS_RT_PLAIN};
    if(next)*next=start+size;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE SetText(DWORD,LONG start,LONG end,const WCHAR* value,ULONG count,TS_TEXTCHANGE* change) override {
    if(readonly_)return TS_E_READONLY;
    if((lock_ & TS_LF_READWRITE)!=TS_LF_READWRITE)return TS_E_NOLOCK;
    if(start<0 || end<start || end>static_cast<LONG>(text.size()))return TS_E_INVALIDPOS;
    text.replace(start,end-start,value ? std::wstring(value,count) : L"");
    *change={start,end,start+static_cast<LONG>(count)};
    selection_={change->acpNewEnd,change->acpNewEnd,{TS_AE_END,FALSE}};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetFormattedText(LONG,LONG,IDataObject**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetEmbedded(LONG,REFGUID,REFIID,IUnknown**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*,const FORMATETC*,BOOL* ok) override { *ok=FALSE;return S_OK; }
  HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD,LONG,LONG,IDataObject*,TS_TEXTCHANGE*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags,const WCHAR* text,ULONG count,LONG* start,LONG* end,TS_TEXTCHANGE* change) override {
    if(start)*start=selection_.acpStart;
    if(end)*end=selection_.acpEnd;
    if(flags & TS_IAS_QUERYONLY)return S_OK;
    TS_TEXTCHANGE local{};
    const auto hr=SetText(0,selection_.acpStart,selection_.acpEnd,text,count,change ? change : &local);
    if(SUCCEEDED(hr) && end)*end=selection_.acpEnd;
    return hr;
  }
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD,IDataObject*,LONG*,LONG*,TS_TEXTCHANGE*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD,ULONG,const TS_ATTRID*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG,ULONG,const TS_ATTRID*,DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG,ULONG,const TS_ATTRID*,DWORD) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG,LONG halt,ULONG,const TS_ATTRID*,DWORD,LONG* next,BOOL* found,LONG* offset) override {
    *next=halt;*found=FALSE;*offset=0;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG,TS_ATTRVAL*,ULONG* count) override { *count=0;return S_OK; }
  HRESULT STDMETHODCALLTYPE GetEndACP(LONG* end) override { *end=static_cast<LONG>(text.size());return S_OK; }
  HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override { *view=1;return S_OK; }
  HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie,const POINT*,DWORD,LONG* pos) override { *pos=selection_.acpEnd;return S_OK; }
  HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie,LONG start,LONG end,RECT* rect,BOOL* clipped) override {
    if (layout_unavailable) return TS_E_NOLAYOUT;
    *rect={layout_x+start*10,layout_y,layout_x+1+end*10,layout_y+24};*clipped=FALSE;return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie,RECT* rect) override { *rect={100,100,800,500};return S_OK; }
  HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie,HWND* hwnd) override { *hwnd=hwnd_;return S_OK; }
  HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*,BOOL* ok) override { *ok=TRUE;return S_OK; }
  HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*,ITfRange*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override { return S_OK; }
  std::wstring text;
  int layout_x=120,layout_y=120;
  bool layout_unavailable=false;
 private:
  ULONG refs_=1;
  DWORD lock_=0;
  bool readonly_=false;
  HWND hwnd_=nullptr;
  ITextStoreACPSink* sink_=nullptr;
  TS_SELECTION_ACP selection_{0,0,{TS_AE_END,FALSE}};
};

struct Document {
  TextStore* store;
  ITfDocumentMgr* manager=nullptr;
  ITfContext* context=nullptr;
  Document(ITfThreadMgr* thread,TfClientId id,bool readonly=false) : store(new TextStore(readonly)) {
    Hr(thread->CreateDocumentMgr(&manager),"create document manager");
    TfEditCookie cookie=0;
    Hr(manager->CreateContext(id,0,static_cast<ITextStoreACP*>(store),&context,&cookie),"create context");
    Hr(manager->Push(context),"push context");
  }
  ~Document() {
    if(manager)manager->Pop(TF_POPF_ALL);
    if(context)context->Release();
    if(manager)manager->Release();
    store->Release();
  }
};
bool Key(VoiceService& service,ITfContext* context,int vk) {
  BOOL eaten=FALSE;
  service.OnKey(context,vk,0,&eaten,false);
  if(eaten)service.OnKey(context,vk,0,&eaten,true);
  const bool handled=eaten!=FALSE;
  service.OnKeyUp(context,vk,&eaten,true);
  return handled;
}
void Type(VoiceService& service,ITfContext* context,const char* letters) {
  for (const char* p=letters;*p;++p)Check(Key(service,context,*p-'a'+'A'),"pinyin key handled by TSF");
}
void Pump(DWORD milliseconds) {
  const auto end=GetTickCount64()+milliseconds;
  do {
    MSG message{};
    while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    Sleep(5);
  } while(GetTickCount64()<end);
}
RECT CandidateRect() {
  HWND found=nullptr;
  EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM data)->BOOL {
    wchar_t name[64]{}; GetClassNameW(window,name,64);
    if(lstrcmpW(name,L"VoiceImeCandidates")==0 && IsWindowVisible(window)) {
      *reinterpret_cast<HWND*>(data)=window; return FALSE;
    }
    return TRUE;
  },reinterpret_cast<LPARAM>(&found));
  if(!found) throw std::runtime_error("candidate popup is not visible");
  UiDpiScope coordinates; RECT rect{}; GetWindowRect(found,&rect); return rect;
}

struct Deadline {
  HANDLE event=CreateWaitableTimerW(nullptr,TRUE,nullptr);
  Deadline() {
    LARGE_INTEGER due{}; due.QuadPart=-30000000;
    if (!event || !SetWaitableTimer(event,&due,0,nullptr,nullptr,FALSE))
      throw std::runtime_error("create pipe deadline");
  }
  ~Deadline() { CloseHandle(event); }
};
// This server talks to the production reader on its process-private QA pipe.
class PipeServer {
 public:
  PipeServer() {
    const auto name=L"\\\\.\\pipe\\VoiceImeTsfQA_"+std::to_wstring(GetCurrentProcessId());
    pipe_=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
        1,65536,65536,0,nullptr);
    if (pipe_==INVALID_HANDLE_VALUE) throw std::runtime_error("create isolated TSF server");
    OVERLAPPED operation{}; operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    bool connected=ConnectNamedPipe(pipe_,&operation)!=FALSE;
    const auto error=connected ? ERROR_SUCCESS : GetLastError();
    if (error==ERROR_PIPE_CONNECTED) connected=true;
    if (error==ERROR_IO_PENDING) {
      const auto waited=WaitForSingleObject(operation.hEvent,3000);
      if (waited!=WAIT_OBJECT_0) CancelIoEx(pipe_,&operation);
      DWORD count=0;
      connected=GetOverlappedResult(pipe_,&operation,&count,TRUE)!=FALSE;
    }
    CloseHandle(operation.hEvent);
    if (!connected) { CloseHandle(pipe_); pipe_=INVALID_HANDLE_VALUE; throw std::runtime_error("TSF pipe connect timeout"); }
    Deadline timeout;
    Message hello; std::size_t used=0;
    while(DecodeFrame(buffer_,&hello,&used)==DecodeStatus::Incomplete) {
      const auto read=ReadPipeChunk(pipe_,nullptr,timeout.event,20);
      buffer_+=read.bytes;
      if(read.status!=PipeReadStatus::Data)throw std::runtime_error("TSF handshake timeout");
    }
    Check(DecodeFrame(buffer_,&hello,&used)==DecodeStatus::Complete && hello.type==MsgType::ClientHello,
          "TSF reader introduces its host window before sending commands");
    const auto peer=PipeClientPeer(pipe_);
    Check(peer && peer->owns_window(reinterpret_cast<HWND>(hello.client_window),
          ExecutableDirectory()+L"\\voiceime_tsf_integration.exe"),"TSF handshake identifies its actual module and process");
    buffer_.erase(0,used);
    Message ready; ready.type=MsgType::ServerReady; Raw(Encode(ready)); Phase("0");
  }
  ~PipeServer() { if (pipe_!=INVALID_HANDLE_VALUE) CloseHandle(pipe_); }
  void Raw(const std::string& bytes) {
    Deadline timeout;
    if (!WritePipeBytes(pipe_,bytes,timeout.event)) throw std::runtime_error("send TSF wire request");
  }
  void Send(std::uint64_t id,const char* text) {
    Message message; message.type=MsgType::InsertText; message.request_id=id; message.text=text;
    Raw(Encode(message));
  }
  void Phase(const char* value) {
    Message message; message.type=MsgType::Phase; message.text=value; message.state_id=++state_id_; Raw(Encode(message));
  }
  void QueryCaret() { Message message; message.type=MsgType::QueryCaret; Raw(Encode(message)); }
  Message Caret(int expected_y) {
    const auto deadline=GetTickCount64()+1500;
    do {
      MSG pending{};
      while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)){TranslateMessage(&pending);DispatchMessageW(&pending);}
      Message message; std::size_t used=0;
      if(DecodeFrame(buffer_,&message,&used)==DecodeStatus::Complete){
        buffer_.erase(0,used);
        if(message.type==MsgType::Caret && message.caret_y==expected_y) return message;
        continue;
      }
      DWORD available=0;
      if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr)) throw std::runtime_error("caret pipe closed");
      if(available){ Deadline timeout; buffer_+=ReadPipeChunk(pipe_,nullptr,timeout.event,20).bytes; }
      else Sleep(5);
    }while(GetTickCount64()<deadline);
    throw std::runtime_error("FAIL voice caret did not follow a layout/selection move without another key");
  }
  bool NoCaret(DWORD milliseconds) {
    const auto deadline=GetTickCount64()+milliseconds;
    do {
      Pump(5);
      DWORD available=0;
      if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr)) return false;
      if(available){ Deadline timeout; buffer_+=ReadPipeChunk(pipe_,nullptr,timeout.event,20).bytes; }
      Message message; std::size_t used=0;
      while(DecodeFrame(buffer_,&message,&used)==DecodeStatus::Complete) {
        buffer_.erase(0,used);
        if(message.type==MsgType::Caret) return false;
      }
    }while(GetTickCount64()<deadline);
    return true;
  }
  void Result(std::uint64_t id,const char* expected) {
    Deadline timeout;
    for (;;) {
      Message message; std::size_t used=0;
      const auto status=DecodeFrame(buffer_,&message,&used);
      if (status==DecodeStatus::Invalid) throw std::runtime_error("invalid TSF response");
      if (status==DecodeStatus::Complete) {
        buffer_.erase(0,used);
        if (message.type!=MsgType::InsertResult) continue;
        Check(message.request_id==id && message.text==expected,"TSF wire acknowledgment preserves request identity and result");
        return;
      }
      const auto read=ReadPipeChunk(pipe_,nullptr,timeout.event,20);
      buffer_+=read.bytes;
      if (read.status!=PipeReadStatus::Data) throw std::runtime_error("TSF response timeout/disconnect");
    }
  }
  bool Closed() {
    Deadline timeout;
    for (;;) {
      const auto read=ReadPipeChunk(pipe_,nullptr,timeout.event,20);
      if (read.status!=PipeReadStatus::Data) return read.status==PipeReadStatus::Closed;
    }
  }
 private:
  HANDLE pipe_=INVALID_HANDLE_VALUE;
  std::uint64_t state_id_=0;
  std::string buffer_;
};
MSG QueuedInsert(std::uint64_t id) {
  MSG message{};
  const auto deadline=GetTickCount64()+3000;
  do {
    if (PeekMessageW(&message,nullptr,kCommitMessage,kCommitMessage,PM_REMOVE)) {
      if (reinterpret_cast<IncomingCommit*>(message.lParam)->id!=id)
        throw std::runtime_error("unexpected queued TSF insert");
      return message;
    }
    Sleep(5);
  } while (GetTickCount64()<deadline);
  throw std::runtime_error("TSF insertion was not queued");
}
}  // namespace

int main(int argc,char** argv) {
  try {
    const auto data=std::filesystem::absolute(argc>1 ? argv[1] : "build/tsf-integration");
    std::filesystem::create_directories(data / "VoiceIme");
    // Environment is private to this disposable executable, not the desktop.
    SetEnvironmentVariableW(L"LOCALAPPDATA",data.c_str());
    const auto hotkey_path=(data/"VoiceIme"/"config.json").string();
    Check(SaveSettings(hotkey_path,Settings{DefaultHotkey()}),"reset isolated hotkey fixture");
    Hr(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"initialize COM");
    CheckIndicatorContract();
    ITfThreadMgrEx* thread=nullptr;
    Hr(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgrEx,
                        reinterpret_cast<void**>(&thread)),"create real TSF manager");
    TfClientId id=0;
    Hr(thread->ActivateEx(&id,TF_TMAE_NOACTIVATETIP | TF_TMAE_NOACTIVATEKEYBOARDLAYOUT | TF_TMAE_SECUREMODE),"activate hidden TSF manager");
    BYTE keyboard[256]{},clear[256]{};
    GetKeyboardState(keyboard);SetKeyboardState(clear);
    for(bool remove:{false,true}) {
      Document edited(thread,id);
      Hr(thread->SetFocus(edited.manager),"focus composition editing fixture");
      VoiceService service; Hr(service.Activate(thread,id),"activate service for composition editing");
      Type(service,edited.context,"nihao"); Key(service,edited.context,VK_LEFT);
      if(remove) Key(service,edited.context,VK_DELETE);
      Key(service,edited.context,VK_RETURN);
      Check(edited.store->text==(remove?L"niao":L"nihao"),"Enter commits the complete edited raw input through real TSF");
      service.Deactivate();
    }
    {
      Document doc(thread,id);
      Document second(thread,id);
      Document readonly(thread,id,true);
      Hr(thread->SetFocus(doc.manager),"focus document");
      VoiceService service;
      Hr(service.Activate(thread,id),"activate production text service");
      Check(service.CachedHotkeyForTest().vk==DefaultHotkey().vk,"TSF loads the hotkey on activation");
      Check(SaveSettings(hotkey_path,Settings{Hotkey{0,VK_F8}}),"save a changed hotkey in isolated preferences");
      Check(service.CachedHotkeyForTest().vk==DefaultHotkey().vk,"TSF keeps its current key decision until notified");
      SendMessageW(service.IndicatorMessageWindowForTest(),HotkeyChangedMessage(),0,0);
      Check(service.CachedHotkeyForTest().vk==VK_F8,"hotkey change notification refreshes the active TSF service");
      Check(SaveSettings(hotkey_path,Settings{DefaultHotkey()}),"restore isolated default hotkey");
      SendMessageW(service.IndicatorMessageWindowForTest(),HotkeyChangedMessage(),0,0);
      ITfLangBarItemMgr* language_bar=nullptr;
      Hr(thread->QueryInterface(IID_ITfLangBarItemMgr,reinterpret_cast<void**>(&language_bar)),"get real language bar manager");
      ITfLangBarItem* item=nullptr;
      Hr(language_bar->GetItem(kSystemInputModeItem,&item),"Windows resolves the service input mode item");
      TF_LANGBARITEMINFO info{};Hr(item->GetInfo(&info),"read indicator identity");
      Check(info.clsidService==CLSID_VoiceIme && info.guidItem==kSystemInputModeItem,"indicator belongs to Simple Voice and uses Windows input-mode GUID");
      ITfLangBarItemButton* mode_button=nullptr;
      Hr(item->QueryInterface(kLangBarButtonIid,reinterpret_cast<void**>(&mode_button)),"get native mode button interface");
      item->Release();
      IndicatorSink mode_sink;DWORD mode_cookie=TF_INVALID_COOKIE;
      Hr(language_bar->AdviseItemSink(&mode_sink,&mode_cookie,kSystemInputModeItem),"observe real system mode item changes");
      mode_sink.flags=0;
      SendMessageW(service.IndicatorMessageWindowForTest(),WM_THEMECHANGED,0,0);
      Check((mode_sink.flags&TF_LBI_ICON)!=0,"system theme change notifies the shell to reload the correctly colored mode icon");
      Check(!IsWindowVisible(service.IndicatorMessageWindowForTest()) && GetAncestor(service.IndicatorMessageWindowForTest(),GA_PARENT)==GetDesktopWindow(),"theme listener is an invisible top-level tool window");
      Check(IndicatorText(mode_button)==L"中","active service initially shows Chinese mode");
      HICON mode_icon=nullptr;Hr(mode_button->GetIcon(&mode_icon),"load embedded mode icon");
      Check(mode_icon!=nullptr,"input indicator returns a real owned icon");DestroyIcon(mode_icon);
      BSTR tooltip=nullptr;Hr(mode_button->GetTooltipString(&tooltip),"read accessible input indicator tooltip");
      Check(std::wstring(tooltip).find(L"轻音输入法 · Simple Voice")!=std::wstring::npos,"indicator tooltip identifies the product in Chinese and English");SysFreeString(tooltip);
      Hr(mode_button->OnClick(TF_LBI_CLK_LEFT,{},nullptr),"click system mode button");
      Check(IndicatorText(mode_button)==L"A" && !Key(service,doc.context,'A'),"system mode click changes actual typing behavior to English");
      Hr(mode_button->OnClick(TF_LBI_CLK_LEFT,{},nullptr),"switch back through system mode button");
      Type(service,doc.context,"nihao");
      Check(!doc.store->text.empty(),"preedit reaches real TSF text store");
      Key(service,doc.context,VK_SPACE);
      Check(doc.store->text==L"你好","space commits nihao as Chinese through TSF");
      Type(service,doc.context,"nihao");
      Key(service,doc.context,VK_ESCAPE);
      Check(doc.store->text==L"你好","Esc removes only the unconfirmed composition");
      Key(service,doc.context,VK_OEM_PERIOD);
      Check(doc.store->text==L"你好。","Chinese punctuation commits through TSF");
      Key(service,doc.context,VK_SHIFT);
      Check(!Key(service,doc.context,'A'),"Shift switches to English passthrough");
      Check(IndicatorText(mode_button)==L"A","Shift keeps the Windows mode indicator synchronized");
      Key(service,doc.context,VK_SHIFT);
      Type(service,doc.context,"nihao");
      Key(service,doc.context,VK_RETURN);
      Check(doc.store->text==L"你好。nihao","Enter preserves raw input after switching back to Chinese");
      service.HandleCommit({L"语音定稿。",1,0});
      Check(doc.store->text==L"你好。nihao语音定稿。","voice commit inserts at the real TSF selection");
      service.HandleCommit({L"语音定稿。",1,0});
      Check(doc.store->text==L"你好。nihao语音定稿。","duplicate commit cannot insert twice");
      service.HandleCommit({L"过期连接",2,99});
      Check(doc.store->text==L"你好。nihao语音定稿。","stale connection cannot insert queued text");
      const auto before=doc.store->text;
      Type(service,doc.context,"ni");
      Hr(thread->SetFocus(second.manager),"switch document focus");
      MSG pending{};
      while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)){TranslateMessage(&pending);DispatchMessageW(&pending);}
      Type(service,second.context,"nihao");
      Key(service,second.context,VK_SPACE);
      Check(doc.store->text==before,"focus switch cancels the previous document preedit");
      Check(second.store->text==L"你好","new document receives its own composition");
      {
        WNDCLASSW klass{}; klass.lpfnWndProc=DefWindowProcW; klass.hInstance=GetModuleHandleW(nullptr);
        klass.lpszClassName=L"SimpleVoicePreviewEditor"; RegisterClassW(&klass);
        HWND editor=CreateWindowExW(0,klass.lpszClassName,L"Isolated preview editor",WS_OVERLAPPED,0,0,600,400,nullptr,nullptr,klass.hInstance,nullptr);
        HWND field=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE,0,0,400,200,editor,nullptr,klass.hInstance,nullptr);
        const HWND old_focus=SetFocus(field);
        Check(IsInputMethodEditor(GetFocus()),"preview editor is an independent editable context");
        service.SetVoiceActiveForTest(true);
        Type(service,second.context,"nihao"); Key(service,second.context,VK_SPACE);
        Check(second.store->text==L"你好你好","pending voice preview still permits pinyin editing in its own window");
        Check(!Key(service,second.context,VK_RETURN),"editor Enter is not intercepted as a voice insertion");
        service.SetVoiceActiveForTest(false); SetFocus(old_focus); DestroyWindow(editor);
      }
      Hr(thread->SetFocus(readonly.manager),"focus readonly document");
      Check(!Key(service,readonly.context,'N'),"readonly document does not consume pinyin");
      DWORD mode_status=0;Hr(mode_button->GetStatus(&mode_status),"read disabled mode status");
      Check((mode_status&TF_LBI_STATUS_DISABLED)!=0 && mode_button->OnClick(TF_LBI_CLK_LEFT,{},nullptr)==S_FALSE,"readonly document disables the system mode button");
      Check(readonly.store->text.empty(),"readonly document stays unchanged");
      Hr(thread->SetFocus(doc.manager),"refocus document for wire tests");
      MSG stale{};
      {
        PipeServer server;
        Message insert; insert.type=MsgType::InsertText; insert.request_id=10; insert.text="管道写入。";
        const auto bytes=Encode(insert);
        server.Raw(bytes.substr(0,2)); Sleep(20); server.Raw(bytes.substr(2));
        auto queued=QueuedInsert(10); DispatchMessageW(&queued);
        const auto written=doc.store->text;
        Check(written==before+L"管道写入。","fragmented pipe request inserts through real TSF");
        server.Result(10,"ok");
        server.Send(10,"管道写入。"); queued=QueuedInsert(10); DispatchMessageW(&queued);
        Check(doc.store->text==written,"duplicate wire request does not duplicate document text");
        server.Result(10,"ok");
        insert.request_id=11; insert.text="已取消的写入";
        Message cancel; cancel.type=MsgType::CancelInsert; cancel.request_id=11;
        Message next=insert; next.request_id=12; next.text="下一次。";
        server.Raw(Encode(insert)+Encode(cancel)+Encode(next));
        auto cancelled=QueuedInsert(11);
        auto following=QueuedInsert(12); // Posting this is a barrier after cancellation was read.
        DispatchMessageW(&cancelled);
        Check(doc.store->text==written,"cancellation suppresses an insert already queued for the UI");
        server.Result(11,"fail");
        DispatchMessageW(&following);
        Check(doc.store->text==written+L"下一次。","cancellation does not suppress a newer request");
        server.Result(12,"ok");
        server.Send(13,"断连后的旧消息"); stale=QueuedInsert(13);
      }
      {
        PipeServer server;
        const auto written=doc.store->text;
        server.Send(1,"重连新消息。"); auto current=QueuedInsert(1);
        DispatchMessageW(&stale);
        Check(doc.store->text==written,"queued request from disconnected pipe cannot insert after reconnection");
        DispatchMessageW(&current);
        Check(doc.store->text==written+L"重连新消息。","reconnection accepts a new request sequence");
        server.Result(1,"ok");
        Hr(thread->SetFocus(readonly.manager),"focus readonly document for wire test");
        server.Send(2,"不可写入"); auto rejected=QueuedInsert(2); DispatchMessageW(&rejected);
        Check(readonly.store->text.empty(),"wire insertion respects readonly TSF documents");
        server.Result(2,"fail");
        server.Raw(std::string("\xff\xff\xff\xff\x02",5));
        Check(server.Closed(),"TSF reader disconnects malformed length frames promptly");
      }
      {
        Hr(thread->SetFocus(doc.manager),"focus document for caret tracking");
        Type(service,doc.context,"ni");
        const auto first=CandidateRect();
        Type(service,doc.context,"hao");
        const auto extended=CandidateRect();
        Check(first.left==extended.left && first.top==extended.top,"candidate stays at composition start while preedit grows");
        doc.store->layout_unavailable=true;
        doc.store->layout_x+=100; doc.store->layout_y+=100; Pump(200);
        const auto delayed=CandidateRect();
        Check(EqualRect(&extended,&delayed),"candidate retains its last anchor during TS_E_NOLAYOUT");
        doc.store->layout_unavailable=false; Pump(200);
        const auto moved=CandidateRect();
        Check(moved.left==extended.left+100 && moved.top==extended.top+100,"candidate follows editor movement without a key event");
        Key(service,doc.context,VK_ESCAPE);
        doc.store->layout_x=120; doc.store->layout_y=120;
        mode_sink.flags=0;
        PipeServer server; server.Phase("1"); server.QueryCaret();
        Check(server.Caret(120).caret_h==24,"voice starts at the actual TSF caret");
        doc.store->layout_x=360; doc.store->layout_y=280;
        Check(server.Caret(280).caret_x>=360,"voice follows moved caret without another input key or server query");
        Check(server.NoCaret(200),"stationary voice caret does not generate duplicate IPC updates");
        doc.store->layout_unavailable=true;
        doc.store->layout_y=400; server.QueryCaret();
        Check(server.NoCaret(200),"unavailable browser-style layout does not publish a zero or fallback caret");
        doc.store->layout_unavailable=false;
        Check(server.Caret(400).caret_h==24,"voice resumes following when layout becomes available");
        mode_button->GetStatus(&mode_status);
        Check((mode_status&TF_LBI_STATUS_DISABLED)!=0 && (mode_sink.flags&TF_LBI_STATUS)!=0,"voice activity notifies Windows to repaint the disabled mode button without a focus change");
        mode_sink.flags=0;
        server.Phase("0");
        Pump(80); doc.store->layout_y=450;
        mode_button->GetStatus(&mode_status);
        Check(mode_status==0 && (mode_sink.flags&TF_LBI_STATUS)!=0,"voice returning idle notifies Windows to re-enable the mode button without a focus change");
        Check(server.NoCaret(200),"idle voice does not keep polling or publishing caret moves");
      }
      const auto start=GetTickCount64();
      service.Deactivate();
      ITfLangBarItem* removed=nullptr;
      const HRESULT removed_hr=language_bar->GetItem(kSystemInputModeItem,&removed);
      Check(FAILED(removed_hr) || !removed,"deactivation removes the mode item from Windows");
      if(removed)removed->Release();
      Check(mode_button->OnClick(TF_LBI_CLK_LEFT,{},nullptr)==S_FALSE,"shell-retained button is safe after service deactivation");
      mode_button->Release();language_bar->Release();
      Check(mode_sink.refs==1,"deactivation releases Windows mode observers");
      Check(GetTickCount64()-start<2000,"TSF reader exits promptly without an engine");
      thread->SetFocus(nullptr);
    }
    SetKeyboardState(keyboard);
    thread->Deactivate();
    thread->Release();
    CoUninitialize();
    std::printf("%d real TSF integration checks passed\n",checks);
    return 0;
  } catch(const std::exception& e) {
    std::fprintf(stderr,"FAIL %s\n",e.what());
    return 1;
  }
}
