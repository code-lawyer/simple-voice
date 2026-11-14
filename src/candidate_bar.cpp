#include "candidate_bar.hpp"

#include <string>

namespace {

constexpr int kSettingsWidth = 36;
constexpr int kBarHeight = 28;
constexpr int kPad = 8;
constexpr int kItemPad = 12;

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  return wide;
}

std::wstring CandidateLabel(int index, const std::string& utf8) {
  return std::to_wstring(index + 1) + L" " + Utf8ToWide(utf8);
}

HFONT MakeFont() {
  return CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
  HGDIOBJ old = SelectObject(dc, font);
  RECT measure{0, 0, 0, 0};
  DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, old);
  return measure.right;
}

int BarWidth(HDC dc, HFONT font, const RimeView& view) {
  int width = kPad;
  for (int i = 0; i < static_cast<int>(view.page.size()); ++i) {
    width += TextWidth(dc, font, CandidateLabel(i, view.page[static_cast<std::size_t>(i)])) + kItemPad;
  }
  width += kSettingsWidth;
  if (width < kSettingsWidth) width = kSettingsWidth;
  return width;
}

HINSTANCE ThisModule() {
  HMODULE module = nullptr;
  static int marker = 0;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&marker), &module);
  return module;
}

Rect WorkArea(Rect placed) {
  RECT screen{placed.x, placed.y, placed.x + placed.w, placed.y + placed.h};
  const HMONITOR monitor = MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
    return Rect{info.rcWork.left, info.rcWork.top, info.rcWork.right - info.rcWork.left,
                info.rcWork.bottom - info.rcWork.top};
  }
  RECT fallback{};
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0)) {
    return Rect{fallback.left, fallback.top, fallback.right - fallback.left, fallback.bottom - fallback.top};
  }
  return placed;
}

}  // namespace

CandidateBar::~CandidateBar() {
  if (hwnd_ != nullptr) DestroyWindow(hwnd_);
}

void CandidateBar::EnsureWindow() {
  if (hwnd_ != nullptr) return;
  const HINSTANCE instance = ThisModule();
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = Proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = L"VoiceImeCandidates";
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hbrBackground = CreateSolidBrush(RGB(32, 32, 32));
  RegisterClassW(&window_class);
  hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VoiceImeCandidates",
                           L"", WS_POPUP, 0, 0, 200, kBarHeight, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) return;
  SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
}

void CandidateBar::Show(Rect caret_screen, Rect window_client_screen, const RimeView& view, InputMode mode) {
  view_ = view;
  mode_ = mode;
  if (!view.composing) {
    Hide();
    return;
  }
  EnsureWindow();
  if (hwnd_ == nullptr) return;
  HDC screen = GetDC(hwnd_);
  HFONT font = MakeFont();
  const int box_w = BarWidth(screen, font, view_);
  DeleteObject(font);
  ReleaseDC(hwnd_, screen);
  const Rect placed = PlaceBox(caret_screen, window_client_screen, box_w, kBarHeight, 16);
  // Caller pixels already match this process. Do not scale them again.
  const Rect clamped = ClampRect(placed, WorkArea(placed));
  SetWindowPos(hwnd_, HWND_TOPMOST, clamped.x, clamped.y, clamped.w, clamped.h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(hwnd_, nullptr, TRUE);
}

void CandidateBar::Hide() {
  if (hwnd_ != nullptr) ShowWindow(hwnd_, SW_HIDE);
}

void CandidateBar::Paint() {
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(hwnd_, &paint);
  RECT client{};
  GetClientRect(hwnd_, &client);
  HBRUSH background = CreateSolidBrush(RGB(32, 32, 32));
  FillRect(dc, &client, background);
  DeleteObject(background);

  const bool settings = client.right >= kSettingsWidth;
  const int settings_left = settings ? client.right - kSettingsWidth : client.right;
  if (settings) {
    RECT strip{settings_left, client.top, client.right, client.bottom};
    HBRUSH button = CreateSolidBrush(RGB(52, 52, 52));
    FillRect(dc, &strip, button);
    DeleteObject(button);
  }

  SetBkMode(dc, TRANSPARENT);
  HFONT font = MakeFont();
  HGDIOBJ old = SelectObject(dc, font);
  const COLORREF text_color = mode_ == InputMode::English ? RGB(180, 220, 255) : RGB(245, 245, 245);
  int x = kPad;
  const int count = static_cast<int>(view_.page.size());
  for (int i = 0; i < count && x < settings_left; ++i) {
    const std::wstring text = CandidateLabel(i, view_.page[static_cast<std::size_t>(i)]);
    const int item_w = TextWidth(dc, font, text) + kItemPad;
    RECT item{x, client.top, x + item_w, client.bottom};
    if (item.right > settings_left) item.right = settings_left;
    if (item.right <= item.left) break;
    if (i == view_.highlight) {
      HBRUSH highlight = CreateSolidBrush(RGB(70, 110, 170));
      FillRect(dc, &item, highlight);
      DeleteObject(highlight);
    }
    SetTextColor(dc, text_color);
    RECT text_rect = item;
    text_rect.left += kItemPad / 2;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x += item_w;
  }
  if (settings) {
    SetTextColor(dc, RGB(220, 220, 220));
    RECT strip{settings_left, client.top, client.right, client.bottom};
    const std::wstring label = Utf8ToWide("设置");
    DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &strip,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  SelectObject(dc, old);
  DeleteObject(font);
  EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK CandidateBar::Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  CandidateBar* self = reinterpret_cast<CandidateBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<CandidateBar*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self != nullptr) self->hwnd_ = hwnd;
  }
  if (message == WM_NCDESTROY && self != nullptr) self->hwnd_ = nullptr;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_PAINT && self != nullptr) {
    self->Paint();
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
