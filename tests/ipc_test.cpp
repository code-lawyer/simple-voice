#include "harness.hpp"
#include "hotkey.hpp"
#include "ipc.hpp"

#include <string_view>

void DecodeWaitsForAFullFrame() {
  Message in;
  in.type = MsgType::InsertText;
  in.request_id = 1;
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

void PhaseAndInsertResultRoundTrip() {
  Message phase;
  phase.type = MsgType::Phase;
  phase.text = "1";
  phase.state_id = 123;
  Message out;
  std::size_t used = 0;
  CHECK(Decode(Encode(phase), &out, &used));
  CHECK(out.type == MsgType::Phase);
  CHECK(out.text == "1");
  CHECK(out.state_id == 123);
  phase.text="5";
  auto stopped=Encode(phase);CHECK(Decode(stopped,&out,&used));CHECK(out.text=="5");
  phase.text="6";CHECK(Encode(phase).empty());
  stopped.back()='6';CHECK(DecodeFrame(stopped,&out,&used)==DecodeStatus::Invalid);

  Message ack;
  ack.type = MsgType::InsertResult;
  ack.request_id = 456;
  ack.text = "fail";
  CHECK(Decode(Encode(ack), &out, &used));
  CHECK(out.type == MsgType::InsertResult);
  CHECK(out.request_id == 456);
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
  enter.state_id = 88;
  Message out;
  std::size_t used = 0;
  CHECK(Decode(Encode(enter), &out, &used));
  CHECK(out.type == MsgType::Enter);
  CHECK(out.state_id == 88);
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

void RetiredAndUnauthenticatedFramesAreRejected() {
  CHECK(Encode(Message{}).empty());
  for(unsigned char type : {3,4,5,6,7,8,13}) {
    Message retired; retired.type=static_cast<MsgType>(type);
    CHECK(Encode(retired).empty());
    const std::string wire{1,0,0,0,static_cast<char>(type)};
    CHECK(DecodeFrame(wire,nullptr,nullptr)==DecodeStatus::Invalid);
  }
  for(auto type : {MsgType::StartListen,MsgType::Enter,MsgType::Cancel,MsgType::Phase,MsgType::ClientHello}) {
    Message message; message.type=type;
    if(type==MsgType::Phase)message.text="3";
    CHECK(Encode(message).empty());
    message.state_id=0x123456789ABCDEULL; message.client_window=0x123456ULL;
    const auto wire=Encode(message);
    Message decoded; std::size_t used=0;
    CHECK(Decode(wire,&decoded,&used)); CHECK(used==wire.size());
    CHECK(decoded.state_id== (type==MsgType::ClientHello ? 0 : message.state_id));
    CHECK(decoded.client_window== (type==MsgType::ClientHello ? message.client_window : 0));
    for(std::size_t i=0;i<wire.size();++i)
      CHECK(DecodeFrame(std::string_view(wire).substr(0,i),nullptr,nullptr)==DecodeStatus::Incomplete);
  }
}
