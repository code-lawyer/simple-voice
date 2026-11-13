# VoiceIme

Windows 上的本地语音输入法。产品行为见 `docs/superpowers/specs/2026-11-13-voice-ime-design.md`。

## 构建

需要 CMake 和 C++17 编译器。这台开发机没有 MSVC，使用 WinLibs 的 g++ 和 Ninja：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\voiceime_tests.exe
```

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

切换到「语音输入法」后，默认 Ctrl+Alt+Space 开麦。确认框里的字按回车才写入正文，Esc 放弃。正在听的时候按回车会等定稿，插入的是定稿。引擎是同目录的 `voiceime_engine.exe`。托盘退出之后，下次激活输入法会再把它拉起来。快捷键文件是 `%LOCALAPPDATA%\VoiceIme\config.json`，没有这个文件就用默认键。托盘可以录新的快捷键，也可以删除最近记住的词。单独的字母、空格、回车、Esc 和系统保留组合不会替换原来的键。
