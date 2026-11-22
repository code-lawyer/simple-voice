// Explicit interactive deployment check, never a CTest: briefly opens its own
// empty edit control, selects the installed profile, optionally captures the taskbar,
// then restores the prior profile and foreground. Never sends keys or records.
#include <initguid.h>
#include "tsf_service.hpp"
#include <msctf.h>
#include <gdiplus.h>
#include <cstdio>
#include <filesystem>

namespace {
bool CaptureTaskbar(const std::filesystem::path& path) {
  const HWND taskbar=FindWindowW(L"Shell_TrayWnd",nullptr);
  RECT rect{};if(!taskbar || !GetWindowRect(taskbar,&rect))return false;
  const int width=std::min(660L,rect.right-rect.left),height=rect.bottom-rect.top;
  HDC screen=GetDC(nullptr),memory=CreateCompatibleDC(screen);
  HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
  auto old=SelectObject(memory,bitmap);
  const bool copied=BitBlt(memory,0,0,width,height,screen,rect.right-width,rect.top,SRCCOPY|CAPTUREBLT)!=FALSE;
  SelectObject(memory,old);DeleteDC(memory);ReleaseDC(nullptr,screen);
  bool saved=false;
  if(copied) {
    Gdiplus::Bitmap image(bitmap,nullptr);
    const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    saved=image.Save(path.c_str(),&png,nullptr)==Gdiplus::Ok;
  }
  DeleteObject(bitmap);return saved;
}
LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  if(message==WM_CLOSE){DestroyWindow(hwnd);return 0;}
  if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
  return DefWindowProcW(hwnd,message,wp,lp);
}
}
int main(int argc,char** argv) {
  const bool activate_only=argc==2 && std::string(argv[1])=="--activate";
  if(!activate_only && (argc!=3 || std::string(argv[1])!="--show")) {
    std::fprintf(stderr,"Usage: voiceime_indicator_live_check --activate | --show <taskbar.png>\n");return 2;
  }
  SetProcessDPIAware();
  if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 1;
  Gdiplus::GdiplusStartupInput gdiplus_input;ULONG_PTR graphics=0;
  Gdiplus::GdiplusStartup(&graphics,&gdiplus_input,nullptr);
  const HWND previous_window=GetForegroundWindow();
  ITfInputProcessorProfiles* profiles=nullptr;ITfThreadMgr* thread=nullptr;
  HRESULT hr=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,
      IID_ITfInputProcessorProfiles,reinterpret_cast<void**>(&profiles));
  TF_LANGUAGEPROFILE previous{};bool have_previous=false;
  if(SUCCEEDED(hr)) {
    ITfInputProcessorProfileMgr* profile_manager=nullptr;
    if(SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(&profile_manager)))) {
      TF_INPUTPROCESSORPROFILE registered{};
      const HRESULT profile_hr=profile_manager->GetProfile(1,0x0804,CLSID_VoiceIme,GUID_VoiceImeProfile,nullptr,&registered);
      std::printf("profile_hresult=%08lx profile_caps=%08lx profile_flags=%08lx\n",profile_hr,registered.dwCaps,registered.dwFlags);
      profile_manager->Release();
    }
    LANGID language=0;profiles->GetCurrentLanguage(&language);
    IEnumTfLanguageProfiles* languages=nullptr;
    if(SUCCEEDED(profiles->EnumLanguageProfiles(language,&languages))) {
      TF_LANGUAGEPROFILE item{};ULONG count=0;
      while(languages->Next(1,&item,&count)==S_OK && count)if(item.fActive){previous=item;have_previous=true;break;}
      languages->Release();
    }
    hr=CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,reinterpret_cast<void**>(&thread));
  }
  TfClientId client=0;if(SUCCEEDED(hr))hr=thread->Activate(&client);
  WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Proc;cls.lpszClassName=L"SimpleVoiceIndicatorCheck";
  cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);RegisterClassW(&cls);
  HWND window=nullptr;bool captured=false,foreground=false,active_profile=false;std::wstring loaded;
  if(SUCCEEDED(hr)) {
    window=CreateWindowExW(0,cls.lpszClassName,activate_only?L"轻音输入法 · 切入检查（自动关闭）":L"轻音输入法 · 图标检查（自动关闭）",WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,CW_USEDEFAULT,500,180,nullptr,nullptr,cls.hInstance,nullptr);
    HWND edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL,
      28,40,420,40,window,nullptr,cls.hInstance,nullptr);
    ShowWindow(window,SW_SHOW);SetForegroundWindow(window);SetFocus(edit);
    hr=profiles->ActivateLanguageProfile(CLSID_VoiceIme,0x0804,GUID_VoiceImeProfile);
    const auto deadline=GetTickCount64()+2000;
    while(GetTickCount64()<deadline && IsWindow(window)) {
      MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
      MsgWaitForMultipleObjects(0,nullptr,FALSE,25,QS_ALLINPUT);
    }
    foreground=GetForegroundWindow()==window;
    ITfInputProcessorProfileMgr* active_manager=nullptr;
    if(SUCCEEDED(profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,reinterpret_cast<void**>(&active_manager)))) {
      TF_INPUTPROCESSORPROFILE active{};const auto active_hr=active_manager->GetActiveProfile(kTipKeyboardCategory,&active);
      active_profile=SUCCEEDED(active_hr) && active.clsid==CLSID_VoiceIme &&
          active.guidProfile==GUID_VoiceImeProfile && active.langid==0x0804;
      wchar_t clsid[40]{};StringFromGUID2(active.clsid,clsid,40);
      std::printf("active_hresult=%08lx active_clsid=%ls caps=%08lx\n",active_hr,clsid,active.dwCaps);active_manager->Release();
    }
    wchar_t path[32768]{};const HMODULE module=GetModuleHandleW(L"voiceime_tsf.dll");
    if(module && GetModuleFileNameW(module,path,32768))loaded=path;
    if(!activate_only && SUCCEEDED(hr) && foreground && active_profile && !loaded.empty())captured=CaptureTaskbar(std::filesystem::u8path(argv[2]));
  }
  if(have_previous)profiles->ActivateLanguageProfile(previous.clsid,previous.langid,previous.guidProfile);
  if(window) {if(foreground && IsWindow(previous_window))SetForegroundWindow(previous_window);if(IsWindow(window))DestroyWindow(window);}
  if(thread){thread->Deactivate();thread->Release();}if(profiles)profiles->Release();
  Gdiplus::GdiplusShutdown(graphics);CoUninitialize();
  std::printf("activate_hresult=%08lx\nfixture_foreground=%d\nloaded_tsf=%ls\ntaskbar_captured=%d\n",hr,foreground,loaded.c_str(),captured);
  if(activate_only)return SUCCEEDED(hr) && foreground && active_profile && !loaded.empty()?0:1;
  return captured?0:1;
}
