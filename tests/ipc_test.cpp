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

void PhaseAndInsertAckRoundTrip() {
  Message phase;
  phase.type = MsgType::Phase;
  phase.text = "1";
  Message out;
  std::size_t used = 0;
  CHECK(Decode(Encode(phase), &out, &used));
  CHECK(out.type == MsgType::Phase);
  CHECK(out.text == "1");

  Message ack;
  ack.type = MsgType::InsertAck;
  ack.text = "fail";
  CHECK(Decode(Encode(ack), &out, &used));
  CHECK(out.type == MsgType::InsertAck);
  CHECK(out.text == "fail");
}

void ShowSettingsRoundTrip() {
  Message in;
  in.type = MsgType::ShowSettings;
  const std::string bytes = Encode(in);
  CHECK(bytes.size() == 5);
  CHECK(static_cast<unsigned char>(bytes[4]) == static_cast<unsigned char>(MsgType::ShowSettings));
  Message out;
  out.text = "kept";
  std::size_t used = 0;
  CHECK(Decode(bytes, &out, &used));
  CHECK(used == 5);
  CHECK(out.type == MsgType::ShowSettings);
  CHECK(out.text.empty());
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
