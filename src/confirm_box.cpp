#include "confirm_box.hpp"
#include "ui_theme.hpp"
#include "recording.hpp"
#include <algorithm>
#include <cwchar>
#include <windowsx.h>

namespace {
std::wstring Wide(const std::string& text) {
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(count, L'\0');
  if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
  return result;
}
}  // namespace

ConfirmBox::~ConfirmBox() {
  hook_ = nullptr;
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
}

bool ConfirmBox::Create(HINSTANCE instance, Hook hook, void* self) {
  UiDpiScope awareness;
  if (!skin_loaded_) SetSkin(UiCurrentSkin());
  hook_ = hook;
  hook_self_ = self;
  WNDCLASSW klass{};
  klass.style = CS_DROPSHADOW;
  klass.lpfnWndProc = Proc;
  klass.hInstance = instance;
  klass.lpszClassName = L"VoiceImeConfirm";
  klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&klass);
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      klass.lpszClassName, L"语音输入", WS_POPUP,
      0, 0, 320, 112, nullptr, nullptr, instance, this);
  if (hwnd_ != nullptr) dpi_ = UiDpi(hwnd_);
  return hwnd_ != nullptr;
}

void ConfirmBox::ShowText(const std::string& utf8, Rect caret, Rect window_client, Phase phase, bool error, bool uncertain) {
  if (hwnd_ == nullptr) return;
  UiDpiScope coordinates;
  const auto next = Wide(utf8);
  if (next==text_ && phase==phase_ && error==error_ && uncertain==uncertain_ &&
      SameRect(caret,caret_) && SameRect(window_client,window_) && IsWindowVisible(hwnd_)) return;
  if (next != text_ || phase != phase_) scroll_ = 0;
  text_ = next;
  phase_ = phase;
  error_ = error;
  uncertain_ = uncertain;
  caret_ = caret;
  window_ = window_client;
  Layout();
}

void ConfirmBox::Layout() {
  if (hwnd_ == nullptr) return;
  UiDpiScope coordinates;
  const bool at_end=scroll_>0 && scroll_==ScrollLimit();
  const auto work = UiWorkArea(PlaceBox(caret_, window_, 1, 1, 16));
  const int pad = UiPx(16, dpi_);
  const int width = std::min(UiPx(380, dpi_), work.w);
  const int max_height = std::min(UiPx(320, dpi_), work.h);
  HDC dc = GetDC(hwnd_);
  HFONT hint_font=UiFont(12,dpi_);
  auto old_hint=SelectObject(dc,hint_font);
  RECT hint_bounds{0,0,std::max(1,width-pad*2),0};
  const auto hint=HintText();
  DrawTextW(dc,hint.c_str(),-1,&hint_bounds,DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc,old_hint); DeleteObject(hint_font);
  footer_height_=hint_bounds.bottom+UiPx(22,dpi_);
  int chrome=UiPx(44,dpi_)+footer_height_;
  HFONT font = UiFont(16, dpi_);
  const auto old = SelectObject(dc, font);
  TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
  line_height_ = std::max(1L,metrics.tmHeight);
  RECT measure{0, 0, std::max(1, width - pad * 2), 0};
  const std::wstring display = text_.empty() ?
      (phase_ == Phase::Listening ? L"请开始说话" : phase_ == Phase::Finalizing ? L"正在处理这段语音…" :
       phase_ == Phase::Stopped ? L"按 Enter 开始校准本次录音" : L"没有识别到语音") : text_;
  DrawTextW(dc, display.c_str(), static_cast<int>(display.size()), &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old); DeleteObject(font); ReleaseDC(hwnd_, dc);
  content_height_ = std::max(UiPx(25, dpi_), static_cast<int>(measure.bottom));
  if (content_height_+chrome>max_height) { footer_height_+=UiPx(20,dpi_); chrome+=UiPx(20,dpi_); }
  int height = std::min(content_height_ + chrome, max_height);
  if (content_height_ + chrome > max_height)
    height = chrome + std::max(1,(max_height-chrome)/line_height_) * line_height_;
  height = std::min(height,max_height);
  viewport_height_ = std::max(1, height - chrome);
  scroll_ = at_end ? ScrollLimit() : std::clamp(scroll_, 0, ScrollLimit());
  if (scroll_ != ScrollLimit()) scroll_ = scroll_ / line_height_ * line_height_;
  const auto placed = PlacePopup(caret_,window_,work,width,height,UiPx(6,dpi_),UiPx(16,dpi_));
  UiPlaceWindow(hwnd_,placed,dpi_,UiPalette(skin_).radius);
  const auto accessible=StatusText()+L"："+display+L"。"+HintText();
  if (accessible!=accessible_text_) {
    accessible_text_=accessible; SetWindowTextW(hwnd_,accessible.c_str());
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE,hwnd_,OBJID_WINDOW,CHILDID_SELF);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void ConfirmBox::Scroll(int position) {
  int next = std::clamp(position, 0, ScrollLimit());
  if (next != ScrollLimit()) next = next / line_height_ * line_height_;
  if (next == scroll_) return;
  scroll_ = next;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

RECT ConfirmBox::ScrollThumb() const {
  RECT client{}; GetClientRect(hwnd_,&client);
  const int height=std::min(viewport_height_,std::max(UiPx(24,dpi_),
      MulDiv(viewport_height_,viewport_height_,std::max(1,content_height_))));
  const int top=UiPx(44,dpi_)+MulDiv(scroll_,viewport_height_-height,std::max(1,ScrollLimit()));
  return {client.right-UiPx(8,dpi_),top,client.right-UiPx(4,dpi_),top+height};
}

void ConfirmBox::Hide() {
  if (dragging_scroll_) { dragging_scroll_=false; if(GetCapture()==hwnd_) ReleaseCapture(); }
  if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
}

void ConfirmBox::SetSkin(UiSkin skin) {
  skin_loaded_=true;
  if (skin_==skin) return;
  skin_=skin;
  if (!hwnd_) return;
  UiDpiScope coordinates;
  UiRoundWindow(hwnd_,dpi_,UiPalette(skin_).radius);
  InvalidateRect(hwnd_,nullptr,FALSE);
}

std::wstring ConfirmBox::StatusText() const {
  if(!activity_.empty()) return activity_;
  if (error_) return L"语音暂不可用";
  if (uncertain_) return L"写入结果待核实";
  if (phase_ == Phase::Committing) return L"正在写入正文";
  if (phase_ == Phase::Listening) {
    wchar_t elapsed[64]{};
    swprintf(elapsed,64,L"%ls · %02d:%02d / %02d:00",
        recording_seconds_>=kMaxRecordingSeconds-30?L"即将到时":L"正在听",
        recording_seconds_/60,recording_seconds_%60,kMaxRecordingSeconds/60);
    return elapsed;
  }
  if (phase_ == Phase::Stopped) return L"已到 10 分钟上限 · 收音已停止";
  if (phase_ == Phase::Finalizing) return L"正在校准文字";
  return text_.empty() ? L"录音已结束" : L"待确认";
}
std::wstring ConfirmBox::HintText() const {
  if (error_) return L"右键查看设置 / 编辑预览 · Esc 关闭";
  if (uncertain_) return L"请先核对正文 · 确认未写入后 Enter 重试 · Esc 关闭";
  if (phase_ == Phase::Committing) return L"正在等待应用确认，请稍候";
  if (phase_ == Phase::Listening) return quiet_capture_ ?
      L"暂未检测到声音，仍在收音 · Enter 停录并校准 · Esc 取消" :
      L"Enter 停录并校准 · 停顿不会结束 · Esc 取消";
  if (phase_ == Phase::Stopped) return L"草稿已保留 · Enter 开始校准 · Esc 取消";
  if (phase_ == Phase::Finalizing) return L"校准后可预览并确认 · Esc 取消";
  if (text_.empty()) return L"按语音快捷键重新开始 · Esc 关闭";
  return L"再次按 Enter 确认写入 · 右键编辑 / 复制 · Esc 取消";
}

void ConfirmBox::SetRecordingProgress(int seconds,bool quiet) {
  seconds=std::clamp(seconds,0,kMaxRecordingSeconds);
  if(recording_seconds_==seconds && quiet_capture_==quiet)return;
  recording_seconds_=seconds;quiet_capture_=quiet;
  if(hwnd_ && phase_==Phase::Listening && IsWindowVisible(hwnd_))Layout();
}

void ConfirmBox::SetActivity(std::wstring activity) {
  if(activity_==activity) return;
  activity_=std::move(activity);
  if(hwnd_) { accessible_text_.clear(); InvalidateRect(hwnd_,nullptr,FALSE); }
}

void ConfirmBox::Paint() {
  UiDpiScope coordinates;
  UiPaintFrame frame(hwnd_); HDC dc=frame.dc();
  const RECT client=frame.rect();
  const auto colors = UiPalette(skin_);
  UiSurface(dc,client,colors,dpi_);
  SetBkMode(dc, TRANSPARENT);
  const int pad = UiPx(16, dpi_);
  if (skin_==UiSkin::Titanium)
    UiFill(dc,{pad,UiPx(38,dpi_),client.right-pad,UiPx(38,dpi_)+std::max(1,UiPx(1,dpi_))},colors.rule);
  if (skin_!=UiSkin::Daylight)
    UiFill(dc,{pad,client.bottom-footer_height_+UiPx(5,dpi_),client.right-pad,client.bottom-footer_height_+UiPx(5,dpi_)+std::max(1,UiPx(1,dpi_))},colors.rule);
  if (skin_==UiSkin::Daylight) {
    HBRUSH badge=CreateSolidBrush(colors.icon_background);
    auto previous_brush=SelectObject(dc,badge); auto previous_pen=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,pad-UiPx(4,dpi_),UiPx(8,dpi_),pad+UiPx(22,dpi_),UiPx(34,dpi_),UiPx(14,dpi_),UiPx(14,dpi_));
    SelectObject(dc,previous_pen); SelectObject(dc,previous_brush); DeleteObject(badge);
  }
  const auto icon_color=(error_ || uncertain_) ? colors.secondary : colors.accent;
  HPEN pen=CreatePen(PS_SOLID,std::max(1,UiPx(1,dpi_)),icon_color);
  auto old_pen=SelectObject(dc,pen); auto old_brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
  const int icon_x=pad,icon_y=UiPx(13,dpi_);
  if (!error_ && !uncertain_ && phase_==Phase::Pending && !text_.empty()) {
    MoveToEx(dc,icon_x+UiPx(2,dpi_),icon_y+UiPx(8,dpi_),nullptr);
    LineTo(dc,icon_x+UiPx(6,dpi_),icon_y+UiPx(12,dpi_));
    LineTo(dc,icon_x+UiPx(15,dpi_),icon_y+UiPx(3,dpi_));
  } else if (phase_==Phase::Finalizing || phase_==Phase::Committing) {
    for (int y : {5,13}) { MoveToEx(dc,icon_x,icon_y+UiPx(y,dpi_),nullptr); LineTo(dc,icon_x+UiPx(16,dpi_),icon_y+UiPx(y,dpi_)); }
    MoveToEx(dc,icon_x+UiPx(5,dpi_),icon_y+UiPx(2,dpi_),nullptr); LineTo(dc,icon_x+UiPx(5,dpi_),icon_y+UiPx(8,dpi_));
    MoveToEx(dc,icon_x+UiPx(11,dpi_),icon_y+UiPx(10,dpi_),nullptr); LineTo(dc,icon_x+UiPx(11,dpi_),icon_y+UiPx(16,dpi_));
  } else {
  RoundRect(dc,icon_x+UiPx(5,dpi_),icon_y,icon_x+UiPx(11,dpi_),icon_y+UiPx(10,dpi_),UiPx(6,dpi_),UiPx(6,dpi_));
  Arc(dc,icon_x+UiPx(2,dpi_),icon_y+UiPx(4,dpi_),icon_x+UiPx(14,dpi_),icon_y+UiPx(14,dpi_),icon_x+UiPx(2,dpi_),icon_y+UiPx(8,dpi_),icon_x+UiPx(14,dpi_),icon_y+UiPx(8,dpi_));
  MoveToEx(dc,icon_x+UiPx(8,dpi_),icon_y+UiPx(13,dpi_),nullptr); LineTo(dc,icon_x+UiPx(8,dpi_),icon_y+UiPx(17,dpi_));
  MoveToEx(dc,icon_x+UiPx(4,dpi_),icon_y+UiPx(17,dpi_),nullptr); LineTo(dc,icon_x+UiPx(12,dpi_),icon_y+UiPx(17,dpi_));
  }
  SelectObject(dc,old_brush); SelectObject(dc,old_pen); DeleteObject(pen);
  HBRUSH dot=CreateSolidBrush(icon_color);
  old_brush=SelectObject(dc,dot); old_pen=SelectObject(dc,GetStockObject(NULL_PEN));
  const int dot_x=client.right-pad-UiPx(4,dpi_);
  if (skin_==UiSkin::Titanium) Rectangle(dc,dot_x,UiPx(20,dpi_),dot_x+UiPx(4,dpi_),UiPx(24,dpi_));
  else Ellipse(dc,dot_x,UiPx(20,dpi_),dot_x+UiPx(4,dpi_),UiPx(24,dpi_));
  SelectObject(dc,old_brush); SelectObject(dc,old_pen); DeleteObject(dot);
  HFONT small = UiFont(13, dpi_);
  auto old = SelectObject(dc, small);
  SetTextColor(dc, colors.secondary);
  const auto status = StatusText();
  RECT top{pad+UiPx(28,dpi_), UiPx(11,dpi_), client.right-pad-UiPx(12,dpi_), UiPx(33,dpi_)};
  DrawTextW(dc, status.c_str(), -1, &top, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  SelectObject(dc, old); DeleteObject(small);

  const int saved = SaveDC(dc);
  RECT body{pad, UiPx(44,dpi_), client.right-pad, client.bottom-footer_height_};
  IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
  body.top -= scroll_;
  body.bottom = body.top + content_height_;
  HFONT font = UiFont(16,dpi_);
  SelectObject(dc, font);
  SetTextColor(dc, colors.text);
  const std::wstring display = text_.empty() ?
      (phase_ == Phase::Listening ? L"请开始说话" : phase_ == Phase::Finalizing ? L"正在处理这段语音…" :
       phase_ == Phase::Stopped ? L"按 Enter 开始校准本次录音" : L"没有识别到语音") : text_;
  DrawTextW(dc, display.c_str(), static_cast<int>(display.size()), &body, DT_WORDBREAK | DT_NOPREFIX);
  RestoreDC(dc, saved); DeleteObject(font);

  if (ScrollLimit()>0) {
    const auto thumb=ScrollThumb();
    HBRUSH brush=CreateSolidBrush(colors.secondary);
    const auto previous_brush=SelectObject(dc,brush);
    const auto previous_pen=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,thumb.left,thumb.top,thumb.right,thumb.bottom,UiPx(4,dpi_),UiPx(4,dpi_));
    SelectObject(dc,previous_pen); SelectObject(dc,previous_brush); DeleteObject(brush);
  }

  small = UiFont(12,dpi_);
  old = SelectObject(dc, small);
  SetTextColor(dc, colors.secondary);
  const auto hint = HintText();
  RECT footer{pad, client.bottom-footer_height_+UiPx(12,dpi_), client.right-pad, client.bottom-UiPx(ScrollLimit()>0 ? 28 : 8,dpi_)};
  DrawTextW(dc, hint.c_str(), -1, &footer, DT_WORDBREAK | DT_NOPREFIX);
  if (ScrollLimit()>0) {
    RECT scroll_hint{pad,client.bottom-UiPx(26,dpi_),client.right-pad,client.bottom-UiPx(8,dpi_)};
    DrawTextW(dc,scroll_==ScrollLimit() ? L"已到末尾" : L"滚轮查看全文",-1,&scroll_hint,DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  }
  SelectObject(dc, old); DeleteObject(small);
}

LRESULT CALLBACK ConfirmBox::Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<ConfirmBox*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<ConfirmBox*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  }
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (self != nullptr) {
    if (message==UiSkinChangedMessage()) { self->SetSkin(UiCurrentSkin()); return 0; }
    if (message == WM_PAINT) { self->Paint(); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_DPICHANGED) {
      self->dpi_ = HIWORD(wp); self->Layout();
      if(self->hook_)self->hook_(hwnd,message,wp,lp,self->hook_self_);
      return 0;
    }
    if (message == WM_DISPLAYCHANGE) {
      if(IsWindowVisible(hwnd)) self->Layout();
      if(self->hook_)self->hook_(hwnd,message,wp,lp,self->hook_self_);
      return 0;
    }
    if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
      InvalidateRect(hwnd,nullptr,FALSE);
      if(self->hook_)self->hook_(hwnd,message,wp,lp,self->hook_self_);
      return 0;
    }
    if (message == WM_MOUSEWHEEL) {
      self->Scroll(self->scroll_ - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * self->line_height_ * 3);
      return 0;
    }
    if (message == WM_LBUTTONDOWN && self->ScrollLimit()>0) {
      RECT client{}; GetClientRect(hwnd,&client);
      const POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
      const auto thumb=self->ScrollThumb();
      if (point.x>=client.right-UiPx(16,self->dpi_) && point.y>=UiPx(44,self->dpi_) &&
          point.y<client.bottom-self->footer_height_) {
        if (point.y>=thumb.top && point.y<thumb.bottom) {
          self->dragging_scroll_=true; self->drag_y_=point.y; self->drag_scroll_=self->scroll_; SetCapture(hwnd);
        } else self->Scroll(self->scroll_+(point.y<thumb.top ? -1 : 1)*self->viewport_height_);
        return 0;
      }
    }
    if (message == WM_MOUSEMOVE && self->dragging_scroll_) {
      const auto thumb=self->ScrollThumb();
      const int travel=self->viewport_height_-(thumb.bottom-thumb.top);
      self->Scroll(self->drag_scroll_+MulDiv(GET_Y_LPARAM(lp)-self->drag_y_,self->ScrollLimit(),std::max(1,travel)));
      return 0;
    }
    if (message == WM_LBUTTONUP && self->dragging_scroll_) {
      self->dragging_scroll_=false; if(GetCapture()==hwnd) ReleaseCapture(); return 0;
    }
    if (message == WM_CAPTURECHANGED) self->dragging_scroll_=false;
    if (message == WM_VSCROLL) {
      int next = self->scroll_;
      switch (LOWORD(wp)) {
        case SB_LINEUP: next -= self->line_height_; break;
        case SB_LINEDOWN: next += self->line_height_; break;
        case SB_PAGEUP: next -= self->viewport_height_; break;
        case SB_PAGEDOWN: next += self->viewport_height_; break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = self->content_height_; break;
      }
      self->Scroll(next); return 0;
    }
    if (message == WM_NCDESTROY) self->hwnd_ = nullptr;
    if (self->hook_ != nullptr && message != WM_NCCREATE) return self->hook_(hwnd,message,wp,lp,self->hook_self_);
  }
  return DefWindowProcW(hwnd,message,wp,lp);
}
