# 开发与验证

目前仅构建 Windows x64。使用 C++17、Win32/GDI、TSF、静态 librime 和
sherpa-onnx。已验证工具链为 WinLibs GCC 16.2.0 / MinGW-w64 UCRT POSIX、
CMake、Ninja；当前构建脚本不支持 MSVC、ARM64 或 32 位。

## 从源码构建

安装 [WinLibs](https://winlibs.com/)、[CMake](https://cmake.org/download/) 和
[Ninja](https://ninja-build.org/)，将其加入 PATH。系统需要 PowerShell 5.1+、
`curl.exe`、`tar.exe` 和 Microsoft Visual C++ x64 Runtime。

```powershell
git clone https://github.com/code-lawyer/simple-voice.git
cd simple-voice
powershell -NoProfile -ExecutionPolicy Bypass -File tools/download_models.ps1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

首次构建会从官方仓库下载并编译固定版本的 Rime 依赖。模型和 SDK 不进 Git。
已下载的文件会做 SHA-256 校验；网络中断后可以重试。

复用已经构建的第三方依赖：

```powershell
cmake -S . -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DVOICEIME_USE_PREBUILT_RIME=ON `
  -DVOICEIME_RIME_PREFIX="$((Get-Location).Path)/build/rime-prefix"
cmake --build build-dev -j 4
ctest --test-dir build-dev --output-on-failure
```

不要覆盖系统当前注册或仍被其他应用加载的 DLL 目录。使用新构建目录，并在
安装更新后注销 Windows 再登录，使各个应用加载同一版 DLL 和引擎。

## 验证边界

CTest 包含单元/并发、隔离引擎 IPC、真实 Windows TSF 文本存储和原生浮层稳定性
四组检查。它们不修改系统输入法注册或个人词库；引擎集成使用模拟采音。

```powershell
# 三段上游 WAV、真实识别模型、先预览后确认、词库异常回退
.\build\voiceime_recognizer_smoke.exe
# 原生截图、长文、鼠标命中、高对比度和 DPI 消息
.\build\voiceime_ui_check.exe build/qa-ui
# 验证模型和字典可初始化；不注册、不采音、不读写个人配置
.\build\voiceime_engine.exe --check
```

`voiceime_installed_check.exe <安装目录>` 只在需要核实真实安装状态时手动运行：
它通过系统 COM 加载已注册的 DLL，并核对生产引擎，不加入 CTest。
它可能启动并保留生产引擎，但不启动麦克风。

真人麦克风、浏览器/Codex 的完整输入过程、物理多屏和屏幕阅读器仍须现场验收。
自动化 TSF 测试不等同于这些验收。

## 发布包

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package.ps1 -BuildDir build
```

只打包应用、运行库、公开字典、许可证和安装脚本，采用明确的文件清单。
下载的模型、SDK、测试程序、个人数据和本机审查记录不进入发布包。
发布产物位于 `dist/`，包括 ZIP 和 SHA-256 文件。

技术兼容标识仍为 `voiceime_*`、`%LOCALAPPDATA%\VoiceIme` 和已有 TSF GUID；
对外产品名为 **Simple Voice / 轻音输入法**。保留标识是为了复用已有个人数据。

`docs/superpowers/` 保存早期设计与实现记录；当前用户行为以主 README 和
`PRODUCT.md` 为准，视觉规则以 `DESIGN.md` 为准。
