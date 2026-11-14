#include "harness.hpp"
#include "candidate_bar.hpp"
#include "place.hpp"

void ClampPullsTheRectInsideTheWorkArea() {
  Rect work{0, 0, 100, 80};
  Rect got = ClampRect(Rect{90, 70, 30, 20}, work);
  CHECK(got.x + got.w <= work.w);
  CHECK(got.y + got.h <= work.h);
  CHECK(got.x >= 0);
  CHECK(got.y >= 0);
}

void SettingsHitIsTheRightStrip() {
  Rect bar{10, 10, 200, 28};
  CHECK(HitSettings(bar, 190, 20));
  CHECK(!HitSettings(bar, 100, 20));
  CHECK(!HitSettings(Rect{0, 0, 20, 28}, 10, 10));
}
