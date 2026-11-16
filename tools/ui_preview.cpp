// Capture the real native controls with fixture text, without starting the
// voice engine, opening a microphone, registering a TIP, or reading user data.
#include "candidate_bar.hpp"
#include "confirm_box.hpp"
#include "hotkey_dialog.hpp"
#include "preferences_window.hpp"
#include "utf8.hpp"
#include <commctrl.h>
#include <ctime>
#include <objidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <oleacc.h>

static HotkeyAvailability fixture_availability = HotkeyAvailability::Available;

static int CheckSystemHotkeys() {
  HWND owner=CreateWindowExW(0,L"STATIC",L"",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
  if (!owner) return 2;
  if (!RegisterHotKey(owner,1,MOD_NOREPEAT,VK_F24)) {
    std::printf("SKIP system hotkey reservation unavailable: error=%lu\n",GetLastError());
    DestroyWindow(owner); return 77;
  }
  const bool conflict=ProbeHotkey({0,VK_F24})==HotkeyAvailability::Conflict;
  const BOOL released=UnregisterHotKey(owner,1);
  DestroyWindow(owner);
  const bool available=ProbeHotkey({0,VK_F24})==HotkeyAvailability::Available;
  std::printf("system_hotkey conflict=%d released=%d available_after_release=%d\n",conflict,released!=FALSE,available);
  return conflict && released && available ? 0 : 1;
}

static LRESULT Hook(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, void*) {
  return DefWindowProcW(hwnd, message, wp, lp);
}

static bool HoverCandidateChild(HWND window, long id) {
  IAccessible* accessible=nullptr;
  if (FAILED(AccessibleObjectFromWindow(window,OBJID_CLIENT,IID_IAccessible,reinterpret_cast<void**>(&accessible)))) return false;
  VARIANT child{}; child.vt=VT_I4; child.lVal=id;
  long x=0,y=0,width=0,height=0;
  const auto hr=accessible->accLocation(&x,&y,&width,&height,child);
  accessible->Release();
  if (FAILED(hr)) return false;
  POINT center{x+width/2,y+height/2}; ScreenToClient(window,&center);
  SendMessageW(window,WM_MOUSEMOVE,0,MAKELPARAM(center.x,center.y));
  return true;
}

static bool Save(HWND hwnd, const std::filesystem::path& path, bool settle = true) {
  if (hwnd == nullptr) return false;
  // Request both client and nonclient repaint before capturing. Synthetic DPI
  // messages still do not validate DWM frames during an actual monitor move.
  RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW | RDW_ALLCHILDREN);
  if (settle) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    Sleep(80);
  }
  RECT rect{};
  const bool client_only=path.filename().string().find("preferences-bottom-dpi-")!=std::string::npos;
  if(client_only) GetClientRect(hwnd,&rect);
  else { GetWindowRect(hwnd,&rect); rect.right-=rect.left; rect.bottom-=rect.top; }
  HDC dc=client_only?GetDC(hwnd):GetWindowDC(hwnd);
  HDC capture = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
  HGDIOBJ old = SelectObject(capture, bitmap);
  const BOOL copied = BitBlt(capture, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY | CAPTUREBLT);
  SelectObject(capture, old);
  bool saved = false;
  if (copied) {
    const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    Gdiplus::Bitmap image(bitmap, nullptr);
    saved = image.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
  }
  DeleteObject(bitmap); DeleteDC(capture); ReleaseDC(hwnd, dc);
  std::printf("%s %ldx%ld %s\n", path.filename().string().c_str(), rect.right, rect.bottom, saved ? "saved" : "FAILED");
  return saved;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (std::strcmp(argv[1],"--hotkey-system-check")==0) return CheckSystemHotkeys();
  SetProcessDPIAware();
  const auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return 2;
  const auto output = std::filesystem::path(argv[1]);
  std::filesystem::create_directories(output);
  bool ok = true;
  auto check = [&](bool value, const char* name) {
    std::printf("%s %s\n", value ? "PASS" : "FAIL", name);
    ok = value && ok;
  };
  {
    const auto menu=UiSkinMenu(UiCurrentSkin(),30);
    check(menu && GetMenuItemCount(menu)==3,"all three skins are available in the native menu");
    if (menu) {
      const auto selected=30+static_cast<UINT>(UiCurrentSkin());
      check((GetMenuState(menu,selected,MF_BYCOMMAND)&MF_CHECKED)!=0,"native skin menu marks the saved choice");
      DestroyMenu(menu);
    }
  }
  BYTE keyboard[256]{};
  GetKeyboardState(keyboard);
  BYTE clear_keyboard[256]{};
  SetKeyboardState(clear_keyboard);
  {
    CandidateBar bar;
    int clicked = -1;
    int settings_clicked = 0;
    bar.SetCandidateHandler([](void* value, int index) { *static_cast<int*>(value) = index; }, &clicked);
    bar.SetSettingsHandler([](void* value) { ++*static_cast<int*>(value); }, &settings_clicked);
    RimeView view;
    view.composing = true;
    view.preedit = "ni hao";
    view.page = {"你好", "拟好", "你", "呢", "泥"};
    bar.Show({160,160,1,22}, {100,100,1000,700}, view, InputMode::Chinese);
    IAccessible* accessible=nullptr;
    const auto accessible_hr=AccessibleObjectFromWindow(bar.hwnd(),OBJID_CLIENT,IID_IAccessible,reinterpret_cast<void**>(&accessible));
    check(SUCCEEDED(accessible_hr) && accessible,"candidate accessibility provider is discoverable");
    if (accessible) {
      long children=0; accessible->get_accChildCount(&children);
      check(children==6,"five accessible candidates plus settings are separate children");
      VARIANT child{},role{},state{}; child.vt=VT_I4; child.lVal=1;
      BSTR name=nullptr; accessible->get_accName(child,&name);
      check(name && std::wstring(name).find(L"你好")!=std::wstring::npos,"accessible candidate has its full label");
      SysFreeString(name);
      accessible->get_accRole(child,&role); accessible->get_accState(child,&state);
      check(role.vt==VT_I4 && role.lVal==ROLE_SYSTEM_LISTITEM && (state.lVal&STATE_SYSTEM_SELECTED),
            "candidate role and selection are available to assistive tools");
      child.lVal=2; accessible->accDoDefaultAction(child);
      check(clicked==1,"accessible candidate action selects the requested item");
      child.lVal=6; accessible->accDoDefaultAction(child);
      check(settings_clicked==1,"accessible settings action opens the settings handler");
      clicked=-1;settings_clicked=0;
      accessible->Release();
    }
    ok = Save(bar.hwnd(), output / "candidate.png") && ok;
    SendMessageW(bar.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(18,18));
    check(clicked == 0, "candidate mouse selection");
    RECT rect{}; GetClientRect(bar.hwnd(), &rect);
    SendMessageW(bar.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(rect.right-20,18));
    check(settings_clicked == 1, "settings hit target");
    check(SendMessageW(bar.hwnd(), WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "candidate preserves editor focus");
    check(HoverCandidateChild(bar.hwnd(),2),"hover fixture targets the second accessible candidate");
    // Capture the injected hover immediately; pumping real cursor leave events
    // would replace this fixture state before the screenshot is taken.
    ok=Save(bar.hwnd(),output/"candidate-hover.png",false) && ok;
    check(HoverCandidateChild(bar.hwnd(),6),"hover fixture targets the accessible settings control");
    ok=Save(bar.hwnd(),output/"candidate-settings-hover.png",false) && ok;
    SetEnvironmentVariableW(L"VOICEIME_TEST_HIGH_CONTRAST",L"1");
    SendMessageW(bar.hwnd(),WM_SETTINGCHANGE,0,0);
    const auto contrast=UiPalette(bar.skin());
    check(contrast.high_contrast && contrast.selected_text==GetSysColor(COLOR_HIGHLIGHTTEXT),
        "high-contrast color branch keeps the system highlight foreground");
    ok=Save(bar.hwnd(),output/"candidate-high-contrast-settings.png",false) && ok;
    HoverCandidateChild(bar.hwnd(),2);
    ok=Save(bar.hwnd(),output/"candidate-high-contrast-hover.png",false) && ok;
    SetEnvironmentVariableW(L"VOICEIME_TEST_HIGH_CONTRAST",nullptr);
    SendMessageW(bar.hwnd(),WM_MOUSELEAVE,0,0);
    SendMessageW(bar.hwnd(),WM_SETTINGCHANGE,0,0);
    view.page = {"今天我们一起讨论接下来的开发计划", "今天我们一起讨论接下来的安排", "今天我们一起", "今天我们", "今天"};
    bar.Show({160,160,1,22}, {100,100,1000,700}, view, InputMode::Chinese);
    ok = Save(bar.hwnd(), output / "candidate-long.png") && ok;
    view.page.assign(5, std::string(200,'W'));
    bar.Show({160,160,1,22}, {100,100,1000,700}, view, InputMode::Chinese);
    ok = Save(bar.hwnd(), output / "candidate-overflow.png") && ok;
    wchar_t title[4096]{};
    GetWindowTextW(bar.hwnd(), title, 4096);
    check(std::wstring(title).find(L"5 ") != std::wstring::npos, "all five candidate labels retained");
    for (UINT dpi : {144u,192u}) {
      SendMessageW(bar.hwnd(), WM_DPICHANGED, MAKELONG(dpi,dpi), 0);
      GetWindowRect(bar.hwnd(), &rect);
      check(rect.bottom-rect.top == UiPx(40,dpi), "candidate DPI layout");
      ok = Save(bar.hwnd(), output / ("candidate-dpi-"+std::to_string(dpi)+".png")) && ok;
    }
    bar.Hide();
    ConfirmBox box;
    if (!box.Create(GetModuleHandleW(nullptr), Hook, nullptr)) ok = false;
    box.ShowText("", {160,200,1,22}, {100,100,1000,700}, Phase::Listening);
    ok = Save(box.hwnd(), output / "voice-empty.png") && ok;
    box.ShowText("", {160,200,1,22}, {100,100,1000,700}, Phase::Pending);
    check(box.HintText().find(L"写入")==std::wstring::npos,"empty result never offers an unavailable insert");
    ok = Save(box.hwnd(), output / "voice-pending-empty.png") && ok;
    box.ShowText("这段文字正在写入。", {160,200,1,22}, {100,100,1000,700}, Phase::Committing);
    check(box.HintText().find(L"Esc")==std::wstring::npos,"in-flight insert does not promise cancellation");
    ok = Save(box.hwnd(), output / "voice-writing.png") && ok;
    box.ShowText("请先查看正文中是否已经出现这段文字。", {160,200,1,22}, {100,100,1000,700}, Phase::Pending,false,true);
    check(box.HintText().find(L"核对正文")!=std::wstring::npos,"uncertain insertion explains duplicate prevention");
    ok = Save(box.hwnd(), output / "voice-uncertain.png") && ok;
    box.ShowText("明天下午三点，我们在会议室讨论新方案。", {160,200,1,22}, {100,100,1000,700});
    ok = Save(box.hwnd(), output / "voice-text.png") && ok;
    box.ShowText("明天下午三点，我们在会议室讨论新方案。", {160,200,1,22}, {100,100,1000,700}, Phase::Finalizing);
    ok = Save(box.hwnd(), output / "voice-finalizing.png") && ok;
    box.ShowText("未能打开麦克风。", {160,200,1,22}, {100,100,1000,700}, Phase::Listening, true);
    ok = Save(box.hwnd(), output / "voice-error.png") && ok;
    std::string long_text;
    for (int n = 0; n < 50; ++n) long_text += "这是一段用于检查长文本边界的语音内容，必须能够完整查看并确认。";
    long_text += "全文结束：验收尾句。";
    box.ShowText(long_text, {160,200,1,22}, {100,100,1000,700});
    ok = Save(box.hwnd(), output / "voice-long.png") && ok;
    check(GetWindowTextLengthW(box.hwnd()) > 1024, "full long transcript retained");
    check(box.ScrollLimit()>0, "long transcript is scrollable");
    SendMessageW(box.hwnd(),WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);
    check(box.ScrollPosition()>0, "mouse wheel scrolls long transcript without taking focus");
    SendMessageW(box.hwnd(),WM_VSCROLL,SB_BOTTOM,0);
    check(box.ScrollPosition()==box.ScrollLimit(), "long transcript scroll reaches end");
    ok = Save(box.hwnd(), output / "voice-long-end.png") && ok;
    const auto saved_scroll=box.ScrollPosition();
    RECT before_skin{}; GetWindowRect(box.hwnd(),&before_skin);
    const auto original_skin=box.skin();
    for (auto skin : {UiSkin::Titanium,UiSkin::Daylight,UiSkin::DeepSpace}) {
      box.SetSkin(skin); bar.SetSkin(skin);
      RECT after_skin{}; GetWindowRect(box.hwnd(),&after_skin);
      check(EqualRect(&before_skin,&after_skin) && box.ScrollPosition()==saved_scroll,
          "live skin switch preserves voice placement and long-text scroll");
      check(box.skin()==skin && bar.skin()==skin,"voice and candidate controls use the same selected skin");
      check(!IsWindowVisible(bar.hwnd()),"skin switch never reopens a hidden candidate bar");
    }
    box.SetSkin(original_skin); bar.SetSkin(original_skin);
    SendMessageW(box.hwnd(),WM_VSCROLL,SB_TOP,0);
    GetClientRect(box.hwnd(),&rect);
    const auto box_dpi=UiDpi(box.hwnd());
    SendMessageW(box.hwnd(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(rect.right-UiPx(6,box_dpi),UiPx(48,box_dpi)));
    SendMessageW(box.hwnd(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(rect.right-UiPx(6,box_dpi),rect.bottom-UiPx(35,box_dpi)));
    SendMessageW(box.hwnd(),WM_LBUTTONUP,0,MAKELPARAM(rect.right-UiPx(6,box_dpi),rect.bottom-UiPx(35,box_dpi)));
    check(box.ScrollPosition()==box.ScrollLimit(), "dragging the scroll thumb reaches transcript end");
    check(SendMessageW(box.hwnd(),WM_MOUSEACTIVATE,0,0) == MA_NOACTIVATE, "voice preserves editor focus");
    for (UINT dpi : {144u,192u}) {
      SendMessageW(box.hwnd(),WM_DPICHANGED,MAKELONG(dpi,dpi),0);
      check(box.ScrollPosition()==box.ScrollLimit(),"DPI change preserves a transcript scrolled to the end");
      GetWindowRect(box.hwnd(),&rect);
      const auto work = UiWorkArea({rect.left,rect.top,1,1});
      check(rect.left>=work.x && rect.top>=work.y && rect.right<=work.x+work.w && rect.bottom<=work.y+work.h, "voice DPI bounds");
      ok=Save(box.hwnd(),output/("voice-dpi-"+std::to_string(dpi)+".png")) && ok;
    }
    DestroyWindow(box.hwnd());

    struct SaveContext { std::string path; bool fail=false; } context{(output/"settings-test.json").string()};
    ok=SaveSettings(context.path,Settings{DefaultHotkey()}) && ok;
    HotkeyDialog hotkey;
    hotkey.Open(GetModuleHandleW(nullptr),DefaultHotkey(),[](void* owner,Hotkey key) {
      auto* context=static_cast<SaveContext*>(owner);
      return !context->fail && SaveSettings(context->path,Settings{key});
    },&context,[](Hotkey){return fixture_availability;});
    ok=Save(hotkey.hwnd(),output/"hotkey.png") && ok;
    SendMessageW(hotkey.hwnd(),WM_KEYDOWN,'A',0);
    check(IsWindow(hotkey.hwnd()),"invalid hotkey keeps dialog open");
    GetDlgItemTextW(hotkey.hwnd(),12,title,4096);
    check(std::wstring(title).find(L"请同时")!=std::wstring::npos,"invalid hotkey gives actionable feedback");
    ok=Save(hotkey.hwnd(),output/"hotkey-invalid.png") && ok;
    fixture_availability=HotkeyAvailability::Conflict;
    SendMessageW(hotkey.hwnd(),WM_KEYDOWN,VK_F8,0);
    GetDlgItemTextW(hotkey.hwnd(),12,title,4096);
    check(std::wstring(title).find(L"占用")!=std::wstring::npos,"system conflict preserves the previous key");
    check(!std::filesystem::exists(context.path) || LoadSettings(context.path).hotkey.vk==DefaultHotkey().vk,
          "conflict never saves the rejected key");
    ok=Save(hotkey.hwnd(),output/"hotkey-conflict.png") && ok;
    fixture_availability=HotkeyAvailability::Unavailable;
    SendMessageW(hotkey.hwnd(),WM_KEYDOWN,VK_F8,0);
    GetDlgItemTextW(hotkey.hwnd(),12,title,4096);
    check(std::wstring(title).find(L"无法检查")!=std::wstring::npos,"failed conflict check is not presented as available");
    fixture_availability=HotkeyAvailability::Available;
    context.fail=true;
    SendMessageW(hotkey.hwnd(),WM_KEYDOWN,VK_F8,0);
    GetDlgItemTextW(hotkey.hwnd(),12,title,4096);
    check(std::wstring(title).find(L"未能保存")!=std::wstring::npos,"save failure gives feedback without inventing a cause");
    ok=Save(hotkey.hwnd(),output/"hotkey-save-failed.png") && ok;
    context.fail=false;
    SendMessageW(hotkey.hwnd(),WM_KEYDOWN,VK_F8,0);
    check(LoadSettings(context.path).hotkey.vk==VK_F8,"valid hotkey persists");
    GetDlgItemTextW(hotkey.hwnd(),12,title,4096);
    check(std::wstring(title).find(L"已保存")!=std::wstring::npos,"save success gives feedback");
    ok=Save(hotkey.hwnd(),output/"hotkey-saved.png") && ok;
    for (UINT dpi : {144u,192u}) {
      SendMessageW(hotkey.hwnd(),WM_DPICHANGED,MAKELONG(dpi,dpi),0);
      ok=Save(hotkey.hwnd(),output/("hotkey-dpi-"+std::to_string(dpi)+".png")) && ok;
    }
    const HWND button_window=hotkey.hwnd();
    HWND button=GetDlgItem(button_window,IDCANCEL);
    SetFocus(button);
    MSG space{button,WM_KEYDOWN,VK_SPACE,0};
    const bool intercepted=hotkey.Translate(&space);
    if (!intercepted) DispatchMessageW(&space);
    space.message=WM_KEYUP;
    if (!hotkey.Translate(&space)) DispatchMessageW(&space);
    check(!intercepted && !IsWindow(button_window),"Space activates the focused native button instead of recording a key");
    hotkey.Open(GetModuleHandleW(nullptr),DefaultHotkey(),nullptr,nullptr);
    const HWND old=hotkey.hwnd();
    SendMessageW(old,WM_KEYDOWN,VK_ESCAPE,0);
    check(!IsWindow(old),"Esc closes hotkey dialog");
  }
  {
    Lexicon lexicon(":memory:");
    for(int i=0;i<230;++i) lexicon.SaveTerm("示例术语"+std::to_string(i),"sample"+std::string(1,char('a'+i%26))+std::string(1,char('a'+i/26)),std::time(nullptr));
    PreferencesWindow preferences; PreferencesActions actions; InputPreferences saved;
    actions.save=[&](const InputPreferences& next){saved=next;return SaveInputPreferences((output/"input-preferences.ini").wstring(),next);};
    actions.skin=[](UiSkin){return true;}; actions.terms_changed=[&]{return lexicon.ExportTerms((output/"voiceime_personal.txt").wstring());};
    actions.input_code=[](const std::string&){return "qingyin";}; actions.diagnostics=[] {return L"Simple Voice RC · Windows x64\r\n语音模型：就绪 · 中英通用\r\n音频只在内存处理；诊断不包含输入正文。";};
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,1);
    const HWND window=preferences.hwnd();
    check(SendDlgItemMessageW(window,31,LB_GETCOUNT,0,0)==100,"personal terms paginate instead of rendering an unbounded list");
    SendMessageW(window,WM_COMMAND,40,0);
    check(SendDlgItemMessageW(window,31,LB_GETCOUNT,0,0)==100,"next page remains bounded");
    SetDlgItemTextW(window,32,L"轻音输入法"); SetDlgItemTextW(window,33,L"qingyin"); SendMessageW(window,WM_COMMAND,35,0);
    check(lexicon.Count()==231,"explicit native save creates a personal term");
    SetDlgItemTextW(window,30,L"轻音");
    check(SendDlgItemMessageW(window,31,LB_GETCOUNT,0,0)==1,"search narrows personal terms");
    check(!IsWindowEnabled(GetDlgItem(window,39)) && !IsWindowEnabled(GetDlgItem(window,40)),"single-page search disables both pagination actions");
    SendDlgItemMessageW(window,31,LB_SETCURSEL,0,0); SendMessageW(window,WM_COMMAND,MAKEWPARAM(31,LBN_SELCHANGE),0);
    ok=Save(window,output/"preferences-terms.png") && ok;
    SendMessageW(window,WM_COMMAND,37,0); check(lexicon.Search("轻音").empty(),"native delete removes selected term");
    SendMessageW(window,WM_COMMAND,38,0); check(lexicon.Search("轻音").size()==1,"native undo restores the deleted term");
    SetDlgItemTextW(window,30,L"没有这个匹配"); ok=Save(window,output/"preferences-empty-search.png") && ok;
    for(int page:{0,2,3}) { preferences.Open(&lexicon,{},UiCurrentSkin(),actions,page); ok=Save(window,output/("preferences-page-"+std::to_string(page)+".png")) && ok; }
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,0);
    for(UINT dpi:{144u,192u}) {
      SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),0);
      SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0);
      RECT button{},client{}; GetWindowRect(GetDlgItem(window,23),&button); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2); GetClientRect(window,&client);
      check(button.top>=0 && button.bottom<=client.bottom,"settings save action remains reachable at high DPI");
      ok=Save(window,output/("preferences-bottom-dpi-"+std::to_string(dpi)+".png")) && ok;
    }
    preferences.Close();
    PreviewEditor editor; std::string edited;
    editor.Open("这是<unk>","原始识别文本",[&](std::string text){edited=std::move(text);});
    SendMessageW(editor.hwnd(),WM_COMMAND,IDOK,0);
    check(IsWindow(editor.hwnd()) && edited.empty(),"unknown speech cannot be saved as a completed preview");
    SendMessageW(editor.hwnd(),WM_COMMAND,12,0);
    ok=Save(editor.hwnd(),output/"preview-editor.png") && ok;
    auto editor_key=[&](int vk) {
      MSG key{GetFocus(),WM_KEYDOWN,static_cast<WPARAM>(vk),1};
      if(!editor.Translate(&key)) { TranslateMessage(&key); DispatchMessageW(&key); }
      MSG pending{};
      while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE)) {
        if(!editor.Translate(&pending)) { TranslateMessage(&pending); DispatchMessageW(&pending); }
      }
      key={GetFocus(),WM_KEYUP,static_cast<WPARAM>(vk),1};
      if(!editor.Translate(&key)) { TranslateMessage(&key); DispatchMessageW(&key); }
    };
    SetFocus(GetDlgItem(editor.hwnd(),10));
    for(int id:{12,13,IDOK,10}) {
      editor_key(VK_TAB);
      check(GetFocus()==GetDlgItem(editor.hwnd(),id),"Tab traverses every preview control and returns to text");
    }
    BYTE preview_keys[256]{}; preview_keys[VK_SHIFT]=0x80; SetKeyboardState(preview_keys);
    editor_key(VK_TAB); check(GetFocus()==GetDlgItem(editor.hwnd(),IDOK),"Shift Tab leaves preview text in reverse order");
    preview_keys[VK_SHIFT]=0; preview_keys[VK_CONTROL]=0x80; SetKeyboardState(preview_keys);
    editor_key(VK_RETURN); preview_keys[VK_CONTROL]=0; SetKeyboardState(preview_keys);
    check(edited=="原始识别文本" && !editor.hwnd(),"restore and save preserve the exact original recognition");
    editor.Open(edited,"原始识别文本",[&](std::string text){edited=std::move(text);});
    SendDlgItemMessageW(editor.hwnd(),10,EM_SETSEL,0,-1);
    SendDlgItemMessageW(editor.hwnd(),10,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"我修改了预览 GitHub 20"));
    SendMessageW(editor.hwnd(),WM_COMMAND,IDOK,0);
    check(edited=="我修改了预览 GitHub 20","manual edits survive saving and reopening the native preview");
  }
  SetKeyboardState(keyboard);
  Gdiplus::GdiplusShutdown(token);
  if (SUCCEEDED(com)) CoUninitialize();
  return ok ? 0 : 1;
}
