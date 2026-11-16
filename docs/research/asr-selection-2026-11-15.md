# 轻音输入法：本机 ASR 模型对比与选型证据

日期：2026-11-15。范围：Windows x64、本机 CPU、隔离模型与公开音频；未访问麦克风、未上传音频、未替换正式模型。研究目录为 `build/asr-benchmark-2026-11-15/`。

## 1. 选型结论

建议将 **中英双语 Zipformer 流式草稿 + Qwen3-ASR-0.6B int8 离线定稿**作为中英混说的默认组合；Qwen 不固定语言。保留旧中文 CTC 定稿模型，由用户明确选择“中文优先”，不按草稿内容自动切换模型。

这是一项有取舍的选择。Qwen 在本轮英文术语和混说诊断样本上最好，但并非所有中文都更准：嘈杂中文报号样本中，旧中文 CTC 的归一化 CER 为 7/53，Qwen 为 18/53；Qwen 还转写了发布者参考稿没有收录的背景内容。此处不能直接断言都是模型幻觉，也不能用词库或 LLM 猜回数字。

建议上线边界：录音仍可保留 60 秒上限，但离线定稿按停顿和最多 15 秒的有界音频段执行，优先在 10–15 秒之间找低能量停顿；原始识别结果可检查；数字、名称和英文缩写不做无依据的强制改写。长音频的压力测试见第 5 节：**60 秒整段和最多 20 秒切段均出现漏句；最多 15 秒切段三轮保留全部 18 次重复句，但这仍不能保证所有自然长句的准确性。**

选 Zipformer 而非 Paraformer，是因为本轮两个 `ALWAYS` 混说保留完整、英文术语少一词错误，加载后工作集也较小。Paraformer 在非常短的单独数字上更好，应该保留为研究候选，不能把本轮选择解释为全面胜出。

## 2. 方法与证据边界

- 机器：Intel Core Ultra 7 255H，16 个逻辑处理器，约 31.59 GiB 物理内存。
- 运行时：项目已有 `sherpa-onnx v2.0.1` Windows x64 C API；所有模型均为 `provider=cpu`、`num_threads=2`、greedy，热词为空；无用户词库、纠错或独立标点模型参与。
- 每个配置启动独立进程，加载一次后处理相同 16 段音频，重复三轮。主对比共六种模型，补充了 SenseVoice 固定中文、Qwen 固定中文及 Paraformer 正确末块处理的三轮实验。Zipformer 与 Qwen 另有一次减少主项目编译干扰后的复测。
- 主对比前半段可能受主项目编译和系统调度影响；不是专用性能实验室，也不是清空系统文件缓存后的冷启动。表中加载时间为三轮进程加载中位数；内存是进程工作集，不是模型文件大小或 GPU 显存。
- 离线耗时包含 stream 创建、音频接收、推理、结果读取和销毁，不含 WAV 文件读取。流式以 20 ms 块尽快送入，记录首次非空结果对应的“已送入音频时长”，结束时补 300 ms 零音频并 `InputFinished`；这不是麦克风到输入框的端到端延迟。
- 原始 8 kHz 文件交由相同 sherpa 特征前端重采样；未通过产品的实际采音/重采样路径。模型测试不能替代集成回归、浏览器/Codex 输入测试或真人验收。

语料在推理前固定于 `corpus.json`，并保留每个文件的 SHA-256 与副本。16 段包括：项目原有三个中文 WAV、三个离线 Microsoft Zira 英文 TTS、官方 `ALWAYS` 混说样例、SenseVoice 的中英文公开样例、FSDD 四个真实短数字、Qwen 官方嘈杂报号样例、Zipformer 星期混说样例、两秒全零静音。

参考文本的可信程度必须分开：

| 参考类型 | 本轮用途 | 限制 |
|---|---|---|
| 已知 TTS 提示词，3 段、共 15 个英文词 | 英文 WER | 单一合成声音，不能代表真人口音或环境噪声 |
| FSDD 文件标签，4 段真实数字，0.36–0.64 秒 | 单词/阿拉伯数字保留率与数字语义正确率 | 极小样本，原始音频头尾静音很短 |
| Qwen 发布者提供的报号文本，53 个归一化字符 | 单独报告报号 CER | 可能没有收录全部背景说话，不等同独立标注集 |
| 官方文档中的 ASR 演示输出 | 混说、退化与输出形式诊断 | 是模型输出，不当成人工金标准；不能混入总体准确率 |
| 无参考的旧中文和星期混说 WAV | 比较输出是否丢信息 | 不能据此计算可信总体 CER |

WER 使用 NFKC、小写、按标点与空格拆词；CER 使用 NFKC、小写后保留字母数字，删除标点空白。报号另将 `幺` 归一为 `一`、阿拉伯数字逐位转汉字，避免仅因数字书写方式计错。未进行繁简转换。完整逐段结果与最小/最大/中位耗时在 `summary.json`，没有构造“总体准确率”或 p95。

## 3. 主对比结果

以下均为三轮结果；文本在各自三轮中一致。Paraformer 使用正确设置 `is_final` 后的结果。

| 模型 | 加载中位 ms | 加载后工作集 MiB | 16 段内峰值工作集 MiB | TTS WER（错误词/15） | 短数字保留（/4） | 报号归一化 CER | 两秒静音 |
|---|---:|---:|---:|---:|---:|---:|---|
| 原中文 CTC 流式 | 1550 | 241 | 256 | 15/15 | 0 | 28/53 | 空 |
| 原中文 CTC 定稿 | 3271 | 476 | 692 | 15/15 | 0 | **7/53** | 空 |
| 双语 Zipformer 流式 | 1140 | 250 | 272 | 2/15 | 0 | 16/53 | 空 |
| 双语 Paraformer 流式 | 898 | 293 | 305 | 3/15 | **3** | 13/53 | 空 |
| SenseVoice auto | 1147 | 311 | 368 | 2/15 | 2 | 25/53 | `그.` |
| Qwen3-ASR auto | 3276 | 1045 | 2319 | **0/15** | 2 | 18/53 | 空 |

“短数字保留”接受原英文单词或等值阿拉伯数字。表中的 0/15 只表示这三个既定 TTS 样本没有词错误，不能对外写成“英文识别零错误”。四个短数字也不足以给出语言识别率。

有代表性的输出：

| 场景 | 原中文定稿 | SenseVoice auto | 双语 Zipformer | 双语 Paraformer | Qwen auto |
|---|---|---|---|---|---|
| GitHub / Python / Simple Voice / Codex | `<unk>` | `GHub Python Sple voice Codex` | `PYTHON SIMPLE VOICE CODAX` | `get up python simple voice code` | 四项完整保留 |
| 中文夹两个 ALWAYS | `<unk>`，英文丢失 | `OSOS` | 两个 ALWAYS 保留 | 一个 always，另一个 `o s` | 两个 always 保留 |
| 开放时间的中文样例 | `开放时间` | `开饭时间` | 详见原始结果 | 详见原始结果 | 繁体 `開放時間`，时间用阿拉伯数字 |

旧中文模型不能识别完整英文，是本轮隔离测试直接复现的模型能力问题，不是词库界面或最终文本落框时机造成的。Qwen 能保留术语也不等于词库改写可以无限制放开。

### 性能与交互

| 音频 | 时长 | 原中文定稿 ms | SenseVoice ms | Zipformer 流式总计算 ms | Paraformer 流式总计算 ms | Qwen 定稿 ms |
|---|---:|---:|---:|---:|---:|---:|
| 英文普通句 | 3.01 s | 103 | 70 | 139 | 161 | 677 |
| Hello world | 1.65 s | 70 | 49 | 67 | 101 | 470 |
| 英文术语列表 | 6.24 s | 178 | 125 | 264 | 285 | 854 |
| ALWAYS 混说 | 5.10 s | 147 | 105 | 209 | 253 | 927 |
| 原中文介绍 | 5.61 s | 181 | 118 | 243 | 289 | 1190 |
| 嘈杂中文报号 | 22.83 s | 677 | 469 | 955 | 1021 | 4475 |

减少编译干扰后的第四次 Qwen 复测分别约 651、480、875、929、1136、4459 ms，输出一致；Zipformer 也输出一致。性能结论没有依赖某次异常快的单次测量。

Zipformer 普通英文句首个非空结果在送入 420 ms 音频后出现，旧中文流式在 800 ms 后输出了错误中文。术语列表中 Zipformer 丢了最前面的 GitHub，首次非空直到 2340 ms，说明“流式支持英文”不等于“每个短术语都及时正确”。Qwen 停顿定稿仍有必要；最终文本必须在浮层确认后提交。

## 4. 语言、末块与静音的集成细节

**默认不把 UI 语言固定为 ASR 语言。** SenseVoice 的 `language="zh"` 三轮中 TTS 错词从 2 降为 1，但 ALWAYS 仍为 OSOS，静音变成“我。”，短数字仅 1/4。单一术语的偶然改善不足以证明固定中文更稳。

Qwen C 配置结构没有 language 字段，但 v2.0.1 支持 `SherpaOnnxOfflineStreamSetOption(stream, "language", "Chinese")`。运行时把该值直接放入提示；官方用的是 `Chinese`、`English` 等完整名称。自动识别是**不设置该选项**，不要把 UI 的 `zh` 或字面量 `auto` 无映射传入。实际源码见 [语言提示构造与读取](https://github.com/k2-fsa/sherpa-onnx/blob/v2.0.1/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc#L460)。

Qwen 强制 Chinese 的三轮结果：普通英文句、术语仍正确；四个孤立英文数字变成“零、一、七、九”，**数值语义 4/4，英文/阿拉伯数字保留 0/4**。不能把它们笼统称为数字识别错误。它还把原本的繁体开放时间样例输出为简体，但纯静音出现“嗯。”，报号 CER 20/53。默认自动更符合保留原话与中英混说的需求；繁简书写应作为明确输出规范处理，不能靠强制语言偶然控制。

**Paraformer 末块必须另行结束。** 单纯 `InputFinished + 300 ms tail` 在此模型上留下了尾字不完整现象。v2.0.1 的 [Paraformer IsReady](https://github.com/k2-fsa/sherpa-onnx/blob/v2.0.1/sherpa-onnx/csrc/online-recognizer-paraformer-impl.h#L158) 检查 `is_final`；补 `SherpaOnnxOnlineStreamSetOption(stream, "is_final", "1")` 后，ALWAYS 样例的最后“什么意思啊”恢复完整，短数字从 2/4 到 3/4。旧实验完整保留在 `para_online-*`，正式对比采用 `para_online_final-*`。

Zipformer 也必须在会话真正结束时 flush；本轮 ALWAYS 测试结束前草稿只到“什么”，flush 后才是“什么意思啊”。不应把暂停预览误当整个流结束，也不应销毁仍需要继续录音的流。

纯零音频可揭示模型会凭空输出；实际产品需用采音/VAD 的有效语音判定阻止无语音调用与错误提交。此测试没有接入 VAD，不能由“Qwen 静音本轮为空”推导其永不产生无声文本。

## 5. 60 秒容量与切段压力测试

额外将同一个已知英文 TTS 句子重复 18 次，每次间隔 250 ms 静音，剩余部分补零，构成整 60 秒音频。参考为 18 次原句，没有文本去重；这是重复内容、内存和长度压力检查，不是新增独立语音准确率样本。

| 方案 | 轮数 | 返回完整句数 | 推理耗时 | 峰值工作集 | 最大已采样 private |
|---|---:|---:|---:|---:|---:|
| 整段，max_total_len=1024 | 3 | 15/18，每轮一致 | 中位 15.14 s，范围 14.64–15.68 s | 3.38 GiB | 4.20 GiB |
| 整段，debug + 2048 | 1，诊断 | 15/18 | 15.13 s | 3.60 GiB | 4.20 GiB |
| 15–20 秒窗内最低能量切段 | 3 | 17/18，每轮一致 | 中位 10.75 s，范围 10.66–11.55 s | 1.92 GiB | 1.96 GiB |
| 10–15 秒窗内最低能量切段 | 3 | **18/18，每轮一致** | 中位 10.13 s，范围 10.04–10.24 s | 2.04 GiB | 2.15 GiB |

15–20 秒实验以 20 ms PCM RMS、10 ms 步长，在下一段的第 15–20 秒范围寻找最低能量位置，同能量取最晚位置。得到 19.62、19.56、20.00、0.82 秒四段；所有切点 RMS=0，音频采样连续且无重叠，文本按段顺序直接拼接。四段分别返回 5、6、6、0 次原句。输入没有丢采样，但模型仍漏一句；**不得把“切段完成”或“没有报错”当作内容完整通过。**

随后将搜索区间缩短为 10–15 秒，其余算法、模型与推理参数不变。切成 13.10、13.04、13.04、13.04、7.78 秒，三轮均返回 4、4、4、4、2 次原句。归一化后 144 个参考词全部保留。`stress-summary.json` 对两种切段方案都通过逐字节 PCM 拼接检查，确认没有漏掉或重复输入采样。保留之前失败实验，不以这一例通过掩盖 20 秒方案的问题。

建议边界策略是“真实停顿优先、最多 15 秒、连续无重叠输入、顺序拼接、绝不删除重复话语”。RMS 20 ms 和绝对阈值并不是可靠 VAD：低能量片段也可能是轻声或辅音。实际实现应检查一段持续停顿，找不到可信停顿时保留全部采样并标明分段需要核对，不能把“最小能量”自动等同于无语音。固定阈值与自然噪声场景仍需集成/真人测试。

调大 max_total_len 的单次诊断未解决漏句，不能声称问题只来自可调整的上下文上限。该实现还受导出模型形状、音频 token 与生成 token 共同约束，实际配置和权重版本都必须固定。压力样本的内容重复也可能影响解码；本轮尚未唯一定位全部漏句机制。

资源方面应同时考虑常驻草稿、离线模型和实际进程，而不是只看 Qwen 加载后的约 1 GiB。工作集峰值使用 Windows 的 `PeakWorkingSetSize`；private 只在加载后和每段结束时采样，表内是这些采样的最大值，可能低于推理中的瞬时 private 峰值。长音频处理后的 ONNX 分配器可能保留高水位内存；表中是本轮进程实测，不能保证产品同样峰值或退出会话后自动回落。

## 6. 精确 C API 配置与文件布局

本轮使用完整 C API，未启动 Python、HTTP 服务或额外常驻服务。`benchmark.cpp` 是可复现的独立实现；以库头文件、DLL SHA 与模型 SHA 为实际基线。

公共参数：配置结构清零，`sample_rate=16000`、`feature_dim=80`、`num_threads=2`、`provider="cpu"`、`decoding_method="greedy_search"`。流式 `enable_endpoint=0`；停顿/结束属于产品会话状态，不能盲目套用测试的结束循环。

### 双语流式 Zipformer

模型目录：`sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20`。

```cpp
config.model_config.transducer.encoder = "<root>/encoder-epoch-99-avg-1.int8.onnx";
config.model_config.transducer.decoder = "<root>/decoder-epoch-99-avg-1.onnx";
config.model_config.transducer.joiner = "<root>/joiner-epoch-99-avg-1.int8.onnx";
config.model_config.tokens = "<root>/tokens.txt";
```

这里 decoder 有意使用官方 int8 示例中的**非量化 decoder**。四个必需文件共 199,056,205 字节，约 189.83 MiB。`bpe.model`、`bpe.vocab` 已列入可选文件清单，主测试不使用热词或 beam search。启用 native hotwords 需要单独配置和准确率验证，不能直接沿用本轮 greedy 结论。

### Qwen3-ASR-0.6B int8 定稿

模型目录：`sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25`。

```cpp
auto& q = config.model_config.qwen3_asr;
q.conv_frontend = "<root>/conv_frontend.onnx";
q.encoder = "<root>/encoder.int8.onnx";
q.decoder = "<root>/decoder.int8.onnx";
q.tokenizer = "<root>/tokenizer";  // 目录，不是 vocab.json
q.max_total_len = 1024;
q.max_new_tokens = 512;
q.temperature = 1e-6f;
q.top_p = 0.8f;
q.seed = 42;
q.hotwords = "";
```

必需文件还包括 `tokenizer/vocab.json`、`tokenizer/merges.txt`、`tokenizer/tokenizer_config.json`；总计 987,015,347 字节，约 941.29 MiB。不需要旧中文模型的 `tokens.txt`。默认 stream 不设置 language；独立标点器不应再次盲目处理已经含标点的 Qwen 输出。

v2.0.1 的 Qwen 原生支持 hotwords，放入 system 提示前缀，可按 stream 覆盖。不能把传统 transducer ContextGraph 的适用范围当作所有 sherpa API 的限制；也不能据此认为热词不会影响非热词。所有本轮质量结果均为**热词为空**，启用后需重新验证误插入、数字、常用词和上下文预算。[Qwen 原生实现](https://github.com/k2-fsa/sherpa-onnx/blob/v2.0.1/sherpa-onnx/csrc/offline-recognizer-qwen3-asr-impl.cc#L49)

## 7. 下载、哈希与许可

可随源码保存的模型清单：[models.json](../../tools/asr-benchmark/models.json)；语料来源和哈希：[corpus.json](../../tools/asr-benchmark/corpus.json)。隔离研究目录的原始 `model-artifacts.json` 仍保留更完整的本次下载记录。清单包含精确下载 URL、压缩包/文件 SHA-256、必需/可选区分、原始许可来源、运行时和测试音频哈希。本研究未修改 `tools/dependencies.json`。

| 包 | 下载字节 | SHA-256 | 核验方式 |
|---|---:|---|---|
| [Qwen int8](https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-qwen3-asr-0.6B-int8-2026-03-25.tar.bz2) | 878702423 | `393f8a14e2f5fb96746aaab342997a40641001fbd5bf9592a080a8329178ee96` | 本机匹配 GitHub 附件 digest |
| [Zipformer 双语](https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20.tar.bz2) | 511274346 | `27ffbd9ee24ad186d99acc2f6354d7992b27bcab490812510665fa8f9389c5f8` | 本机计算；旧附件无上游 digest |
| [SenseVoice int8](https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17.tar.bz2) | 163002883 | `7d1efa2138a65b0b488df37f8b89e3d91a60676e416f515b952358d83dfd347e` | 本机匹配 GitHub 附件 digest |

Paraformer 为避免下载包含 fp32 的约 999 MiB 完整包，仅下载其三个必需文件与 README，固定 [Hugging Face commit 8e40c43232a1c5c66c82111efc5820d3accca11b](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-paraformer-bilingual-zh-en/tree/8e40c43232a1c5c66c82111efc5820d3accca11b)。每文件 URL 和 SHA 在清单中，不虚构未下载整包的 SHA。

建议默认组合的必需文件 SHA：

| 模型 / 文件 | SHA-256 |
|---|---|
| Zip encoder-epoch-99-avg-1.int8.onnx | `8fa764187a261844f859d7143ebaa563af5d10adfece4c18a8f414c88cba2a9b` |
| Zip decoder-epoch-99-avg-1.onnx | `2e3b5ec371f8899ee6acd829fd753ba45772df57a91bdf37cde3136354e7db7d` |
| Zip joiner-epoch-99-avg-1.int8.onnx | `1ed689c5ed19dbaa725d9d191bb4822b5f4855a39e1ffd28cbc1f340d25b2ee0` |
| Zip tokens.txt | `a8e0e4ec53810e433789b54a5c0134a7eaa2ffca595a6334d54c00da858841d3` |
| Qwen conv_frontend.onnx | `d22dc4423e0940e49884e903d2ea2f7e5567c14fc1aed97e4e26d6b8f208ef9e` |
| Qwen encoder.int8.onnx | `60748d3e6744a57c9c91e1b17424a6c2990567e8adceb0783940c03ed98fa9d9` |
| Qwen decoder.int8.onnx | `4f6885be5959ae26af3089d38ee7972c5fafbeeb1cf8d5e76eab6d8b61ca5771` |
| Qwen tokenizer/vocab.json | `ca10d7e9fb3ed18575dd1e277a2579c16d108e32f27439684afa0e10b1440910` |
| Qwen tokenizer/merges.txt | `8831e4f1a044471340f7c0a83d7bd71306a5b867e95fd870f74d0c5308a904d5` |
| Qwen tokenizer/tokenizer_config.json | `4942d005604266809309cabc9f4e9cb89ce855d59b14681fdc0e1cc62ea26c4c` |

许可不是统一的“sherpa 开源所以模型都一样”：

- Qwen 原始 [0.6B 权重模型卡](https://huggingface.co/Qwen/Qwen3-ASR-0.6B) 声明 Apache-2.0。本轮实际使用 sherpa 发布的 ONNX 量化转换版，包内 README 归因于 Wasser1462 的 [转换项目](https://github.com/Wasser1462/Qwen3-ASR-onnx)，不等同原始 PyTorch 权重。发布应保留原模型和转换来源及适用许可文本。
- Zipformer 转换模型卡与 [原始权重作者 pfluo 的固定版本模型卡](https://huggingface.co/pfluo/k2fsa-zipformer-chinese-english-mixed/blob/6eb615ae77ecac05c5628d5c8ed7037c14a338d5/README.md) 均声明 Apache-2.0。将原模型作者/训练项目与 sherpa 转换来源一起列入第三方说明。
- SenseVoice 源码 MIT 不等于模型权重 MIT。其权重采用 [FunASR Model Open Source License Agreement 1.1](https://github.com/modelscope/FunASR/blob/58830eca4012644aac0c3218c3ccc7d98f003fda/MODEL_LICENSE)；项目方[正式说明](https://github.com/QwenAudio/SenseVoice/issues/334#issuecomment-5083546605)解释可商用并须保留作者、来源和模型名称等要求。不可把它标成 Apache-2.0 或“仅限非商用”。
- Paraformer 转换模型卡声明 Apache-2.0；本轮未选入默认组合。若之后发布，应进一步固定原始 ModelScope 权重的许可链，不能只凭转换仓库标签推定全部权利。
- 四个 FSDD 测试音频来自 [固定数据集版本](https://github.com/Jakobovski/free-spoken-digit-dataset/tree/26eb9aaf76e81b692f806f9140c2d2777410d7a1)，数据许可 CC BY-SA 4.0。仅放在隔离研究目录；不应无说明混入产品安装包。

## 8. 交付物与后续验证

关键产物均在 `build/asr-benchmark-2026-11-15/`：

- `benchmark.cpp` 及独立可执行文件：C API 直接调用，无产品后处理。
- `corpus.json`、`corpus.snapshot.tsv`、`corpus-snapshot/`：参考分类、可复现音频副本。
- `results/*.jsonl`：全部逐段三轮结果、语言对照、末块修正前后、减少编译干扰的第四轮与长音频实验。
- `summary.json`、`analyze.mjs`：可复算 WER/CER、文本一致性与资源统计。
- `model-artifacts.json`、`model-provenance.mjs`：精确下载、哈希、许可来源。
- `stress-reference.json`、`stress-split-manifest.json`、`stress15-split-manifest.json`、`stress-summary.json`：60 秒构造方式、两组连续无重叠切点、逐字节 PCM 覆盖验证与文本计数。

集成回归需要另行确认：真实应用的英文/中英草稿、停顿后的最终预览、继续录音时的分段状态、取消和切换模型、短词与数字不被词库改写、静音不提交、超过段长的边界提示，以及中文优先模式的实际模型选择。TTS 和公开 WAV 成功不能代替真人验收；真人验收依用户决定继续后置。

建议追加真人验收语料时优先覆盖：普通英文句、连续术语、人名/项目名、中英切换、数字纠正、轻声单词、有空调/键盘声的自然环境，以及自然说满 30–60 秒而不是重复单句。新语料到来前不把本轮小样本结论扩大为普遍准确率承诺。

## 9. 纳入源码的复现工具

已将本项目编写的基准源代码、准备/运行/评分脚本、公开语料来源、明文参考与模型哈希整理到 [tools/asr-benchmark](../../tools/asr-benchmark/README.md)。根目录可配置，默认 CPU 2 线程、三轮串行对比；权重、DLL、WAV、机器绝对路径和个人数据不纳入源码。公开资源下载是显式选项，复用缓存也校验固定哈希。

[recorded-summary.json](../../tools/asr-benchmark/recorded-summary.json) 保存本研究的逐段输出与统计快照、语言/末块对照和长音频失败/成功结果。新工具逐轮评分，分别报告英文保留、数字语义和不同参考来源；不把模型演示输出冒充人工金标准，也不去重重复话语。

交付时使用已有缓存完成所有必需文件哈希校验，重新编译并实际运行了 Zipformer 的完整 16 段一次，输出与原研究逐段一致；对原 9 配置 × 16 段 × 3 轮原始数据使用新评分器复算，指标一致。已验证缺段记录会被拒绝，压力语料两种切分的 PCM 可逐字节重建；Windows TTS 在正常进程下完成固定提示词生成。此轮整理没有重新进行全部模型横评或真人验收。完整命令、TTS 版本差异处理和各模型许可边界见工具 README。
