# VoiceIme

Windows 上的本地全拼加语音输入法。无账号。产品行为见 `docs/superpowers/specs/2026-11-13-pinyin-voice-ime-design.md`。

拼音引擎是静态 librime。方案和词库在 `rime-data`，许可证说明见 `rime-data/SOURCE.txt`。个人拼音数据在 `%LOCALAPPDATA%\VoiceIme\rime`。语音词库仍是 `%LOCALAPPDATA%\VoiceIme\lexicon.sqlite`。

全拼用空格上首选、数字 1 到 5 选词、Shift 切换中文和英文。语音仍是 Ctrl+Alt+Space 开麦，回车写入定稿，Esc 作废。设置在托盘和候选条右侧。

## 构建

需要 CMake 和 C++17 编译器。这台开发机没有 MSVC，使用 WinLibs 的 g++ 和 Ninja。构建依赖静态 librime。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\voiceime_tests.exe
```

构建 `voiceime_tsf.dll` 之后，生成后步骤把源码树的 `rime-data` 复制到 DLL 同目录（`build\rime-data`）。输入法从 DLL 目录加载方案，不从源码树加载。测试仍使用源码树的 `rime-data`。

带一个参数时只跑名字里包含该参数的测试，例如 `build\voiceime_tests.exe utf8`。

## 模型

`tools\download_models.ps1` 下载 sherpa-onnx v2.0.1 的 Windows x64 动态库，以及三个本地中文模型：

- `models/streaming/` 流式 Zipformer CTC，给确认框里的草稿
- `models/final/` 非流式 Zipformer CTC，停口后的定稿
- `models/punct/` 标点

权重和 SDK 不进 git。2025 年 CTC 权重的许可以各模型目录里的说明为准。引擎不上传、不保存音频。

三个目录缺一个时，`build\voiceime_engine.exe` 打印缺少的路径，退出码 2。`build\voiceime_engine.exe --fake` 不加载模型。

## 注册

`build\voiceime_register.exe` 需要管理员权限。它写入 HKCU 和 HKLM，HKLM 成功后才调用 `InstallLayoutOrTip`，语言栏里才会出现「语音输入法」。HKLM 写失败时打印 `hklm denied`，退出码是 1，当前用户的项仍然保留。卸掉同样需要管理员：`build\voiceime_register.exe --unregister`，机器项删不掉时退出码是 1。

切换到「语音输入法」后，中文模式打全拼，候选条出现。空格上首选，数字 1 到 5 选当前页对应项，单独按 Shift 切换英文。默认 Ctrl+Alt+Space 开麦。确认框里的字按回车才写入正文，Esc 放弃。正在听的时候按回车会等定稿，插入的是定稿。引擎是同目录的 `voiceime_engine.exe`。托盘退出之后，下次激活输入法会再把它拉起来。快捷键文件是 `%LOCALAPPDATA%\VoiceIme\config.json`，没有这个文件就用默认键。设置在托盘和候选条右侧：可以录新的快捷键，也可以删除最近记住的词。单独的字母、空格、回车、Esc 和系统保留组合不会替换原来的键。

## 验收

下面每条都要在本机新开的记事本里做。已经开着的旧进程不算，因为它可能还映射着旧的 DLL。

- [ ] 语言栏选中「语音输入法」。打 `nihao`，出现候选，空格后文档里是「你好」，而不是 `nihao`。
- [ ] 打一句更长的全拼，空格上首选；数字键能选另一个候选；Esc 后文档里没有这段汉字。
- [ ] 按一下 Shift 后打 `abc`，文档里是 `abc`。再按一下 Shift，恢复全拼。
- [ ] 中文模式下按句号，文档里是中文句号。
- [ ] 光标在正文，按 Ctrl+Alt+Space，确认框出现在屏幕内。说话时正文不变。回车后正文出现定稿。Esc 的那一次正文不变，语音词库不增加。
- [ ] 有未选定拼音时按语音快捷键，拼音不上屏，确认框出现。
- [ ] 拔网线或断开网络后，`nihao` 和语音快捷键仍能走通。本机没有可用麦克风时，拼音验收仍然算数，语音验收记为阻塞在麦克风，不假装通过。
- [ ] 进程目录和 `%LOCALAPPDATA%\VoiceIme` 下不产生音频文件。

命令行窗口、Windows Terminal 不在这次验收里。当前设计只保证使用 TSF 的普通文本框。
