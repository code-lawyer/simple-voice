#pragma once

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

Rect PlaceBox(Rect caret_screen, Rect window_client_screen, int box_w, int box_h, int margin);
Rect ClampRect(Rect wanted, Rect work);
inline bool SameRect(Rect a, Rect b) { return a.x==b.x && a.y==b.y && a.w==b.w && a.h==b.h; }
Rect PlacePopup(Rect caret, Rect window, Rect work, int width, int height, int gap, int margin);
