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
