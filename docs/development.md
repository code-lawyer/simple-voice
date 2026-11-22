# 开发与验证

目标为 Windows Intel/AMD x64，C++17、Win32/GDI、TSF、静态 librime 和 sherpa-onnx。
本轮工具链为 WinLibs GCC 16.2.0 / MinGW-w64 UCRT POSIX、CMake 与 Ninja。
不支持 MSVC、32 位或 ARM64 构建。系统需要 PowerShell 5.1+、curl、tar 和 VC++ x64 Runtime。

## 构建

安装 [WinLibs](https://winlibs.com/)、[CMake](https://cmake.org/download/)、[Ninja](https://ninja-build.org/)，加入 PATH：

```powershell
git clone https://github.com/code-lawyer/simple-voice.git
cd simple-voice
# 仅编译及自动回归不需要下载模型权重
./tools/download_models.ps1 -RuntimeOnly
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure -j1
```

首次从官方仓库构建固定版本 Rime 依赖。模型和 SDK 不进 Git。
同一工具链复用已有静态依赖可加：

```powershell
cmake -S . -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DVOICEIME_USE_PREBUILT_RIME=ON `
  -DVOICEIME_RIME_PREFIX="$((Get-Location).Path)/build/rime-prefix"
```

使用新目录构建，不能覆盖系统已注册或仍被应用加载的 DLL。
`VOICEIME_TESTING` / `VOICEIME_UI_TESTING` 仅用于独立测试可执行文件，生产引擎和 DLL 不接受 fixture 变量。
源词表变动会在每次构建同步到可执行文件目录；个人候选数据库在新组合开始时按代次刷新。

## 验证层次

CTest 五组：单元/并发，模拟采音的真实引擎 IPC，真实 Windows TSF 隔离宿主，原生浮层稳定性，模型 worker 故障恢复。
不修改系统注册、生产词库或用户窗口。包括不安全改写、迁移/保留期限、拼音英语路由、
组合光标、原生编辑器语音状态隔离、切换模型/取消迟到回调和写入断线保护。
IPC v2 还覆盖连接身份、前台授权、话轮关联、跨应用确认和快捷键配置通知。
单元测试的临时词库和设置位于所选构建目录的 `unit/`，每次运行使用独立子目录。
测试须使用普通桌面用户令牌；受限沙箱令牌可能被身份校验或 Windows TSF 拒绝。
权限边界、夹具范围和同时升级 DLL/引擎的要求见 [本地 IPC 约定](ipc-security.md)。

```powershell
foreach ($skin in @('titanium','daylight','deep-space')) {
  $env:VOICEIME_TEST_SKIN=$skin
  ./build/voiceime_ui_check.exe "build/ui-$skin"
  if ($LASTEXITCODE) { throw 'Native UI assertions failed' }
}
./tools/download_models.ps1
./build/voiceime_engine.exe --check
./build/voiceime_engine.exe --check-chinese
./build/voiceime_recognizer_smoke.exe
# 16kHz/mono/PCM16 WAV；TSV字段：id、路径、分号分隔关键词
# <empty>检查静音，<observe>仅记录，不冒充准确率检查
./build/voiceime_recognizer_smoke.exe path/to/corpus.tsv
./build/voiceime_recognizer_smoke.exe path/to/chinese-corpus.tsv --chinese
```

UI 工具测试键盘、分页、编辑/恢复、可访问性对象、长文本、高对比和 DPI 消息。
`preferences-bottom-dpi-*` 是合成 DPI 后的客户区截图，避免把 DWM 非客户区残影当成真实布局。
这不验证物理多屏、完整屏幕阅读器、真人麦克风或浏览器/Codex 场景。

本机选型和复现工具：

- [语音对比](research/asr-selection-2026-11-15.md)、`tools/asr-benchmark/`。
- [拼音数据与候选评测](research/pinyin-selection-2026-11-15.md)、`tools/pinyin-data/`。
- [rc.12 的语音与跨应用词库回归](releases/v0.2.0-rc.12-validation.md)。

`voiceime_installed_check.exe <安装目录>` 是另行部署后才运行的生产检查，会使用系统 COM 注册并可能启动生产引擎。
附加 `--pinyin` 可在本工具的前台窗口核对拼音候选。
不在本轮开发回归或 CI 中运行；不能把源码测试结果称为已安装版验证。

## CI 与打包

`.github/workflows/windows.yml` 使用 Windows 2022 + MSYS2 UCRT64，执行构建、五组 CTest、
原生控件检查及包生成。手动 workflow_dispatch 可勾选真实模型加载/样本 smoke。
推送或打开 Pull Request 会触发工作流，构建与测试结果见 [GitHub Actions](https://github.com/code-lawyer/simple-voice/actions/workflows/windows.yml)。
CI 使用固定哈希的 SDK，MSYS2 编译器跟随软件包仓库版本，二进制字节可能随工具链而异。

```powershell
./tools/package.ps1 -BuildDir build -OutputDir dist/rc
```

版本取自 `src/product.hpp`，四份 PE 版本必须一致（引擎、模型 worker、TSF DLL、注册助手）。公开词表按 `rime-data/SHA256SUMS`
逐文件校验后打包；其余采用明确白名单。模型、SDK、测试程序、个人数据和本机原始日志不进入 ZIP。
包内 `package-manifest.json` 记录每个文件的 SHA-256；ZIP 另附 `.sha256`。

发布前应在全新解压目录、与源码无关的当前目录运行 `tools/setup.ps1 -Action Check`，
校验默认及兼容模型；可选的 SenseVoiceSmall、Whisper Turbo 另以隔离样本验证。
再按照真人验收清单决定公开发行。`install.cmd` 会请求管理员注册，
`Check` 不注册。版本升级使用新目录。替换已注册版本后，重启 Windows 才会开放拼音学习库。回滚时先卸载当前版本并重启，再安装旧版。

兼容标识仍为 `voiceime_*`、`%LOCALAPPDATA%\VoiceIme` 及既有 TSF GUID。
产品名 Simple Voice / 轻音输入法；当前行为以 README、PRODUCT 为准，视觉规则以 DESIGN 为准。
