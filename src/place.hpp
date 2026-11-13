#pragma once

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

Rect PlaceBox(Rect caret_screen, Rect window_client_screen, int box_w, int box_h, int margin);
