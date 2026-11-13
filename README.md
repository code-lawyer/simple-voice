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

语音模型到实现计划的 Task 8 才下载。当前测试不需要模型文件。
