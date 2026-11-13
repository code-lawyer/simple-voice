#include "harness.hpp"
#include "hotkey.hpp"
#include "ipc.hpp"

#include <string_view>

void DecodeWaitsForAFullFrame() {
  Message in;
  in.type = MsgType::Draft;
  in.text = "明天下午";
  auto bytes = Encode(in);
  Message out;
  std::size_t used = 99;
  CHECK(!Decode(std::string_view(bytes).substr(0, 3), &out, &used));
  CHECK(used == 0);
  CHECK(Decode(bytes, &out, &used));
  CHECK(out.text == "明天下午");
  CHECK(DefaultHotkey().vk == 0x20);
}

void EnterAndCaretRoundTrip() {
  Message enter;
  enter.type = MsgType::Enter;
  Message out;
  std::size_t used = 0;
  CHECK(Decode(Encode(enter), &out, &used));
  CHECK(out.type == MsgType::Enter);
  CHECK(out.text.empty());

  Message caret;
  caret.type = MsgType::Caret;
  caret.caret_x = 10;
  caret.caret_y = 20;
  caret.caret_w = 3;
  caret.caret_h = 18;
  caret.window_x = 1;
  caret.window_y = 2;
  caret.window_w = 800;
  caret.window_h = 600;
  CHECK(Decode(Encode(caret), &out, &used));
  CHECK(out.caret_x == 10);
  CHECK(out.caret_h == 18);
  CHECK(out.window_w == 800);
  CHECK(out.window_h == 600);
}
