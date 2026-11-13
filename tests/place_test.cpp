#include "harness.hpp"
#include "place.hpp"

void MissingCaretGoesToBottomRight() {
  Rect box = PlaceBox(Rect{0, 0, 0, 0}, Rect{10, 20, 800, 600}, 200, 40, 16);
  CHECK(box.x == 10 + 800 - 200 - 16);
  CHECK(box.y == 20 + 600 - 40 - 16);
  Rect under = PlaceBox(Rect{100, 200, 2, 18}, Rect{0, 0, 800, 600}, 200, 40, 16);
  CHECK(under.x == 100);
  CHECK(under.y == 218);
}
