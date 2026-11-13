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
