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
