#include "confirm_box.hpp"

#include <algorithm>
#include <string>

namespace {

std::wstring Utf8ToWide(const std::string& utf8) {
  if (utf8.empty()) return L"";
  const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(count > 0 ? count : 0, L'\0');
  if (count > 0) {
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
  }
  return wide;
}

}  // namespace

bool ConfirmBox::Create(HINSTANCE instance, Hook hook, void* self) {
  hook_ = hook;
  hook_self_ = self;
  WNDCLASSW window_class{};
  window_class.lpfnWndProc = Proc;
  window_class.hInstance = instance;
  window_class.lpszClassName = L"VoiceImeConfirm";
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hbrBackground = CreateSolidBrush(RGB(32, 32, 32));
  RegisterClassW(&window_class);
  hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"VoiceImeConfirm",
                           L"", WS_POPUP, 0, 0, 200, 40, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) return false;
  SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
  return true;
}

void ConfirmBox::ShowText(const std::string& utf8, Rect caret, Rect window_client) {
  if (hwnd_ == nullptr) return;
  const std::wstring wide = Utf8ToWide(utf8.empty() ? "…" : utf8);
  SetWindowTextW(hwnd_, wide.c_str());
  HDC screen = GetDC(hwnd_);
  HFONT font = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  HGDIOBJ old = SelectObject(screen, font);
  RECT measure{0, 0, 640, 0};
  DrawTextW(screen, wide.c_str(), static_cast<int>(wide.size()), &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(screen, old);
  ReleaseDC(hwnd_, screen);
  DeleteObject(font);
  const int box_w = std::max(200, static_cast<int>(measure.right) + 24);
  const int box_h = std::max(40, static_cast<int>(measure.bottom) + 16);
  const Rect placed = PlaceBox(caret, window_client, box_w, box_h, 16);
  SetWindowPos(hwnd_, HWND_TOPMOST, placed.x, placed.y, placed.w, placed.h,
               SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(hwnd_, nullptr, TRUE);
}

void ConfirmBox::Hide() {
  if (hwnd_ != nullptr) ShowWindow(hwnd_, SW_HIDE);
}

void ConfirmBox::Paint() {
  PAINTSTRUCT paint;
  HDC dc = BeginPaint(hwnd_, &paint);
  RECT client;
  GetClientRect(hwnd_, &client);
  HBRUSH brush = CreateSolidBrush(RGB(32, 32, 32));
  FillRect(dc, &client, brush);
  DeleteObject(brush);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(245, 245, 245));
  HFONT font = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
  HGDIOBJ old = SelectObject(dc, font);
  wchar_t title[1024];
  const int count = GetWindowTextW(hwnd_, title, 1024);
  RECT text = client;
  InflateRect(&text, -12, -8);
  DrawTextW(dc, title, count, &text, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  DeleteObject(font);
  EndPaint(hwnd_, &paint);
}

LRESULT CALLBACK ConfirmBox::Proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  ConfirmBox* self = reinterpret_cast<ConfirmBox*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<ConfirmBox*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self != nullptr) self->hwnd_ = hwnd;
  }
  if (message == WM_PAINT && self != nullptr) {
    self->Paint();
    return 0;
  }
  if (self != nullptr && self->hook_ != nullptr && message != WM_NCCREATE) {
    return self->hook_(hwnd, message, wparam, lparam, self->hook_self_);
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
