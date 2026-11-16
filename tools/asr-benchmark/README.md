# ASR 模型选择的可复现基准

这是 2026-11-15 模型选择研究的独立工具，不调用输入法、麦克风、用户词库、标点后处理或在线识别服务。它保留小样本诊断方法和失败案例，不是通用 ASR 排行榜。

源码不包含模型权重、DLL、WAV、个人数据或本机绝对路径。默认生成目录为仓库内 `build/asr-benchmark/`；可用 `--work-root` / `-WorkRoot` 指定另一目录。准备脚本仅读取并校验复用目录，不向正式 `models/` 或系统安装目录写入。

## 文件与默认行为

| 文件 | 作用 |
|---|---|
| `models.json` | 固定运行时/模型下载 URL、压缩包和必需文件 SHA-256、许可来源、配置映射 |
| `corpus.json` | 16 段诊断语料的来源、参考类别、明文提示词、原研究音频 SHA-256 |
| `prepare.mjs` | 复用或下载公开资源、校验哈希、准备隔离语料和本地布局 |
| `verify-layout.mjs` | 每次计时前重新核对实际模型、运行时和音频字节 |
| `generate-tts.ps1` | Windows 离线合成三段固定英文提示词，输出 WAV，不播放 |
| `benchmark.cpp` / `run.ps1` | 编译并串行调用 sherpa C API，记录加载、逐段计算、文本与内存 |
| `score.mjs` | 检查每轮每段完整性，分别计算 TTS WER、数字保留/语义、报号 CER |
| `stress.mjs` | 构造 60 秒重复句，测试整段、15–20 秒切分和 10–15 秒切分 |
| `recorded-summary.json` | 原研究的逐段统计与输出、三轮稳定性、长音频结果快照；可与新运行比较 |

默认 CPU、2 线程、每配置独立进程、三轮轮流运行，模型之间不并发。默认六个配置：原中文 CTC 流式/离线、SenseVoice auto、双语 Zipformer、正确结束末块的双语 Paraformer、Qwen auto。没有 GPU、热词、外部标点或模糊词库改写。

## 准备和运行

需要 Windows x64、Node.js、Windows PowerShell、可编译 C++17 的 MinGW-w64 `g++` 和支持 `.tar.bz2` 的 `tar`。本轮使用 Node.js 24.16.0 和现有 MinGW 环境验证。脚本不安装工具链、不启动后台服务、不修改输入法注册。模型和解压缓存需要数 GiB 磁盘；Qwen 的瞬时内存明显高于权重文件大小。

从仓库根目录执行一次完整准备，再进行默认三轮对比：

```powershell
node tools/asr-benchmark/prepare.mjs --work-root build/asr-benchmark --download --generate-tts
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/asr-benchmark/run.ps1 -WorkRoot build/asr-benchmark
```

`--download` 明确允许获取清单中的公开资源，缺失文件时才下载。下载后先校验 SHA-256，再解压；已有文件哈希不一致会失败，不会用新文件悄悄覆盖旧资源。解压保留上游包内许可材料。Paraformer 只获取固定 Hugging Face commit 的必需 int8 文件，避免下载完整 fp32 包。

有原研究缓存时，可以离线复用：

```powershell
node tools/asr-benchmark/prepare.mjs --work-root build/asr-repro --reuse-root build/asr-benchmark-2026-11-15 --reuse-root .
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/asr-benchmark/run.ps1 -WorkRoot build/asr-repro
```

`--reuse-root` 可以重复。脚本检查该目录本身、模型包同名子目录、`models/<包目录>`，以及清单里的项目相对位置；音频检查该目录、`audio/` 和 `corpus-snapshot/`。所有复用的模型和公开 WAV 仍校验哈希。没有资源且不带 `--download` 时，脚本会说明缺少哪个固定资源，不会访问网络。

只运行部分配置或缩短为一次工具检查：

```powershell
node tools/asr-benchmark/prepare.mjs --work-root build/asr-repro --reuse-root build/asr-benchmark-2026-11-15 --reuse-root . --configurations zip_online,qwen
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/asr-benchmark/run.ps1 -WorkRoot build/asr-repro -Configurations zip_online -Repeats 1
```

语料仍是完整 16 段；干净环境中的语料准备可能需要其他模型包里的测试 WAV，所以只选两个推理模型不一定只下载两个包。`-Compiler` 可指定编译器，`-Threads` 可更改线程数，但不同线程数的结果不能冒充本轮 CPU 2 线程复测。

若要重现语言及旧 Paraformer 末块的对照，准备时将 `--configurations` 设为 `ctc_online,ctc_offline,sense,zip_online,para_online_final,qwen,sense_zh,qwen_chinese,para_online`，随后使用默认运行命令。`para_online` 故意保留旧结束方式，仅作反例；默认是修正后的 `para_online_final`。Qwen auto 不设置 language；固定中文使用 `Chinese`，不是未经映射的 `zh`。

## 语料来源和 Windows TTS

| 语料 | 获取方式 | 参考用途 |
|---|---|---|
| 三段原中文 WAV | 固定中文 CTC 包的 `test_wavs/` | 一段官方 ASR 演示输出，另两段无参考回归 |
| ALWAYS / 星期混说 | 固定双语 Zipformer 包的 `test_wavs/` | 观察英文保留；不是独立人工金标准 |
| SenseVoice 中英文样例 | 固定 SenseVoice 包的 `test_wavs/` | 官方 ASR 演示输出，单独诊断 |
| 嘈杂中文报号 | 固定 Qwen 包的 `test_wavs/noise2.wav` | 发布者参考稿，背景说话可能没有全部标注 |
| 四个真实短数字 | [FSDD 固定版本](https://github.com/Jakobovski/free-spoken-digit-dataset/tree/26eb9aaf76e81b692f806f9140c2d2777410d7a1) 的固定文件 | 数据集数字标签 |
| 三段英文 TTS | 本机 Microsoft Zira Desktop、16 kHz mono PCM16、Rate 0、Volume 100 | 已知提示词，合成声音诊断 |
| 两秒静音 | 本地生成全零 PCM | 检查无语音时的输出 |

每段来源和文件哈希都在 `corpus.json`，模型包 URL 和 SHA 在 `models.json`。`sense_en_gold` 是历史样本 ID，实际 `reference_kind` 明确为 `upstream_asr_example_not_gold`，不能因 ID 中的 gold 就把它当人工标注。

三段 TTS 的明文为：

- `Please open the project and save the file.`
- `Hello world.`
- `GitHub. Python. Simple Voice. Codex.`

`--generate-tts` 使用已安装的 Windows 语音，不使用麦克风或扬声器。其他 Windows/语音版本可能生成不同字节：脚本会记录实际 SHA 和是否与原研究一致，不把不同音频伪装成同一份语料。可用 `--voice` 选择已安装语音，但此时结果是新的合成声音诊断。本轮验证中，受限沙箱无法正常调用该语音，正常 Windows 进程可完成生成；遇到语音初始化错误时可在普通终端运行或复用已有诊断 WAV，不需要脚本自行提权。

TTS 只验证基础英文能力和术语退化，不能代表真人口音、设备噪声或用户验收。音频没有纳入源码；原录音和语音模型也不能因测试工具的项目许可就被视为同一许可。

## 60 秒压力复现

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/asr-benchmark/run.ps1 -WorkRoot build/asr-repro -Suite stress -Include2048
```

默认三轮分别测试：整段 60 秒、在第 15–20 秒内找最低能量切点、在第 10–15 秒内找最低能量切点。`-Include2048` 额外执行一次调大 Qwen 上下文参数的诊断，与原研究相同。`-Suite all` 会连同主语料一起执行。

构造方法是重复普通英文 TTS，间隔 250 ms 静音，填满 60 秒后补零。原研究语音可容纳 18 次；换声音后重复数由实际时长计算并写入 `stress.json`。切点按 20 ms RMS、10 ms 步长搜索，同能量取最晚；切段无重叠，逐字节重建 PCM 检查覆盖完整。文本只按段顺序拼接，**绝不删除重复句**。

原结果必须保留：整段 1024 为 15/18，2048 单次仍为 15/18；15–20 秒切段为 17/18；10–15 秒切段三轮为 18/18。最后一项成功不保证自然长句不漏词。RMS 最小值也不等于真实静音，不能把此实验分割器直接当作生产 VAD。

## 结果和评分

每次运行创建新的 `results/<UTC时间>/`，已有非空输出目录会被拒绝覆盖。`run.json` 保存实际计划、线程数、编译器和源码/清单哈希；JSONL 保存每轮加载和每段输出；`summary.json` 保存评分。`-ResultsRoot` 可以指定另一个空目录。

评分器按运行计划检查每轮每段和加载记录，缺失或重复记录会失败。主测试每轮单独评分，结果有变化时会显示 `stable=false`，不会用第一轮掩盖后续变化。

- TTS WER 只有 3 段、15 个词；不将 ASR 演示输出纳入总体准确率。
- 短数字分别记录原英文/阿拉伯数字保留与数值语义正确。Qwen 固定中文输出“零、一、七、九”是语义正确但未保留英语，不能笼统算成四次数字错误。
- 报号 CER 单独归一化 `幺/一` 及数字书写；不会从其他输出猜回号码。本轮 Qwen 报号仍比原中文 CTC 差。
- 标点/大小写归一化不改变数字顺序，不做繁简转换或文本去重。
- 工作集峰值来自 Windows `PeakWorkingSetSize`；private 是加载后和每段结束时采样的最大值，不是连续 private 峰值。历史快照中的 `peak_private_mib` 也仅有此含义。
- 流式计算尽快送入 20 ms 音频块；首次非空结果对应送入的音频时长，不能宣称为真人麦克风端到端延迟。

空闲时在相同线程数、相同模型/语料哈希下比较；系统文件缓存没有被清空，主机负载也不是实验室固定值。更换录音、TTS 声音、推理配置或热词后，应记为新的实验。

## 各模型的许可边界

- sherpa-onnx 运行时为 Apache-2.0；其 ONNX Runtime 为 MIT 及相应上游依赖许可。
- Qwen 原权重模型卡为 Apache-2.0，本轮是 sherpa 发布的 Wasser1462 ONNX 转换版本；保留两层归因。
- 双语 Zipformer 的转换和原始 pfluo 模型卡均声明 Apache-2.0。
- 旧中文离线 CTC 模型卡及维护者说明为 Apache-2.0；旧中文流式 CTC 的转换卡没有明确权重许可，不能推定继承运行时 Apache 许可。
- SenseVoice 源码 MIT，模型权重为 FunASR Model License 1.1，两者不能混同。
- Paraformer 转换卡声明 Apache-2.0；若进一步分发，原 ModelScope 权重许可链仍需确认。
- FSDD 音频为 CC BY-SA 4.0；Windows 合成语音与其他测试音频按各自来源处理。

精确一手来源见 `models.json`、`corpus.json` 和[研究报告](../../docs/research/asr-selection-2026-11-15.md)。本目录不重新许可或重新托管这些权重、语音和数据。

## 本次工具交付的验证

完成了缓存复用与全部必需哈希验证、实际编译、一次 16 段 Zipformer 运行（输出逐段等于原研究）、对已有 9 配置 × 16 段 × 3 轮原始记录及四种长音频配置的重新评分，以及压力音频的逐字节覆盖检查。缺段的实际失败运行被新评分器拒绝；TTS 在正常 Windows 进程生成的三个 WAV 哈希与原研究一致。没有为整理工具重新运行全部模型横评，也没有把这些检查算作产品或真人验收。
