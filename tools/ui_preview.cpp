// Capture the real native controls with fixture text, without starting the
// voice engine, opening a microphone, registering a TIP, or reading user data.
#include "candidate_bar.hpp"
#include "confirm_box.hpp"
#include "hotkey_dialog.hpp"
#include "preferences_window.hpp"
#include "preferences_controls.hpp"
#include "sqlite3.h"
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

static std::wstring AccessibleName(HWND control) {
  IAccessible* accessible=nullptr;
  if(FAILED(AccessibleObjectFromWindow(control,OBJID_CLIENT,IID_IAccessible,reinterpret_cast<void**>(&accessible)))) return {};
  VARIANT child{}; child.vt=VT_I4; child.lVal=CHILDID_SELF;
  BSTR name=nullptr;
  accessible->get_accName(child,&name);
  std::wstring value=name?name:L"";
  SysFreeString(name); accessible->Release();
  return value;
}

// Ask the real button to print into a DIB so label visibility can be compared
// independently of its accessible caption, color scheme or installed font.
static std::vector<DWORD> ButtonPixels(HWND button) {
  RECT rect{}; GetClientRect(button,&rect);
  if(rect.right<=0 || rect.bottom<=0) return {};
  BITMAPINFO info{}; auto& header=info.bmiHeader;
  header.biSize=sizeof(header); header.biWidth=rect.right; header.biHeight=-rect.bottom;
  header.biPlanes=1; header.biBitCount=32; header.biCompression=BI_RGB;
  HDC dc=CreateCompatibleDC(nullptr);
  if(!dc) return {};
  void* bits=nullptr;
  HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
  std::vector<DWORD> pixels;
  if(bitmap && bits) {
    auto previous=SelectObject(dc,bitmap);
    SendMessageW(button,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_ERASEBKGND);
    GdiFlush();
    const auto* first=static_cast<const DWORD*>(bits);
    pixels.assign(first,first+rect.right*rect.bottom);
    SelectObject(dc,previous);
  }
  if(bitmap) DeleteObject(bitmap);
  DeleteDC(dc);
  return pixels;
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
  const bool client_only=path.filename().string().rfind("preferences-",0)==0;
  if(client_only) GetClientRect(hwnd,&rect);
  else { GetWindowRect(hwnd,&rect); rect.right-=rect.left; rect.bottom-=rect.top; }
  HDC dc=client_only?GetDC(hwnd):GetWindowDC(hwnd);
  HDC capture = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right, rect.bottom);
  HGDIOBJ old = SelectObject(capture, bitmap);
  const BOOL copied = client_only ? PrintWindow(hwnd,capture,PW_CLIENTONLY|2)
                                  : BitBlt(capture, 0, 0, rect.right, rect.bottom, dc, 0, 0, SRCCOPY | CAPTUREBLT);
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
    check(box.HintText().find(L"停顿不会结束")!=std::wstring::npos,"recording explains manual stop instead of a silence timer");
    box.SetRecordingProgress(61,false);
    check(box.StatusText().find(L"01:01 / 10:00")!=std::wstring::npos,"recording clock continues beyond the former one-minute limit");
    ok = Save(box.hwnd(), output / "voice-empty.png") && ok;
    box.SetRecordingProgress(575,false);
    check(box.StatusText().find(L"即将到时")!=std::wstring::npos,"the last 30 seconds announce the recording limit");
    ok = Save(box.hwnd(), output / "voice-limit-warning.png") && ok;
    box.ShowText("保留的录音草稿", {160,200,1,22}, {100,100,1000,700}, Phase::Stopped);
    check(box.HintText().find(L"开始校准")!=std::wstring::npos && box.HintText().find(L"写入")==std::wstring::npos,
          "limit-stopped capture asks for calibration, never insertion of an unfinished draft");
    ok = Save(box.hwnd(), output / "voice-stopped.png") && ok;
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
    for(int i=0;i<230;++i) lexicon.SaveTerm("示例术语"+std::to_string(i),(i<101?"page":"sample")+std::string(1,char('a'+i%26))+std::string(1,char('a'+i/26)),std::time(nullptr));
    PreferencesWindow preferences; PreferencesActions actions; InputPreferences saved;
    int input_saves=0,skin_saves=0,diagnostic_polls=0,model_polls=0;
    actions.save=[&](const InputPreferences& next){++input_saves;saved=next;return SaveInputPreferences((output/"input-preferences.ini").wstring(),next);};
    actions.skin=[&](UiSkin skin){++skin_saves;return SaveSkin((output/"appearance.json").wstring(),skin);};
    actions.hotkey_label=[] {return HotkeyLabel(DefaultHotkey());}; actions.terms_changed=[&]{return lexicon.ExportTerms((output/"voiceime_personal.txt").wstring());};
    actions.input_code=[](const std::string&){return "qingyin";}; actions.diagnostics=[&] {++diagnostic_polls;return L"Simple Voice RC · Windows x64\r\n语音模型：就绪 · 中英通用\r\n音频只在内存处理；诊断不包含输入正文。";};
    ModelSettings selected,last_requested;int uses=0;ModelTaskState task=ModelTaskState::Idle;
    bool reject=false,pending_model=false;
    auto requested_task=ModelTaskState::Running;
    std::wstring model_result;
    const std::wstring failure=L"未能启用，请检查目录并重试。\r\n"+std::wstring(600,L'长');
    actions.models=[&]{return selected;};
    actions.save_model_idle_release=[&](bool enabled){selected.idle_minutes=enabled?ModelSettings::kDefaultIdleMinutes:0;return true;};
    actions.model_status=[&](SpeechModel model)->std::wstring {
      ++model_polls;
      if(model==last_requested.model && !model_result.empty()) return model_result;
      return model==selected.model?L"使用中 · 已准备":L"尚未安装 · 启用时自动下载并验证。";
    };
    actions.model_task=[&]{return task;};
    actions.use_model=[&](const ModelSettings& next)->ModelActionResult {
      last_requested=next; ++uses;
      if(reject) return {failure};
      if(pending_model) { task=requested_task; return {L"正在验证模型。",true}; }
      selected=next; return {L"已启用选择的模型。"};
    };
    actions.practice_status=[] {return L"测试预览";};
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,1);
    const HWND window=preferences.hwnd();
    auto field=[&](int id){return preference::Field(window,id);};
    auto text=[&](int id) {
      const auto control=field(id);
      std::wstring value(GetWindowTextLengthW(control)+1,L'\0');
      value.resize(GetWindowTextW(control,value.data(),static_cast<int>(value.size())));return value;
    };
    auto bounds=[&](int id) { RECT r{}; GetWindowRect(field(id),&r); return r; };
    auto logical_font=[&](int id) {
      LOGFONTW font{};
      GetObjectW(reinterpret_cast<HFONT>(SendMessageW(field(id),WM_GETFONT,0,0)),sizeof(font),&font);
      return font;
    };
    auto preferences_key=[&](int vk) {
      MSG key{GetFocus(),WM_KEYDOWN,static_cast<WPARAM>(vk),1};
      if(!preferences.Translate(&key)){TranslateMessage(&key);DispatchMessageW(&key);}
      MSG pending{};
      while(PeekMessageW(&pending,nullptr,0,0,PM_REMOVE))
        if(!preferences.Translate(&pending)){TranslateMessage(&pending);DispatchMessageW(&pending);}
      key={GetFocus(),WM_KEYUP,static_cast<WPARAM>(vk),1};
      if(!preferences.Translate(&key)){TranslateMessage(&key);DispatchMessageW(&key);}
    };
    check(SendMessageW(field(31),LB_GETCOUNT,0,0)==100,"personal terms paginate instead of rendering an unbounded list");
    check(AccessibleName(field(preference::Query)).find(L"搜索词语")!=std::wstring::npos &&
          AccessibleName(field(preference::Code)).find(L"输入码")!=std::wstring::npos,
          "Windows accessibility exposes distinct search and input-code names");
    SendMessageW(window,WM_COMMAND,40,0);
    check(SendMessageW(field(31),LB_GETCOUNT,0,0)==100,"next page remains bounded");
    SetWindowTextW(field(32),L"轻音输入法"); SetWindowTextW(field(33),L"qingyin");
    SetFocus(field(33)); preferences_key(VK_RETURN);
    check(lexicon.Count()==231,"Enter in the word form invokes its page-specific save action");
    SetWindowTextW(field(30),L"轻音");
    check(SendMessageW(field(31),LB_GETCOUNT,0,0)==1,"search narrows personal terms");
    check(!IsWindowEnabled(field(39)) && !IsWindowEnabled(field(40)),"single-page search disables both pagination actions");
    SendMessageW(field(31),LB_SETCURSEL,0,0); SendMessageW(window,WM_COMMAND,MAKEWPARAM(31,LBN_SELCHANGE),0);
    check(IsWindowEnabled(field(preference::DeleteTerm)),"selecting a real search result enables Delete");
    ok=Save(window,output/"preferences-terms.png") && ok;
    SendMessageW(window,WM_COMMAND,37,0); check(lexicon.Search("轻音").empty(),"native delete removes selected term");
    SendMessageW(window,WM_COMMAND,38,0); check(lexicon.Search("轻音").size()==1,"native undo restores the deleted term");
    SendMessageW(field(preference::Terms),LB_SETCURSEL,0,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(preference::Terms,LBN_SELCHANGE),0);
    SendMessageW(field(preference::DeleteTerm),BM_CLICK,0,0);
    lexicon.SaveTerm("轻音输入法","newcode",std::time(nullptr));
    SendMessageW(field(preference::Undo),BM_CLICK,0,0);
    check(lexicon.Search("newcode").size()==1 && text(preference::StatusLine).find(L"同名词语")!=std::wstring::npos,
          "undo leaves a recreated word and its new input code intact");
    lexicon.Delete("轻音输入法"); SendMessageW(field(preference::Undo),BM_CLICK,0,0);
    check(lexicon.Search("qingyin").size()==1,"undo remains available after the naming conflict is resolved");
    SetWindowTextW(field(preference::Query),L"page");
    SendMessageW(field(preference::Next),BM_CLICK,0,0);
    check(SendMessageW(field(preference::Terms),LB_GETCOUNT,0,0)==1,"filtered final page contains one term");
    SendMessageW(field(preference::Terms),LB_SETCURSEL,0,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(preference::Terms,LBN_SELCHANGE),0);
    SendMessageW(field(preference::DeleteTerm),BM_CLICK,0,0);
    check(SendMessageW(field(preference::Terms),LB_GETCOUNT,0,0)==100 &&
          !IsWindowEnabled(field(preference::Previous)) && !IsWindowEnabled(field(preference::Next)) &&
          !IsWindowVisible(field(preference::TermsEmpty)),"deleting the only term on the final page reveals the previous results");
    SendMessageW(field(preference::Undo),BM_CLICK,0,0);
    SetWindowTextW(field(30),L"没有这个匹配"); ok=Save(window,output/"preferences-empty-search.png") && ok;
    for(int page:{0,2,3,4,5}) { preferences.Open(&lexicon,{},UiCurrentSkin(),actions,page); ok=Save(window,output/("preferences-page-"+std::to_string(page)+".png")) && ok; }
    const auto dpi=UiDpi(window);
    const auto title_font=logical_font(preference::PageTitle),section_font=logical_font(115);
    check(title_font.lfWeight==FW_BOLD && title_font.lfHeight==-UiPx(24,dpi) &&
          section_font.lfWeight==FW_NORMAL && section_font.lfHeight==-UiPx(16,dpi),
          "settings use a 24-DIP bold title and 16-DIP regular section headings");
    check(logical_font(116).lfHeight==-UiPx(14,dpi) && logical_font(128).lfHeight==-UiPx(14,dpi),
          "body and explanatory text both measure 14 DIP");
    auto copy=bounds(116),action=bounds(preference::DataBackup);
    check(std::abs(action.top-copy.bottom-UiPx(12,dpi))<=1,"backup action follows the measured description by 12 DIP");
    RECT normal{},client{}; GetWindowRect(window,&normal); GetClientRect(window,&client);
    const int frame_w=normal.right-normal.left-client.right, frame_h=normal.bottom-normal.top-client.bottom;
    SetWindowPos(window,nullptr,0,0,frame_w+UiPx(864,dpi),frame_h+UiPx(630,dpi),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    ok=Save(window,output/"preferences-review-backup.png") && ok;
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,0);
    ok=Save(window,output/"preferences-review-input.png") && ok;
    const int previous_model_polls=model_polls,previous_diagnostic_polls=diagnostic_polls;
    SendMessageW(window,WM_TIMER,1,0);
    check(model_polls==previous_model_polls && diagnostic_polls==previous_diagnostic_polls,
          "input preferences do not poll hidden model files or diagnostics");
    SetWindowPos(window,nullptr,0,0,normal.right-normal.left,normal.bottom-normal.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    check(text(preference::HotkeyValue)==HotkeyLabel(DefaultHotkey()),"input settings show the configured voice shortcut");
    if(IsWindowVisible(field(preference::Navigation))) {
      SetFocus(field(preference::Navigation)); preferences_key(VK_DOWN);
      check(text(preference::PageTitle)==L"我的词语" && IsWindowVisible(field(30)),"arrow key in the sidebar opens the vocabulary page");
      preferences_key(VK_UP);
    }
    BYTE nav_keys[256]{}; nav_keys[VK_CONTROL]=0x80; SetKeyboardState(nav_keys);
    preferences_key(VK_TAB);
    check(text(preference::PageTitle)==L"我的词语","Ctrl+Tab switches settings pages");
    nav_keys[VK_SHIFT]=0x80; SetKeyboardState(nav_keys); preferences_key(VK_TAB); SetKeyboardState(clear_keyboard);
    check(text(preference::PageTitle)==L"输入偏好","Ctrl+Shift+Tab returns to the preceding settings page");
    SetFocus(field(preference::Navigation));
    MSG mnemonic{GetFocus(),WM_SYSCHAR,L's',0x20000001};
    const bool accessed=preferences.Translate(&mnemonic);
    check(accessed && GetFocus()==field(preference::Skin),"Alt+S still focuses the native appearance selector");
    check(AccessibleName(field(preference::Skin)).find(L"输入浮层外观")!=std::wstring::npos,
          "Windows accessibility exposes the appearance selector label");
    check(text(102).find(L"(&S)")!=std::wstring::npos && !(SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEACCEL),
          "keyboard navigation exposes the original accelerator while keeping its accessible caption");
    ok=Save(window,output/"preferences-access-keys.png") && ok;
    MSG mouse{field(preference::PageTitle),WM_LBUTTONDOWN,0,0};
    preferences.Translate(&mouse);
    BOOL always_show=FALSE; SystemParametersInfoW(SPI_GETKEYBOARDCUES,0,&always_show,0);
    check(always_show || (SendMessageW(window,WM_QUERYUISTATE,0,0)&UISF_HIDEACCEL),
          "mouse input hides optional key cues unless Windows requests them");
    SetFocus(field(preference::Skin)); SendMessageW(field(preference::Skin),CB_SHOWDROPDOWN,TRUE,0);
    preferences_key(VK_ESCAPE);
    check(IsWindow(window) && !SendMessageW(field(preference::Skin),CB_GETDROPPEDSTATE,0,0),"Esc dismisses an open skin selector and keeps settings open");
    auto select_skin=[&](UiSkin skin) {
      SendMessageW(field(preference::Skin),CB_SETCURSEL,static_cast<int>(skin),0);
      SendMessageW(GetParent(field(preference::Skin)),WM_COMMAND,MAKEWPARAM(preference::Skin,CBN_SELCHANGE),reinterpret_cast<LPARAM>(field(preference::Skin)));
    };
    select_skin(UiSkin::Daylight); ok=Save(window,output/"preferences-skin-daylight.png") && ok;
    select_skin(UiSkin::DeepSpace); ok=Save(window,output/"preferences-skin-deep-space.png") && ok;
    check(skin_saves==0,"browsing the skin preview does not save a preference");
    SendMessageW(field(preference::Learning),BM_SETCHECK,BST_UNCHECKED,0);
    SendMessageW(field(preference::SaveOptions),BM_CLICK,0,0);
    check(skin_saves==1 && LoadSkin((output/"appearance.json").wstring())==UiSkin::DeepSpace &&
          !LoadInputPreferences((output/"input-preferences.ini").wstring()).learning,"Save persists the selected skin and input settings to the fixture");
    preferences.ReloadInputs({},UiSkin::Titanium);
    check(SendMessageW(field(preference::Skin),CB_GETCURSEL,0,0)==0 &&
          SendMessageW(field(preference::Learning),BM_GETCHECK,0,0)==BST_CHECKED,"reloading preferences restores the controls behind the preview");
    SetWindowTextW(field(preference::Term),L"恢复前的草稿"); SetWindowTextW(field(preference::Code),L"old");
    InputPreferences missing; missing.microphone=L"fixture-disconnected-microphone";
    preferences.ReloadInputs(missing,UiSkin::Titanium);
    check(text(preference::Term).empty() && text(preference::Code).empty(),"restoring a backup clears the old term editor");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,2);
    const int before_missing=input_saves;
    SendMessageW(field(preference::SaveOptions),BM_CLICK,0,0);
    check(input_saves==before_missing && text(preference::StatusLine).find(L"麦克风")!=std::wstring::npos,
          "the device page requires replacing a disconnected microphone before saving");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,0);
    const int before_appearance=input_saves;
    SendMessageW(field(preference::SaveOptions),BM_CLICK,0,0);
    check(input_saves==before_appearance+1 && saved.microphone==missing.microphone,"appearance changes preserve a disconnected device preference");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,2);
    const int before_replacement=input_saves;
    SendMessageW(field(preference::Devices),CB_SETCURSEL,0,0);
    SendMessageW(field(preference::RefreshDevices),BM_CLICK,0,0);
    SendMessageW(field(preference::SaveOptions),BM_CLICK,0,0);
    check(input_saves==before_replacement+1 && saved.microphone.empty(),"refresh keeps the selected system microphone and permits saving it");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,3);
    check(text(preference::PracticeText)==L"测试预览","the diagnostic page immediately refreshes its native practice preview");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,4);
    check(SendMessageW(field(60),CB_GETCOUNT,0,0)==3,"new users see exactly three curated models");
    check(SendMessageW(field(preference::ModelIdle),BM_GETCHECK,0,0)==(selected.idle_minutes?BST_CHECKED:BST_UNCHECKED),
          "the model idle preference is displayed before activating any model");
    check(!field(64) && !IsWindowEnabled(field(67)),"model page has one activation action and no idle cancellation");
    SendMessageW(field(60),CB_SETCURSEL,1,0);SendMessageW(window,WM_COMMAND,MAKEWPARAM(60,CBN_SELCHANGE),0);
    check(!IsWindowEnabled(field(69)),"unsupported model hints are disabled");
    check(text(62).find(L"当前模型：Qwen3-ASR")==0,"browsing a model does not label it as the active model");
    SendMessageW(window,WM_COMMAND,63,0);check(uses==1 && selected.model==SpeechModel::SenseVoice,"model choice invokes the activation workflow");
    selected.directories[1]=L"C:\\validated-after-opening";
    SendMessageW(window,WM_COMMAND,63,0);
    check(last_requested.directories[1]==selected.directories[1],"activation uses validated settings instead of a stale form snapshot");
    task=ModelTaskState::Running;SetFocus(field(63));SendMessageW(window,WM_TIMER,1,0);
    check(!IsWindowEnabled(field(63)) && !IsWindowEnabled(field(65)) && IsWindowEnabled(field(67)),"busy task disables duplicate activation but leaves cancellation available");
    check(GetFocus()==field(67),"focus moves to cancellation when activation becomes busy");
    task=ModelTaskState::Queued;SendMessageW(window,WM_TIMER,1,0);
    check(IsWindowEnabled(field(63)) && IsWindowEnabled(field(67)),"a queued selection may be replaced or cancelled");
    task=ModelTaskState::Idle;SendMessageW(window,WM_TIMER,1,0);
    check(!IsWindowEnabled(field(67)) && GetFocus()==field(63),"completion restores focus to an available action");
    SendMessageW(field(62),EM_SETSEL,0,5);SendMessageW(window,WM_TIMER,1,0);
    DWORD start=0,end=0;SendMessageW(field(62),EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
    check(start==0 && end==5,"unchanged status polling preserves the user's text selection");
    SetFocus(field(62));preferences_key(VK_TAB);
    check(GetFocus()==field(63),"Tab leaves the read-only model status");
    reject=true;SendMessageW(window,WM_COMMAND,63,0);reject=false;
    check(text(11)==failure && (GetWindowLongPtrW(field(11),GWL_STYLE)&WS_VSCROLL),"long errors and backup paths stay selectable and scrollable");
    SetFocus(field(11));preferences_key(VK_TAB);
    check(GetFocus()==field(IDCANCEL),"Tab leaves a long operation message for Close");
    pending_model=true;
    SendMessageW(field(preference::ModelUse),BM_CLICK,0,0);
    task=ModelTaskState::Idle; model_result=L"验证失败。请重新选择模型文件。";
    SendMessageW(window,WM_TIMER,1,0);
    check(text(preference::StatusLine).find(model_result)!=std::wstring::npos,
          "the model footer replaces the pending message with the completed operation result");
    model_result.clear(); SendMessageW(field(preference::ModelUse),BM_CLICK,0,0);
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,3);
    const auto practice_hint=text(preference::StatusLine);
    task=ModelTaskState::Idle; pending_model=false; SendMessageW(window,WM_TIMER,1,0);
    check(text(preference::StatusLine)==practice_hint,"model completion leaves the diagnostic page feedback intact");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,4);
    check(text(preference::StatusLine).find(L"已准备")!=std::wstring::npos,"returning to the model page shows its pending completion");
    pending_model=true; requested_task=ModelTaskState::Queued;
    SendMessageW(field(preference::ModelUse),BM_CLICK,0,0);
    reject=true; SendMessageW(field(preference::ModelUse),BM_CLICK,0,0);
    task=ModelTaskState::Idle; SendMessageW(window,WM_TIMER,1,0);
    check(text(preference::StatusLine)==failure,"a rejected queue replacement keeps its synchronous failure feedback");
    pending_model=false; reject=false; preferences.Open(&lexicon,{},UiCurrentSkin(),actions,3);
    SetFocus(field(75));preferences_key(VK_TAB);
    check(GetFocus()==field(54),"Tab leaves first-use output for diagnostics");
    preferences_key(VK_TAB);check(GetFocus()==field(55),"Tab leaves diagnostics for Copy");
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,0);
    for(UINT dpi:{144u,192u}) {
      SendMessageW(window,WM_DPICHANGED,MAKELONG(dpi,dpi),0);
      SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0);
      RECT button{},client{}; GetWindowRect(field(23),&button); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&button),2); GetClientRect(window,&client);
      check(button.top>=0 && button.bottom<=client.bottom,"settings save action remains reachable at high DPI");
      ok=Save(window,output/("preferences-bottom-dpi-"+std::to_string(dpi)+".png")) && ok;
    }
    SendMessageW(window,WM_DPICHANGED,MAKELONG(96,96),0);
    preferences.Open(&lexicon,{},UiCurrentSkin(),actions,5);
    SetFocus(field(preference::DataRestore));
    SetWindowPos(window,nullptr,0,0,470,310,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    const auto pane_bounds=bounds(preference::Content),focused_bounds=bounds(preference::DataRestore);
    check(GetFocus()==field(preference::DataRestore) && focused_bounds.top>=pane_bounds.top && focused_bounds.bottom<=pane_bounds.bottom,
          "shrinking the window keeps the focused restore action in view");
    SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0);
    const int old_scroll=GetScrollPos(field(preference::Content),SB_VERT);
    const auto first_top=bounds(116).top;
    SendMessageW(window,WM_VSCROLL,SB_TOP,0);
    check(old_scroll>0 && bounds(116).top==first_top+old_scroll && GetFocus()==field(preference::DataRestore),
          "manual scrolling moves clipped children without changing keyboard focus");
    ShowWindow(window,SW_MINIMIZE); preferences.Open(&lexicon,{},UiCurrentSkin(),actions,2);
    check(preferences.hwnd()==window && !IsIconic(window) && text(preference::PageTitle)==L"语音设备",
          "reopening minimized settings restores the same window on the requested page");
    if(IsIconic(window)) ShowWindow(window,SW_RESTORE);
    SetWindowPos(window,nullptr,0,0,470,420,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    preferences.Open(&lexicon,{},UiSkin::Titanium,actions,5);
    const auto narrow_copy=bounds(116);
    check(narrow_copy.bottom-narrow_copy.top>MulDiv(copy.bottom-copy.top,96,dpi) &&
          std::abs(bounds(preference::DataBackup).top-narrow_copy.bottom-12)<=1,
          "a wrapped description grows and its action follows it in a narrow window");
    check(IsWindowVisible(field(preference::CompactNavigation)) && !IsWindowVisible(field(preference::Navigation)),
          "narrow windows use a keyboard-accessible page selector");
    auto compact=field(preference::CompactNavigation);
    SendMessageW(compact,CB_SETCURSEL,2,0);
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(preference::CompactNavigation,CBN_SELCHANGE),reinterpret_cast<LPARAM>(compact));
    check(text(preference::PageTitle)==L"语音设备" && IsWindowVisible(field(50)),"compact selector opens the requested settings");
    RECT before{},after{}; GetWindowRect(field(preference::SaveOptions),&before);
    SendMessageW(window,WM_VSCROLL,SB_BOTTOM,0); GetWindowRect(field(preference::SaveOptions),&after);
    check(EqualRect(&before,&after) && GetScrollPos(field(preference::Content),SB_VERT)>0,"the action bar stays fixed when the form scrolls");
    ok=Save(window,output/"preferences-compact.png") && ok;
    SetEnvironmentVariableW(L"VOICEIME_TEST_HIGH_CONTRAST",L"1");
    SendMessageW(window,WM_SETTINGCHANGE,0,0); preferences.Open(&lexicon,{},UiSkin::Titanium,actions,0);
    if(!always_show) {
      SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEACCEL),0);
      const auto button=field(preference::SaveOptions);
      const auto hidden=ButtonPixels(button);
      SetWindowTextW(button,L"保存设置");
      const auto plain=ButtonPixels(button);
      SetWindowTextW(button,L"保存设置(&A)");
      SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEACCEL),0);
      const auto shown=ButtonPixels(button);
      check(!plain.empty() && hidden==plain && shown!=plain,"high-contrast button hides and reveals the mnemonic while keeping a native caption");
      SendMessageW(window,WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEACCEL),0);
    }
    ok=Save(window,output/"preferences-high-contrast.png") && ok;
    SetEnvironmentVariableW(L"VOICEIME_TEST_HIGH_CONTRAST",nullptr);
    preferences.Close();
    preferences.Open(nullptr,{},UiSkin::Titanium,actions,1);
    const auto unavailable=preferences.hwnd();
    wchar_t unavailable_hint[160]{};
    GetWindowTextW(preference::Field(unavailable,preference::StatusLine),unavailable_hint,160);
    check(!IsWindowEnabled(preference::Field(unavailable,preference::SaveTerm)) &&
          !IsWindowVisible(preference::Field(unavailable,preference::Terms)) &&
          GetWindowTextLengthW(preference::Field(unavailable,preference::Count))==0 &&
          std::wstring(unavailable_hint).find(L"词库不可用")!=std::wstring::npos,
          "dictionary-open failure has its own state and never claims an empty dictionary");
    ok=Save(unavailable,output/"preferences-terms-unavailable.png") && ok;
    preferences.Close();
    {
      const auto path=(output/"settings-read-test.sqlite").u8string();
      Lexicon stored(path); stored.SaveTerm("词库恢复测试","ciku",std::time(nullptr));
      sqlite3* other=nullptr;
      const bool locked=sqlite3_open(path.c_str(),&other)==SQLITE_OK &&
          sqlite3_exec(other,"BEGIN EXCLUSIVE",nullptr,nullptr,nullptr)==SQLITE_OK;
      check(locked,"fixture exclusively locks its own test database");
      if(locked) {
        preferences.Open(&stored,{},UiSkin::Titanium,actions,1);
        const auto dialog=preferences.hwnd();
        GetWindowTextW(preference::Field(dialog,preference::StatusLine),unavailable_hint,160);
        check(std::wstring(unavailable_hint).find(L"暂时无法读取")!=std::wstring::npos &&
              !IsWindowEnabled(preference::Field(dialog,preference::SaveTerm)),"a locked dictionary shows a read error when first opened");
        sqlite3_exec(other,"ROLLBACK",nullptr,nullptr,nullptr);
        preferences.Open(&stored,{},UiSkin::Titanium,actions,0);
        preferences.Open(&stored,{},UiSkin::Titanium,actions,1);
        check(IsWindowVisible(preference::Field(dialog,preference::Terms)) &&
              SendMessageW(preference::Field(dialog,preference::Terms),LB_GETCOUNT,0,0)==1 &&
              IsWindowEnabled(preference::Field(dialog,preference::SaveTerm)),"returning to terms recovers after the database becomes readable");
        preferences.Close();
      }
      sqlite3_close(other);
    }
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
