#include "place.hpp"

Rect PlaceBox(Rect caret_screen, Rect window_client_screen, int box_w, int box_h, int margin) {
  Rect box{0, 0, box_w, box_h};
  if (caret_screen.w > 0 && caret_screen.h > 0) {
    box.x = caret_screen.x;
    box.y = caret_screen.y + caret_screen.h;
    return box;
  }
  box.x = window_client_screen.x + window_client_screen.w - box_w - margin;
  box.y = window_client_screen.y + window_client_screen.h - box_h - margin;
  return box;
}

Rect ClampRect(Rect wanted, Rect work) {
  if (wanted.w > work.w) {
    wanted.x = work.x;
    wanted.w = work.w;
  } else if (wanted.x < work.x) {
    wanted.x = work.x;
  } else if (wanted.x + wanted.w > work.x + work.w) {
    wanted.x = work.x + work.w - wanted.w;
  }
  if (wanted.h > work.h) {
    wanted.y = work.y;
    wanted.h = work.h;
  } else if (wanted.y < work.y) {
    wanted.y = work.y;
  } else if (wanted.y + wanted.h > work.y + work.h) {
    wanted.y = work.y + work.h - wanted.h;
  }
  return wanted;
}
