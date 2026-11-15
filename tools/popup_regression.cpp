#include "candidate_bar.hpp"
#include "confirm_box.hpp"
#include "ui_theme.hpp"
#include <objbase.h>
#include <cstdio>
#include <map>

namespace {
struct Trace { WNDPROC original=nullptr; RECT previous{}; int moves=0,paints=0,shows=0; };
std::map<HWND,Trace> traces;
LRESULT CALLBACK Observe(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
  auto& trace=traces.at(hwnd);
  if (message==WM_WINDOWPOSCHANGED) {
    RECT rect{}; GetWindowRect(hwnd,&rect);
    if (!EqualRect(&rect,&trace.previous)) ++trace.moves;
    trace.previous=rect;
  }
  if (message==WM_PAINT) ++trace.paints;
  if (message==WM_SHOWWINDOW) ++trace.shows;
  return CallWindowProcW(trace.original,hwnd,message,wp,lp);
}
void Watch(HWND hwnd) {
  auto& trace=traces[hwnd]; trace={}; GetWindowRect(hwnd,&trace.previous);
  trace.original=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Observe)));
}
void Unwatch(HWND hwnd) { SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(traces.at(hwnd).original)); }
void Flush(HWND hwnd) { UpdateWindow(hwnd); MSG m{}; while (PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);} }
LRESULT Hook(HWND h,UINT m,WPARAM w,LPARAM l,void*) { return DefWindowProcW(h,m,w,l); }
}
bool CheckSkin(UiSkin skin) {
  UiDpiScope dpi;
  const auto work=UiWorkArea({0,0,1,1});
  const Rect caret{work.x+work.w-55,work.y+work.h-65,1,24};
  ConfirmBox voice; voice.Create(GetModuleHandleW(nullptr),Hook,nullptr);
  voice.SetSkin(skin);
  voice.ShowText("正在输入一段稳定的文字",caret,work,Phase::Listening); Flush(voice.hwnd()); Watch(voice.hwnd());
  for(int i=0;i<20;++i){ voice.ShowText("正在输入一段稳定的文字",caret,work,Phase::Listening); Flush(voice.hwnd()); }
  const auto voice_trace=traces.at(voice.hwnd()); Unwatch(voice.hwnd());
  RECT voice_edge{}; GetWindowRect(voice.hwnd(),&voice_edge);
  const Rect moved_caret{work.x+80,work.y+120,1,24};
  Watch(voice.hwnd()); voice.ShowText("正在输入一段稳定的文字",moved_caret,work,Phase::Listening); Flush(voice.hwnd());
  const auto voice_move=traces.at(voice.hwnd()); Unwatch(voice.hwnd()); voice.Hide();
  CandidateBar candidates; RimeView view; view.composing=true; view.preedit="nihao"; view.page={"你好","拟好","你","呢","泥"};
  candidates.SetSkin(skin);
  candidates.Show(caret,work,view,InputMode::Chinese); Flush(candidates.hwnd()); Watch(candidates.hwnd());
  for(int i=0;i<20;++i){ candidates.Show(caret,work,view,InputMode::Chinese); Flush(candidates.hwnd()); }
  const auto candidate_trace=traces.at(candidates.hwnd()); Unwatch(candidates.hwnd());
  RECT candidate_edge{}; GetWindowRect(candidates.hwnd(),&candidate_edge);
  Watch(candidates.hwnd()); candidates.Show(moved_caret,work,view,InputMode::Chinese); Flush(candidates.hwnd());
  const auto candidate_move=traces.at(candidates.hwnd()); Unwatch(candidates.hwnd()); candidates.Hide();
  SendMessageW(voice.hwnd(),WM_DISPLAYCHANGE,0,0);
  SendMessageW(candidates.hwnd(),WM_DISPLAYCHANGE,0,0);
  SetEnvironmentVariableW(L"VOICEIME_TEST_SKIN",L"deep-space");
  SendMessageW(voice.hwnd(),UiSkinChangedMessage(),0,0);
  SendMessageW(candidates.hwnd(),UiSkinChangedMessage(),0,0);
  const bool notification=voice.skin()==UiSkin::DeepSpace && candidates.skin()==UiSkin::DeepSpace;
  SetEnvironmentVariableW(L"VOICEIME_TEST_SKIN",nullptr);
  const bool stays_hidden=!IsWindowVisible(voice.hwnd()) && !IsWindowVisible(candidates.hwnd());
  std::printf("voice repeated_identical_updates=20 physical_moves=%d paints=%d visibility_changes=%d\n",voice_trace.moves,voice_trace.paints,voice_trace.shows);
  std::printf("candidate repeated_identical_updates=20 physical_moves=%d paints=%d visibility_changes=%d\n",candidate_trace.moves,candidate_trace.paints,candidate_trace.shows);
  const bool stable=voice_trace.moves==0 && voice_trace.paints==0 && voice_trace.shows==0 && candidate_trace.moves==0 && candidate_trace.paints==0 && candidate_trace.shows==0;
  const bool moves_once=voice_move.moves==1 && candidate_move.moves==1;
  const bool avoids_caret=voice_edge.bottom<=caret.y && candidate_edge.bottom<=caret.y;
  std::printf("moved_caret voice_moves=%d candidate_moves=%d\n",voice_move.moves,candidate_move.moves);
  std::puts(moves_once ? "PASS moving caret causes one final placement" : "FAIL moving caret causes redundant placement");
  std::puts(avoids_caret ? "PASS popups flip above a caret near the screen bottom" : "FAIL popup overlaps the bottom caret");
  std::puts(stays_hidden ? "PASS display change does not reopen dismissed popups" : "FAIL display change reopens dismissed popup");
  std::puts(stable ? "PASS unchanged popups stay still without repaint" : "FAIL unchanged popup jumps or repaints");
  std::puts(notification ? "PASS appearance notification updates both controls while hidden" : "FAIL appearance notification did not update both controls");
  return stable && moves_once && avoids_caret && stays_hidden && notification;
}
int main() {
  SetProcessDPIAware(); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  bool ok=true;
  for (auto skin : {UiSkin::Titanium,UiSkin::Daylight,UiSkin::DeepSpace}) {
    std::printf("skin=%s\n",SkinId(skin));
    ok=CheckSkin(skin) && ok;
  }
  CoUninitialize(); return ok ? 0 : 1;
}
