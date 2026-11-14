# 拼音加语音 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让已经注册的 Windows 输入法在普通文本框里打全拼整句，并保留本地语音：确认框、回车写入、Esc 作废。

**Architecture:** 按键先经过纯函数 `RouteKey`，再由 `PinyinController` 调用组字器。真正的组字是 TSF 进程里的 librime，不进语音引擎。语音模型、确认框、语音词库留在 `voiceime_engine.exe`。两套词库互不写入。

**Tech Stack:** C++17、现有 WinLibs g++ 与 Ninja、CMake、librime 1.17.0（静态链进 `voiceime_tsf.dll`）、袖珍简化字拼音词库（Apache-2.0）、Win32、TSF。不用 MSVC。不用小狼毫，不用雾凇，不用微信的文件。

**Spec:** `docs/superpowers/specs/2026-11-13-pinyin-voice-ime-design.md`

## Global Constraints

- 全拼整句。不做简拼、双拼、模糊音、五笔、笔画、手写。
- 候选 5 个。空格或回车上高亮项。数字 1 到 5 选当前页对应项。Page Up、Page Down、减号、等号翻页。Esc 取消组字。
- 单独按 Shift 切换中文/英文并保持。按住 Shift 再按字母，或 CapsLock 亮着时按字母：该字母直接进文档，模式不变，未选定拼音不上屏。
- 中文模式用中文标点，字母和数字半角。没有全角开关。
- 没有汉字候选时，空格把当前拉丁字母原文上屏。
- 拼音用行内组字。语音不用组字串，只在确认框里，回车才 `ITfInsertAtSelection`。
- 语音会话在听、定稿中或待确认时，字母直接进文档；回车、Esc、语音快捷键仍归语音。
- 有未选定拼音时按语音快捷键：拼音不上屏，然后开麦。
- 默认语音快捷键 Ctrl+Alt+Space。没有 `config.json` 时用这个。
- 拼音个人词在 `%LOCALAPPDATA%\VoiceIme\rime\`。语音词库仍是 `%LOCALAPPDATA%\VoiceIme\lexicon.sqlite`，规则不变。两边互不写入。
- 代码 Apache-2.0。librime 保持 BSD-3-Clause。不链接 GPL。不把 GPL 或 LGPL 的方案文件拷进仓库。`BUILD_DATA` 必须关掉，因为 librime 自带的 preset 摘自 LGPL 数据。
- 音频不落盘、不上传。模型目录不重新下载。
- 每次提交用 `git -c user.name=lanzh -c user.email=lanzh@localhost commit`。不改 git config。不提交 `RESEARCH.md`。不 push。
- 验收用新开的记事本。命令行和 Windows Terminal 不在范围内。

## 文件

- 新建 `src/key_route.hpp`、`src/key_route.cpp`：按键决定，不调用 librime。
- 新建 `src/composer.hpp`：`Composer`、`RimeView`、`PinyinController`。
- 新建 `src/rime_session.hpp`、`src/rime_session.cpp`：librime 的 `Composer`。
- 新建 `src/candidate_bar.hpp`、`src/candidate_bar.cpp`：候选条窗口。命中测试放在头文件里，测试不必链窗口。
- 修改 `src/place.hpp`、`src/place.cpp`：加 `ClampRect`。
- 新建 `rime-data/`：自写方案、标点、`SOURCE.txt`，以及下载的 `pinyin_simp.dict.yaml` 和它的 `LICENSE`。
- 新建 `cmake/librime.cmake`：静态编译并链上 librime 1.17.0。
- 修改 `src/ipc.hpp`、`src/ipc.cpp`、`src/engine_main.cpp`、`src/tsf_service.cpp`、`CMakeLists.txt`、`README.md`。
- 测试：`tests/key_route_test.cpp`、`tests/pinyin_controller_test.cpp`、`tests/rime_session_test.cpp`、`tests/candidate_test.cpp`，并在 `tests/main.cpp` 登记。

## Review Focus

这五类输入最容易在真实打字里出错，单测必须钉住：

1. 组字过程中 Ctrl 或 Alt 组合（复制、粘贴）必须交给文档。钉在 Task 1 `keyroute.ChordPassesToTheDocument`。
2. 只读或密码框里，拼音和语音快捷键都不吃键。钉在 Task 1 `keyroute.DisabledDocumentPassesEveryKey`。
3. 语音框还在时，字母进文档，回车不交给拼音。钉在 Task 2 `pinyin.VoiceSessionLetsLettersThrough`。
4. 未选定拼音时按语音快捷键，组字器的 `ProcessKey` 不能被调用，`Clear` 必须被调用。钉在 Task 2 `pinyin.VoiceHotkeyClearsWithoutCommitting`。
5. 主键盘和数字小键盘的 6，以及候选只有 2 个时按 3，都必须交给文档。钉在 Task 1 `keyroute.DigitPastThePagePasses`。

---

### Task 1: 按键路由

**Files:**
- Create: `src/key_route.hpp`
- Create: `src/key_route.cpp`
- Test: `tests/key_route_test.cpp`
- Modify: `tests/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Hotkey` 与 `DefaultHotkey()`（`src/hotkey.hpp`）。修饰键位：ctrl=1，alt=2，shift=4。默认是 `{1|2, 0x20}`。
- Produces:
  - `enum class InputMode { Chinese, English };`
  - `enum class KeyAction { Pass, ToRime, ToggleMode, OpenVoice, VoiceEnter, VoiceEscape, Consume, PassAfterClear };`
  - `struct KeyState { InputMode mode; bool accepts; bool voice_active; bool composing; int candidate_count; bool shift; bool ctrl; bool alt; bool caps; bool repeat; int vk; };`
  - `struct KeyDecision { KeyAction action; };`
  - `KeyDecision RouteKey(const KeyState& state, Hotkey hotkey);`

判断顺序固定为：

1. `accepts` 为假：`Pass`。
2. ctrl、alt 与 vk 同时等于 `hotkey`：`repeat` 为真则 `Consume`，否则 `OpenVoice`。比较前把左右 Ctrl 都当成 ctrl，左右 Alt 都当成 alt。`state.ctrl` / `state.alt` 已经是合并后的值，函数不再读左右键。
3. `voice_active`：Enter（`0x0D`）为 `VoiceEnter`，Esc（`0x1B`）为 `VoiceEscape`，其余 `Pass`。
4. vk 是 `0x10`、`0xA0` 或 `0xA1`，且 ctrl 与 alt 都为假：`ToggleMode`。
5. 中文模式，vk 在 `0x41` 到 `0x5A`，ctrl 与 alt 都为假，并且 `shift` 或 `caps` 为真：`PassAfterClear`。
6. 英文模式：`Pass`。
7. 中文模式，且 ctrl 与 alt 都为假：
   - 字母 `0x41`–`0x5A`：`ToRime`。
   - 正在组字时，退格 `0x08`、空格 `0x20`、回车 `0x0D`、Esc `0x1B`：`ToRime`。
   - 正在组字时，Page Up `0x21`、Page Down `0x22`、减号 `0xBD`、等号 `0xBB`：`ToRime`。
   - 正在组字时，主键盘 `0x31`–`0x35` 或小键盘 `0x61`–`0x65`：主键盘序号是 `vk - 0x30`，小键盘序号是 `vk - 0x60`。序号从 1 到 5，且小于等于 `candidate_count`，才 `ToRime`，否则 `Pass`。
   - 标点 `0xBA`–`0xC0` 和 `0xDB`–`0xDF`：无论是否在组字，都 `ToRime`。
8. 其余：`Pass`。

- [ ] **Step 1: Write the failing test**

在 `tests/key_route_test.cpp` 写这些函数，并在 `tests/main.cpp` 用前缀 `keyroute.` 登记。把 `tests/key_route_test.cpp` 加进 `voiceime_tests`。

```cpp
void ChineseLetterGoesToRime() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
}
void ChordPassesToTheDocument() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.ctrl = true;
  s.vk = 0x43;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.ctrl = false;
  s.alt = true;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void DisabledDocumentPassesEveryKey() {
  KeyState s{};
  s.mode = InputMode::Chinese;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void DigitPastThePagePasses() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.candidate_count = 2;
  s.vk = 0x33;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.candidate_count = 5;
  s.vk = 0x36;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.vk = 0x65;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
}
void ShiftLetterDoesNotToggle() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.shift = true;
  s.vk = 0x42;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::PassAfterClear);
  s.shift = false;
  s.caps = true;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::PassAfterClear);
}
void BareShiftToggles() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::English;
  s.vk = 0xA0;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToggleMode);
}
void VoiceHotkeyAndItsRepeat() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::OpenVoice);
  s.repeat = true;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Consume);
}
void VoiceSessionStealsEnterOnly() {
  KeyState s{};
  s.accepts = true;
  s.voice_active = true;
  s.mode = InputMode::Chinese;
  s.composing = true;
  s.vk = 0x41;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.vk = 0x0D;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::VoiceEnter);
  s.vk = 0x1B;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::VoiceEscape);
}
void IdleEnterPasses() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0x0D;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void ChinesePeriodGoesToRime() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0xBE;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
  s.mode = InputMode::English;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
}
void PageKeyOnlyWhileComposing() {
  KeyState s{};
  s.accepts = true;
  s.mode = InputMode::Chinese;
  s.vk = 0xBD;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::Pass);
  s.composing = true;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::ToRime);
}
void VoiceHotkeyStillOpensWhileVoiceIsActive() {
  KeyState s{};
  s.accepts = true;
  s.voice_active = true;
  s.ctrl = true;
  s.alt = true;
  s.vk = 0x20;
  CHECK(RouteKey(s, DefaultHotkey()).action == KeyAction::OpenVoice);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target voiceime_tests`

Expected: 编译失败，`RouteKey` 未声明。

- [ ] **Step 3: Implement `RouteKey`**

按上面的顺序写。不要在这个文件里包含 `rime_api.h`。

- [ ] **Step 4: Run test to verify it passes**

Run: `build\voiceime_tests.exe keyroute`

Expected: 输出以 `passed` 结尾，退出码 0。已有测试仍全部通过。

- [ ] **Step 5: Commit**

```bash
git add src/key_route.hpp src/key_route.cpp tests/key_route_test.cpp tests/main.cpp CMakeLists.txt
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: route pinyin and voice keys"
```

### Task 2: 组字控制器

**Files:**
- Create: `src/composer.hpp`
- Test: `tests/pinyin_controller_test.cpp`
- Modify: `tests/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 的 `RouteKey`、`KeyState`、`KeyAction`、`Hotkey`。
- Produces:
  - `struct RimeView { bool composing; std::string preedit; std::vector<std::string> page; int highlight; std::string commit; };`
  - `class Composer { public: virtual ~Composer(); virtual RimeView ProcessKey(int vk, bool shift) = 0; virtual void Clear() = 0; virtual const RimeView& view() const = 0; };`
  - `class PinyinController { public: explicit PinyinController(Composer* composer); InputMode mode() const; const RimeView& view() const; KeyDecision Apply(KeyState state, Hotkey hotkey); };`

`Apply` 忽略调用者传入的 `state.mode`、`state.composing`、`state.candidate_count`，改用自己的 `mode()` 和 `view()`。`page` 的长度就是候选个数。然后：

- `ToggleMode`：`Clear()`，中文变英文，英文变中文。
- `OpenVoice`：`Clear()`。不要调用 `ProcessKey`。
- `PassAfterClear`：`Clear()`。
- `ToRime`：`ProcessKey(state.vk, state.shift)`。
- `Pass`、`Consume`、`VoiceEnter`、`VoiceEscape`：不调用 `Clear`，也不调用 `ProcessKey`。

初始模式是中文。`Clear()` 之后视图的 `composing` 为假，`commit` 为空。

- [ ] **Step 1: Write the failing test**

用一个记录调用的假 `Composer`。登记前缀 `pinyin.`。

```cpp
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
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target voiceime_tests`

Expected: 编译失败，`PinyinController` 未声明。

- [ ] **Step 3: Implement `PinyinController` 在 `src/composer.hpp`**

头文件即可，逻辑不长。假组字器只放在测试里。

- [ ] **Step 4: Run test to verify it passes**

Run: `build\voiceime_tests.exe pinyin`

Expected: 以 `passed` 结尾，退出码 0。

- [ ] **Step 5: Commit**

```bash
git add src/composer.hpp tests/pinyin_controller_test.cpp tests/main.cpp CMakeLists.txt
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: coordinate pinyin composition and voice keys"
```

### Task 3: 方案和词库

**Files:**
- Create: `rime-data/voiceime.schema.yaml`
- Create: `rime-data/voiceime.dict.yaml`
- Create: `rime-data/default.yaml`
- Create: `rime-data/SOURCE.txt`
- Create: `rime-data/pinyin_simp.dict.yaml`
- Create: `rime-data/pinyin_simp.LICENSE`
- Create: `rime-data/pinyin_simp.AUTHORS`

**Interfaces:**
- Consumes: 无。
- Produces: 共享数据目录。方案 id 是 `voiceime`。词典名是 `voiceime`，它 `import_tables` 引入 `pinyin_simp`。用户词库名是 `voiceime`。

- [ ] **Step 1: Write the failing test**

本任务没有 C++ 测试。检查就是下面的文件内容。先不要编译 librime。

- [ ] **Step 2: 记录上游并核对许可证**

`git ls-remote https://github.com/rime/rime-pinyin-simp HEAD`，把打印的哈希写入 `rime-data/SOURCE.txt`，并写明仓库 `rime/rime-pinyin-simp`、许可证 Apache-2.0。用这个哈希下载 `pinyin_simp.dict.yaml`、`LICENSE`、`AUTHORS`。打开 `LICENSE`，确认文本是 Apache-2.0。不是就停，不要换一份 GPL 词库。

不要下载 `pinyin_simp.schema.yaml`。那份方案带简拼、纠错派生和笔画反查。

- [ ] **Step 3: 写入自有方案**

`voiceime.schema.yaml` 的引擎只用 `recognizer`、`key_binder`、`speller`、`punctuator`、`selector`、`navigator`、`express_editor`，分段器用 `ascii_segmentor`、`matcher`、`abc_segmentor`、`punct_segmentor`、`fallback_segmentor`，翻译器只用 `punct_translator` 和 `script_translator`。不要 `simplifier`，不要 `reverse_lookup`，不要 `dependencies`。

`speller/alphabet` 是 `zyxwvutsrqponmlkjihgfedcba`，`delimiter` 是空格和撇号。`algebra` 只有这两条：

```yaml
- derive/^([nl])ue$/$1ve/
- derive/^([jqxy])u/$1v/
```

`translator/dictionary` 是 `voiceime`，`enable_user_dict: true`。`preedit_format` 把 `v` 显示成 `ü`（`nl` 的 `v`/`ue`，以及 `jqxy` 的 `v`）。

`punctuator` 与 `recognizer` 使用 `import_preset: default`。`key_binder/bindings` 只写四条：有菜单时减号和 Page Up 发送 `Page_Up`，等号和 Page Down 发送 `Page_Down`。

`voiceime.dict.yaml` 的表头如下。`...` 后面的唯一词条用制表符分成三列：`你好`、`ni hao`、`999999`。

```yaml
---
name: voiceime
version: "1"
sort: by_weight
import_tables:
  - pinyin_simp
...
你好	ni hao	999999
```

`default.yaml` 含 `menu/page_size: 5`。`punctuator/half_shape` 定为：逗号 `，`，句号 `。`，问号 `？`，叹号 `！`，冒号 `：`，分号 `；`，反斜杠 `、`，单引号成对 `‘’`，双引号成对 `“”`，圆括号 `（）`，方括号 `【】`，尖括号 `《》`，花括号 `「」`。`` ` `` `~` `@` `#` `$` `%` `^` `&` `*` `-` `_` `+` `=` `|` `/` 保持原字符。

`recognizer/patterns/punct` 为 `^/([0-9]+[a-z]*|[a-z]+)$`。不要写反查图案。

- [ ] **Step 4: 核对方案里没有简拼和笔画**

Run: `rg -n "abbrev:|reverse_lookup|stroke|simplifier" rime-data`

Expected: 无匹配。

- [ ] **Step 5: Commit**

```bash
git add rime-data
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: add the local full-pinyin schema"
```

### Task 4: 静态 librime

**Files:**
- Create: `cmake/librime.cmake`
- Create: `src/rime_session.hpp`
- Create: `src/rime_session.cpp`
- Test: `tests/rime_session_test.cpp`
- Modify: `CMakeLists.txt`
- Modify: `tests/main.cpp`

**Interfaces:**
- Consumes: `Composer`、`RimeView`。共享数据在 `rime-data/`。
- Produces: `class RimeSession : public Composer`。`bool Start(const std::wstring& shared_dir, const std::wstring& user_dir)`。失败返回 false。`Shutdown()`。`ProcessKey` / `Clear` / `view` 实现 `Composer`。用户目录由调用者传入，生产环境用 `%LOCALAPPDATA%\VoiceIme\rime`。

- [ ] **Step 1: Write the failing test**

登记前缀 `rime.`。每个测试用自己的临时用户目录。

```cpp
void NihaoCommitsTheWeightedWord() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  for (int vk : {0x4E, 0x49, 0x48, 0x41, 0x4F}) session.ProcessKey(vk, false);
  CHECK(session.view().composing);
  CHECK(!session.view().page.empty());
  CHECK(session.view().page[0] == "你好");
  RimeView done = session.ProcessKey(0x20, false);
  CHECK(done.commit == "你好");
  CHECK(!done.composing);
}
void LongPinyinProducesHanzi() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  const int keys[] = {0x4A,0x49,0x4E,0x54,0x49,0x41,0x4E,0x54,0x49,0x41,0x4E,0x51,0x49,0x42,0x55,0x43,0x55,0x4F};
  for (int vk : keys) session.ProcessKey(vk, false);
  CHECK(!session.view().page.empty());
  CHECK(ContainsCjk(session.view().page[0]));
}
void PeriodCommitsChinesePunct() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  RimeView done = session.ProcessKey(0xBE, false);
  CHECK(done.commit == "。");
}
void UnknownLettersCommitRawOnSpace() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  for (int i = 0; i < 4; ++i) session.ProcessKey(0x58, false);
  RimeView done = session.ProcessKey(0x20, false);
  CHECK(done.commit == "xxxx");
  CHECK(!ContainsCjk(done.commit));
}
void EnterCommitsTheHighlightAndEscapeDropsIt() {
  RimeSession session;
  CHECK(session.Start(SharedDataDir(), TempUserDir()));
  for (int vk : {0x4E, 0x49, 0x48, 0x41, 0x4F}) session.ProcessKey(vk, false);
  CHECK(session.view().page.size() >= 2);
  const std::string second = session.view().page[1];
  RimeView picked = session.ProcessKey(0x32, false);
  CHECK(picked.commit == second);
  for (int vk : {0x4E, 0x49, 0x48, 0x41, 0x4F}) session.ProcessKey(vk, false);
  RimeView dropped = session.ProcessKey(0x1B, false);
  CHECK(dropped.commit.empty());
  CHECK(!dropped.composing);
  for (int vk : {0x4E, 0x49, 0x48, 0x41, 0x4F}) session.ProcessKey(vk, false);
  RimeView entered = session.ProcessKey(0x0D, false);
  CHECK(entered.commit == "你好");
}
void SelectingACandidateCreatesAUserDb() {
  RimeSession session;
  const std::wstring user = TempUserDir();
  CHECK(session.Start(SharedDataDir(), user));
  for (int vk : {0x4E, 0x49, 0x48, 0x41, 0x4F}) session.ProcessKey(vk, false);
  session.ProcessKey(0x20, false);
  session.Shutdown();
  CHECK(DirectoryContainsUserDb(user));
}
```

`ContainsCjk` 为真当 UTF-8 里有一个码点在 `0x4E00` 到 `0x9FFF`。`SharedDataDir()` 指向源码树的 `rime-data`。`DirectoryContainsUserDb` 为真当目录里存在名字含 `voiceime` 和 `userdb` 的条目。

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target voiceime_tests`

Expected: 编译失败，`RimeSession` 未声明。

- [ ] **Step 3: 编译并链接 librime 1.17.0**

`cmake/librime.cmake` 把依赖和 librime 装到 `build/rime-prefix`。标签固定：

- librime `1.17.0`，提交 `33e78140250125871856cdc5b42ddc6a5fcd3cd4`
- yaml-cpp `0.8.0`
- leveldb `1.23`
- marisa-trie `0.2.6`
- OpenCC `1.1.9`

librime 的 CMake 参数：`-DBUILD_SHARED_LIBS=OFF`、`-DBUILD_TEST=OFF`、`-DENABLE_LOGGING=OFF`、`-DBUILD_DATA=OFF`、`-DENABLE_EXTERNAL_PLUGINS=OFF`、`-DBUILD_MERGED_PLUGINS=OFF`。`CMAKE_PREFIX_PATH` 指向上面的安装前缀。静态库链进 `voiceime_tests` 和 `voiceime_tsf`。不要产出需要另带的 `rime.dll`。

`RimeSession::Start` 调用 `RimeInitialize`、`RimeDeployerInitialize` 和 `RimeStartMaintenance`，等到部署结束再 `RimeCreateSession`。共享目录是 `rime-data`，用户目录是参数。`ProcessKey` 用 `RimeProcessKey` 和 `RimeGetContext` / `RimeGetCommit`。`page` 最多 5 条，`highlight` 是页内下标。`Clear` 调用 `RimeClearComposition`，不产生 commit。

不要从 `lexicon.cpp` 调用 librime，也不要在 `rime_session.cpp` 里包含 `lexicon.hpp`。

- [ ] **Step 4: Run test to verify it passes**

Run: `build\voiceime_tests.exe rime`

Expected: 以 `passed` 结尾，退出码 0。第一次部署可以花较长时间，这是部署，不是失败。

- [ ] **Step 5: Commit**

提交 `cmake/librime.cmake`、`src/rime_session.*`、测试和 `CMakeLists.txt`。不要把 `build/rime-prefix` 交进去。

```bash
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: compose full pinyin with librime"
```

### Task 5: 候选条的位置和设置按钮

**Files:**
- Modify: `src/place.hpp`
- Modify: `src/place.cpp`
- Create: `src/candidate_bar.hpp`
- Create: `src/candidate_bar.cpp`
- Test: `tests/candidate_test.cpp`
- Modify: `tests/main.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Rect`、`PlaceBox`、`RimeView`。
- Produces:
  - `Rect ClampRect(Rect wanted, Rect work);`
  - `bool HitSettings(Rect bar, int x, int y);` 按钮是条的右侧 36 像素，通高。条宽小于 36 时永远为假。
  - `class CandidateBar`，方法 `void Show(Rect caret_screen, Rect window_client_screen, const RimeView& view, InputMode mode)`、`void Hide()`、`HWND hwnd() const`。

`ClampRect` 先把矩形移进 `work`，宽或高超过工作区时贴在左上角并缩到工作区之内。`Show` 用 `PlaceBox` 得到位置，再用当前显示器的 `rcWork` 做 `ClampRect`，然后 `SetWindowPos`。窗口类名 `VoiceImeCandidates`，样式与确认框相同：`WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`，`WS_POPUP`。进程 DPI 不在这里改；坐标用调用者给的像素，不再乘一次缩放。

条上从左到右画页内候选，高亮项加亮，最右画「设置」。没有组字时 `Hide`。

- [ ] **Step 1: Write the failing test**

```cpp
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
```

登记前缀 `candidate.`。

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target voiceime_tests`

Expected: 编译失败，`ClampRect` 或 `HitSettings` 未声明。

- [ ] **Step 3: Implement `ClampRect`、`HitSettings` 和 `CandidateBar`**

`candidate_bar.cpp` 只编进 `voiceime_tsf`。`ClampRect` 和 `HitSettings` 放在测试能链到的静态库 `voiceime_core` 一侧：`ClampRect` 在 `place.cpp`，`HitSettings` 做成 `candidate_bar.hpp` 里的 inline 函数。

- [ ] **Step 4: Run test to verify it passes**

Run: `build\voiceime_tests.exe candidate`

Expected: 以 `passed` 结尾，退出码 0。

- [ ] **Step 5: Commit**

```bash
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: place the pinyin candidate bar on screen"
```

### Task 6: 接到文本框

**Files:**
- Modify: `src/tsf_service.cpp`
- Modify: `src/tsf_service.hpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `PinyinController`、`RimeSession`、`CandidateBar`、`RouteKey` 的动作、现有 `WriteClient`、`DocumentAcceptsText`、`HotkeyDown`。
- Produces: 中文模式下的行内组字和候选条。语音消息仍是 `StartListen`、`Enter`、`Cancel`。

`VoiceService` 持有一个 `RimeSession` 和一个 `PinyinController`。`Activate` 时 `Start`，共享目录是 DLL 旁边的 `rime-data`，用户目录是 `%LOCALAPPDATA%\VoiceIme\rime`。`Start` 失败时拼音按键改为 `Pass`，语音快捷键保持原样。

`OnKey` 在 keydown 组装 `KeyState`：`accepts` 来自 `DocumentAcceptsText`，`voice_active` 为现有语音阶段不是空闲，`shift`/`ctrl`/`alt`/`caps` 用现在的 `GetAsyncKeyState` 或 `GetKeyState` 读法，`repeat` 用 `IsKeyRepeat`。然后 `Apply`。

- `Pass`：`eaten` 为假。
- `PassAfterClear`：控制器已经清掉组字；结束 TSF 组字且不插入；`eaten` 为假。
- `ToRime`：`eaten` 为真。`commit` 非空时结束组字并插入 `commit`。否则把 `preedit` 设成组字串，并用光标矩形 `Show` 候选条。
- `ToggleMode`：结束组字且不插入，`Hide` 候选条，`eaten` 为真。
- `OpenVoice`：结束组字且不插入，`Hide`，再走现有的 `StartListen`。
- `VoiceEnter` / `VoiceEscape`：不改拼音组字，走现有的回车和 Esc。
- `Consume`：`eaten` 为真，不发消息。

插入拼音用组字结束，不用剪贴板，不用 `SendInput`。语音插入函数保持原样。

候选条的像素来自现有光标矩形，不要再乘 DPI。

- [ ] **Step 1: 先保持现有 `OnKey`，确认单测仍通过**

Run: `build\voiceime_tests.exe`

Expected: 以 `passed` 结尾。这一任务没有能伪造 TSF 宿主的红灯测试。吃键与否以 `KeyAction` 为准：`Pass` 和 `PassAfterClear` 不吃，其余都吃。

- [ ] **Step 2: 编译当前的文本服务**

Run: `cmake --build build --target voiceime_tsf`

Expected: 退出码 0。这只证明旧 DLL 还能生成。

- [ ] **Step 3: 按接口把 `OnKey` 接上**

拼音部署未完成时不阻塞 `Activate` 的返回值：部署放在第一次 `ToRime` 之前，失败则这一次 `Pass`。不要在字母键上等待语音引擎。

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target voiceime_tsf` 然后 `build\voiceime_tests.exe`

Expected: TSF 目标退出码 0，测试输出以 `passed` 结尾。不要在这一步声称记事本已经通过。

- [ ] **Step 5: Commit**

```bash
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: show pinyin composition in the text service"
```

### Task 7: 候选条上的设置

**Files:**
- Modify: `src/ipc.hpp`
- Modify: `src/ipc.cpp`
- Modify: `tests/ipc_test.cpp`
- Modify: `src/engine_main.cpp`
- Modify: `src/candidate_bar.cpp`
- Modify: `src/tsf_service.cpp`

**Interfaces:**
- Consumes: `Message`、`Encode`、`Decode`、引擎里现有的托盘菜单。
- Produces: `MsgType::ShowSettings = 14`。正文为空。引擎收到后弹出和托盘相同的菜单：设置快捷键、最近记住的词、退出。

候选条左键落在 `HitSettings` 内时，TSF 写一条 `ShowSettings`。管道没连上就什么都不做。点候选词本身不发这条消息，选词仍由数字键和空格完成。

- [ ] **Step 1: Write the failing test**

在 `tests/ipc_test.cpp` 增加 `ShowSettingsRoundTrip`：`Encode` 一个 `ShowSettings`，`Decode` 回来类型是 `ShowSettings`，正文为空。在 `tests/main.cpp` 登记 `ipc.ShowSettingsRoundTrip`。

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target voiceime_tests`

Expected: 编译失败，`ShowSettings` 不是 `MsgType` 的枚举值。

- [ ] **Step 3: 实现消息和按钮**

`Decode` 不认识的类型仍按现在的方式失败。`14` 必须认识。引擎的菜单函数保持原有三项，托盘和这条消息都调用它。

- [ ] **Step 4: Run test to verify it passes**

Run: `build\voiceime_tests.exe ShowSettings`

Expected: 以 `passed` 结尾，退出码 0。

- [ ] **Step 5: Commit**

```bash
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "feat: open the voice settings menu from the candidate bar"
```

### Task 8: 说明和验收清单

**Files:**
- Modify: `README.md`

**Interfaces:**
- Consumes: 前面各任务的路径和快捷键。
- Produces: 一份与规格一致的使用说明。不勾选手工验收。

- [ ] **Step 1: 改 README**

写明：这是本地全拼加语音，无账号。全拼用空格、数字和 Shift。语音仍是 Ctrl+Alt+Space，回车写入，Esc 作废。设置在托盘和候选条右侧。词库和方案在 `rime-data`，许可证见 `SOURCE.txt`。构建依赖 librime 静态库。删掉「只有语音、字母直接进文档」的旧说明。

把规格里的 8 条记事本验收原样抄进 README，复选框全部保持 `- [ ]`。

- [ ] **Step 2: 跑完全部单测**

Run: `build\voiceime_tests.exe`

Expected: 一行 `N passed`，退出码 0，`N` 大于原来的 26。

- [ ] **Step 3: 确认没有禁掉的功能被加进来**

Run: `rg -n "wubi|五笔|emoji|雾凇|weasel|小狼毫|ShowSettings" src rime-data README.md`

Expected: `ShowSettings` 只出现在设置消息里。其余无匹配。`rime-data` 的 `rg "abbrev:|reverse_lookup"` 仍无匹配。

- [ ] **Step 4: Commit**

```bash
git add README.md
git -c user.name=lanzh -c user.email=lanzh@localhost commit -m "docs: describe the pinyin and voice input method"
```

手工记事本验收不在这个提交里声称完成。
