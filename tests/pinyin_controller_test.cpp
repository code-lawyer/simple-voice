#include "harness.hpp"
#include "composer.hpp"

#include <string>
#include <vector>

class FakeComposer : public Composer {
 public:
  int clears = 0;
  int processes = 0;
  int last_vk = 0;

  void SetComposing(std::string preedit, std::vector<std::string> page) {
    view_.composing = true;
    view_.preedit = std::move(preedit);
    view_.page = std::move(page);
    view_.commit.clear();
  }

  RimeView ProcessKey(int vk, bool) override {
    ++processes;
    last_vk = vk;
    return view_;
  }

  void Clear() override {
    ++clears;
    view_ = {};
  }

  const RimeView& view() const override { return view_; }

 private:
  RimeView view_{};
};

void VoiceHotkeyClearsWithoutCommitting() {
  FakeComposer fake;
  PinyinController box(&fake);
  fake.SetComposing("nihao", {"你好", "拟好"});
  KeyState s{};
  s.accepts = true;
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(box.Apply(s, DefaultHotkey()).action == KeyAction::OpenVoice);
  CHECK(fake.clears == 1);
  CHECK(fake.processes == 0);
  CHECK(!box.view().composing);
  CHECK(box.view().commit.empty());
}
void VoiceSessionLetsLettersThrough() {
  FakeComposer fake;
  PinyinController box(&fake);
  KeyState s{};
  s.accepts = true;
  s.voice_active = true;
  s.vk = 0x41;
  CHECK(box.Apply(s, DefaultHotkey()).action == KeyAction::Pass);
  CHECK(fake.processes == 0);
  s.vk = 0x0D;
  CHECK(box.Apply(s, DefaultHotkey()).action == KeyAction::VoiceEnter);
  CHECK(fake.processes == 0);
}
void ShiftTogglesAndClears() {
  FakeComposer fake;
  PinyinController box(&fake);
  fake.SetComposing("ni", {"你"});
  KeyState s{};
  s.accepts = true;
  s.vk = 0x10;
  CHECK(box.Apply(s, DefaultHotkey()).action == KeyAction::ToggleMode);
  CHECK(box.mode() == InputMode::English);
  CHECK(fake.clears == 1);
  s.vk = 0x41;
  CHECK(box.Apply(s, DefaultHotkey()).action == KeyAction::Pass);
  CHECK(fake.processes == 0);
}
void LetterIsSentToTheComposer() {
  FakeComposer fake;
  PinyinController box(&fake);
  KeyState s{};
  s.accepts = true;
  s.vk = 0x41;
  CHECK(box.Apply(s, DefaultHotkey()).action == KeyAction::ToRime);
  CHECK(fake.processes == 1);
  CHECK(fake.last_vk == 0x41);
}
