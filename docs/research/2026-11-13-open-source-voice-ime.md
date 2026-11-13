# 开源语音输入法调研

日期：2026-11-13  
问题：有没有开源的语音输入法，做到本地模型识别、本地词库，并记住用户习惯？有哪些功能块可以拆开参考？

## 结论

有接近的项目，没有一个同时覆盖这三件事、并且在 Windows 上作为真正输入法成熟可用。

现有项目分成三类：

1. **完整输入法，语音和词库并排放着。** Android 上的 Feelime、Linux 上的 WowType 最接近。语音是一条上屏路径，词库学习主要服务拼音选词。公开说明里看不到「用户改了语音识别结果，下次自动更准」这条闭环。
2. **本地听写工具，不是输入法。** Handy、SayIt、VocoType、LiltKey、fcitx5-vinput。按住快捷键说话，文字落到光标处。词库大多是人手维护的热词或替换表，不是用得越多越懂你。
3. **本地词库和习惯学习，没有语音。** Rime（librime）、水杉输入法（msime-engine）。这是「记住用户习惯」该抄的部分。

对这个仓库最有用的空白是：**Windows 上还没有一套开源、可再分发的「本地 ASR + 可学习词库 + 输入法上屏」。** Linux 已经有 Fcitx5 插件可以照着做，Windows 只有听写壳（SayIt、Handy）和纯拼音输入法（小狼毫 / 水杉的 TSF 前端）。

下面每个判断都跟到了项目自己的 README、许可证或维护者说明。星数、下载量会变，这里不引用。

## 和目标最接近的项目

| 项目 | 平台 | 本地识别 | 词库 / 习惯 | 是不是输入法 | 许可证 | 适合抄什么 |
| --- | --- | --- | --- | --- | --- | --- |
| [Feelime](https://github.com/feelime/feelime) | Android | sherpa-onnx：流式 Zipformer 出草稿，停顿后 Paraformer 整句纠错，再加标点模型 | librime 中文引擎，独立用户词库、连续造词 | 是，`InputMethodService` | GPL-3.0（1.0.19 及更早是 MIT） | 双通道识别、音频不落盘、拼音习惯与语音并列 |
| [VocoType-linux](https://github.com/LeonardNJU/VocoType-linux) | Linux Fcitx5 / IBus，macOS InputMethodKit | FunASR Contextual Paraformer，可选两遍；CPU | 图形化用户词典：热词、别名、保护词，YAML 为权威格式。人手维护，不是自动调频 | 接入系统输入框架，但不是拼音输入法 | GPL-3.0 | 热词、ITN、术语保护、按住说话、语音编辑 |
| [WowType](https://github.com/Gloridust/WowType) | Linux fcitx5，另有 macOS | 微信输入法离线模型，本机推理，模型不随仓库分发，从腾讯服务器下载 | 拼音侧会记住选过的词和常连用的词，30 天不用逐渐淡掉；可加自定义词 | 是，fcitx5 拼音输入法 | 仓库徽章为 MIT 与 Apache-2.0 | 习惯衰减、语音作为输入法里的一种状态 |
| [fcitx5-vinput](https://github.com/xifan2333/fcitx5-vinput) | Linux Fcitx5 | sherpa-onnx 离线，或豆包 / 百炼 / OpenAI 兼容云端 | README 未描述可学习词库 | Fcitx5 插件，和现有拼音共存 | GPL-3.0 | 插件形态、点按/长按、对选中文字做语音改写 |
| [LiltKey](https://github.com/tangzk/liltkey) | Linux Fcitx5 | SenseVoice：流式草稿，停顿或松键时再识别定稿；自动标点 | README 未描述用户词库 | Fcitx5 侧听写，原有拼音继续用 | 自有代码 MIT；模型和第三方另计 | CPU 上「草稿 + 定稿」两遍策略 |
| [FUTO Keyboard / Voice Input](https://github.com/futo-org/voice-input) | Android | Whisper、Parakeet、asr4all，完全离线 | 语音个人词典取自系统个人词典；词太多时只放进子集 | 键盘内置语音，也可给别的键盘当识别服务 | 源码公开，但是 FUTO Source First，不是 OSI 开源许可证 | 系统词典如何塞进有限上下文 |

### Feelime

Android 离线输入法。键盘 UI 是本地 HTML/CSS/JS，经 JS Bridge 调 `InputConnection`。语音是双通道：流式 Zipformer 出临时文本，停顿段用 Paraformer 整句纠错，再用标点模型恢复大小写、缩写和标点。中文走 librime，有独立用户词库和连续造词。三个模型都在本机：

- `sherpa-onnx-streaming-zipformer-bilingual-zh-en-2023-02-20-mobile`
- `sherpa-onnx-paraformer-zh-small-2024-03-09`
- `sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`

音频不写磁盘，内存里最多留约 24 秒 PCM。词库来自 rime-frost（GPL-3.0）。当前许可证是 GPL-3.0。

它是「拼音习惯」和「本地语音」放进同一个输入法的最好样本。README 把两项并列列出，没有说语音纠错会写回用户词库。

来源：<https://github.com/feelime/feelime>

### VocoType-linux

Linux 上作为 Fcitx5 全局模块或 IBus 引擎，macOS 上是 InputMethodKit 的 Palette Input Method，和当前键盘输入源共存。最终识别用 FunASR Contextual Paraformer ONNX，可选两遍预览；松键后的最终文字只由完整录音、热词、标点、术语和 ITN 决定。用户词典是 YAML：canonical、aliases、保护词，可用图形界面改，也可以热加载。ITN 把「二零二六年五月十一号」收成 `2026/05/11` 这类写法。另有可选的 OpenAI 兼容接口做润色和受限语音编辑；端点不在本机时，转写和输入框上下文会离开设备。

这是热词和术语保护做得最完整的桌面样本。它不是「越用越记住」，词要用户自己加。许可证为 GPL-3.0。不覆盖 Windows。

来源：<https://github.com/LeonardNJU/VocoType-linux>，许可证文件同仓库 `LICENSE`。

### WowType

fcitx5 拼音输入法。整句、简拼、错键、自定义词都在本地，词库来自雾凇拼音，作者写明拼音引擎没有用微信输入法的代码、词库或模型。习惯学习写在产品说明里：记住选过的词和常连用的词，例如选过一次「我们再讨论」，下次不再出「我们在讨论」；30 天不用会淡掉。`Ctrl+Delete` 可以把学错的词从用户词库删掉。

语音是另一条路径：按住右 Alt 说话，松开上屏。识别用微信输入法的离线模型，约 100MB，从腾讯服务器下载到 `~/.local/share/wowtype/wtlocal/`，不进仓库。推理代码来自 voicekey。模型载入后常驻约 150MB。

拼音侧的习惯衰减值得抄。语音模型本身不能当成可再分发的开源组件，许可和接口都不在这个项目控制范围内。

来源：<https://github.com/Gloridust/WowType>

### fcitx5-vinput 与 LiltKey

两者都是「挂在 Fcitx5 上的语音」，不是另一套拼音引擎。

fcitx5-vinput：点按切换或按住说话；本地 sherpa-onnx，或运行时换成云端；可以用 LLM 做纠错、排版、翻译；选中文字后按住另一个键，用语音改这段字。GPL-3.0。没有用户词频。

LiltKey：按住 Ctrl+Alt。SenseVoice 先给流式草稿，停顿或松键时再识别该段并定稿；校正失败就退回流式结果。录音不上传。自有代码 MIT。v0.5.1 的安装说明目前主要针对 Ubuntu 26.04 amd64。没有用户词库。

来源：<https://github.com/xifan2333/fcitx5-vinput>，<https://github.com/tangzk/liltkey>

### FUTO

Android 上把 Whisper 系小模型做成键盘内置语音，也可以作为别的键盘的识别后端。个人词典从系统个人词典抽出；上下文装不下时只带一个子集。源码可审计，许可证是 FUTO Source First：公开过，但限制商用和去掉付费提醒，不能把它当成可以随便嵌进本项目的开源库。

来源：<https://github.com/futo-org/voice-input>，键盘说明 <https://gitlab.futo.org/keyboard/keyboard-wiki/-/wikis/FUTO-Keyboard>，许可性质见第三方综述 <https://privacygear.nl/en/reviews/futo-keyboard-review/>（许可条款以 FUTO 自己的许可证文本为准）。

## 桌面听写：能上屏，但不是输入法

这些项目解决「按快捷键，文字出现在当前输入框」。它们用模拟按键或剪贴板，不走输入法候选条，也不在用户选词时调频。

| 项目 | 平台 | 本地引擎 | 词库 | 许可证 |
| --- | --- | --- | --- | --- |
| [Handy](https://github.com/cjpais/Handy) | Windows、macOS、Linux | Whisper（Small 到 Large）或 Parakeet V3；Silero VAD 去静音 | 主 README 没有可学习词库。Linux 上屏依赖 xdotool / wtype / dotool | MIT |
| [SayIt](https://github.com/crosswk/SayIt) | Windows、macOS | 七个 GGUF：Parakeet、SenseVoice Small、Fun-ASR Nano、Nemotron 3.5、三种尺寸的 Qwen3-ASR。也可走云或自建服务 | 有热词和应用级规则。迁到 GGML 之后，本地 Qwen3-ASR / SenseVoice / Fun-ASR Nano **不把热词送进识别器**，只在识别后做写法还原 | AGPL-3.0 |
| [Scribe](https://github.com/perrette/scribe) | Linux、macOS、Windows | Vosk 流式、Whisper、Whisper FUTO，或 Groq / OpenAI | 未作为习惯系统描述 | 见仓库 |

SayIt 的配置文档把这件事写得很明确：豆包、千问云端会收到热词；本地 GGML 引擎没有热词通道。热词仍然能在识别完成后把「Say It」换回「SayIt」。这是后处理替换，不是解码期偏置，更不是词频记忆。

Handy 把 Linux 上屏的坑写在 README 里：X11 用 xdotool，Wayland 用 wtype、dotool 或 ydotool；浮层会抢焦点，导致贴错窗口。做全局快捷键听写时，这些约束直接适用。真正的输入法走 Fcitx5 / IBus / TSF 提交字符串，可以避开这条路。

## 词库和习惯：没有语音，但是该抄的引擎

语音识别的「习惯」和拼音输入法的「习惯」不是同一条数据。

拼音输入法在用户**选了一个候选**时学习：这个读音更常对应哪个词，哪两个词常连着出现。Rime 把这套状态放在用户词典里。维护者对导出快照的说明是：每条有提交次数 `c`、记忆强度 `d`、对应时刻 `t`；文件头的 `tick` 是词典最后一次更新的时刻，同步时用来对齐。实现见 `UserDictionary::UpdateEntry`：提交会增加 commits，并用 `formula_d` 更新记忆强度；也可以把条目标成删除。存储是本机 LevelDB，不要把库文件放进网盘。2025 年合并的改动还允许按断句学习新词，而不是把整句当成一个词写进用户词典，且该行为默认关闭。

来源：

- <https://github.com/rime/librime/blob/master/src/rime/dict/user_dictionary.cc>
- <https://github.com/rime/librime/issues/870>
- <https://github.com/rime/librime/issues/1162>
- <https://github.com/rime/librime/pull/1014>

水杉输入法把同一类能力做成可嵌入引擎。`msime-engine` 以 MPL-2.0 发布，负责全拼、双拼、五笔和多语言方案，词格、拼写纠错和学习在引擎内；界面和系统对接留给宿主。官网写明转换和词频学习在本机完成，联网功能可以关。Windows 产品是原生 TSF 前端。组织仓库里的 `msime-cloud` 才包含云候选、AI 联想、翻译和语音识别，那是可选联网能力，不是本地语音输入法。

来源：<https://msime.app/>，<https://msime.app/code/>，<https://github.com/metasequoiaime>

WowType 的「30 天淡忘」是这套机制的产品化版本：学到的词会过期，学错了可以单条忘掉。语音输入法如果只加静态热词、不做衰减，专有名词会越积越脏。

## 可拆开用的功能块

不要从一个「大而全输入法」里整仓复制。按链路拆：

### 1. 采集和端点检测

听写需要知道一句话从哪开始、到哪结束。Handy 用 Silero VAD 去掉静音再送识别。Feelime 把音频留在内存，不写盘，密码框不进剪贴板历史。这两条都该保留：端点检测决定体感，不落盘决定能不能把「本地」说完整。

### 2. 流式草稿加句末定稿

三个项目独立选了同一种结构：

- Feelime：Zipformer 流式草稿，Paraformer 整句纠错。
- LiltKey：SenseVoice 流式草稿，停顿或松键时再识别定稿，失败则退回草稿。
- VocoType：说话时可以预览，松键后的最终结果不采用预览，而走完整离线链路。

预览模型追求首字快，定稿模型追求准。两者不要合成一次解码。

### 3. 本地识别运行时

[sherpa-onnx](https://k2-fsa.github.io/sherpa/onnx/onnx.html) 是目前最适合嵌进输入法的推理层：ONNX Runtime，流式和非流式都有，官方说明识别过程不访问网络。中英流式可参考 Zipformer bilingual；中文整句可参考 Paraformer；标点有现成的 ct-transformer 包，Feelime 已经在用。

热词有硬限制。sherpa 文档写明：热词只在 **transducer** 模型的 `modified_beam_search` 里生效，推理时加分，不用重新训练。其它模型没有这条热词通路。中文热词文件按字或建模单元切开，并设 `hotwords_score`。SenseVoice、普通 Paraformer 不能假设「丢一个 hotwords.txt 就生效」。FunASR 自己的 Contextual Paraformer 是另一套上下文偏置，VocoType 走的是这条，不要和 sherpa 的 ContextGraph 混成一个接口。

来源：<https://github.com/k2-fsa/sherpa/blob/master/docs/source/onnx/hotwords/index.rst>，<https://k2-fsa.github.io/sherpa/onnx/onnx.html>

模型权重另有许可证。LiltKey 单独放了 `MODEL_LICENSES.md`。选定 SenseVoice、Paraformer、Qwen3-ASR 或 Whisper 之前，要读权重自己的许可，不能只看推理代码是 Apache 或 MIT。

### 4. 标点、ITN、术语保护

识别器经常吐出没有标点、数字还是口语的文本。Feelime 在纠错后再跑标点模型。VocoType 把日期、时间、长度、金额做成 ITN，并且有「保护词」：固定写法不被后面的润色改掉。sherpa 的识别配置里也有 `rule_fsts` / `rule_fars`，用来挂 ITN 规则。

这三步应该在上屏前，并且和「习惯学习」分开。ITN 改的是写法，不是用户词频。

### 5. 热词和习惯要分成两本账

| 账本 | 谁在用 | 什么时候写 | 作用 |
| --- | --- | --- | --- |
| 热词 / 别名 | VocoType、SayIt、FUTO、sherpa | 用户手工添加，或从联系人、代码符号表导入 | 解码时偏向「张三」「Kubernetes」这种写法 |
| 用户词频 | librime、WowType、msime-engine | 用户选了候选，或事后把识别结果改成另一个词 | 下次排序更靠前，长期不用就变弱 |

SayIt 已经证明：很多本地 ASR 根本收不到热词。这时第二本账仍然有用，但只能做识别后的替换和排序，效果弱于解码期偏置。输入法如果选 Zipformer 这类 transducer，两本账可以接上；如果选 SenseVoice 定稿，热词多半只能后处理。

语音场景还有一个拼音输入法没有的信号：用户上屏后立刻改字。那一对「识别结果 → 用户改正」比「选了第几个候选」更值钱，现有开源语音项目都没把它做成自动记忆。这是本项目可以比 Feelime / VocoType 多做的一块，不必等声学模型重训。

### 6. 上屏

| 方式 | 参考 | 结果 |
| --- | --- | --- |
| Android `InputConnection` | Feelime | 真正的输入法，能拿上下文，能避开密码框 |
| Fcitx5 模块或插件 | VocoType、fcitx5-vinput、LiltKey、WowType | 与现有拼音共存，commit 走输入法，不模拟按键 |
| IBus 引擎 | VocoType | GNOME 默认框架的对应物 |
| macOS InputMethodKit | VocoType、WowType | 与当前输入源共存 |
| Windows TSF | 水杉 `msime-windows` | Windows 上做真输入法的公开样本 |
| 全局热键 + 模拟按键 / 剪贴板 | Handy、SayIt | 几天能做出可用听写，做不成候选条和输入框上下文 |

Windows 上如果目标是「输入法」，TSF 是系统接口，水杉的 Windows 前端是能读到的开源结构。如果第一版只验证识别和词库，Handy / SayIt 的热键上屏更快，但后面接习惯学习时会缺「当前正在组的字」和「用户选了哪个候选」。

## 不建议依赖的做法

- **把微信、豆包、千问客户端的私有协议或离线包当成引擎。** WowType 使用微信离线包但不分发；一些 macOS 菜单栏工具会逆向这些客户端。模型文件和私有接口可以随时变，也不能随开源仓库再分发。
- **把 FUTO 源码嵌进来。** 能读，许可不允许按普通开源方式组合分发。
- **整仓复制 GPL 输入法再改个名字。** Feelime、VocoType、fcitx5-vinput 是 GPL-3.0。链接或改它们再分发，衍生作品也要按 GPL 提供对应源码。只把它们当架构参考、自己用 sherpa-onnx（Apache-2.0）和 librime（BSD-3-Clause）重写，许可上更干净。msime-engine 是 MPL-2.0，修改引擎文件本身要按 MPL 开源，周边代码可以保持别的许可。
- **假设任何本地模型都支持热词。** sherpa 官方只保证 transducer + `modified_beam_search`。SayIt 的本地 GGML 路径明确不发送热词。

## 建议的参考组合

若要做「本地识别、本地词库、记住习惯」，而不是再做一个快捷键听写：

1. 运行时用 sherpa-onnx。流式草稿用中英 Zipformer，定稿用 Paraformer 或 SenseVoice，标点用 ct-transformer。和 Feelime、LiltKey 的两遍结构一致。
2. 热词只挂在支持 `modified_beam_search` 的那一遍上。定稿模型若不支持，就做识别后的别名替换，并在界面上写明差别。
3. 习惯学习不要从零写公式。拼音选词直接嵌 librime 或 msime-engine。语音侧另记「原文 → 改正」和专有名词，写进同一本用户库，带提交次数和衰减，交互上参考 WowType 的淡忘和单条删除。
4. 上屏在 Linux 做成 Fcitx5 模块，在 Windows 走 TSF。第一版可以用热键验证模型，但别把模拟按键当成最终架构。
5. 音频默认不落盘。润色模型如果要读输入框上下文，默认关闭，并说明文字会发到哪个端点。VocoType 和 SayIt 都是这样做的。

## 还没在本次逐页核对的内容

- Mozc 的用户历史预测器是日文输入法里同类机制的另一个实现，仓库在 <https://github.com/google/mozc>。这次没有逐行核对 `user_history_predictor`，所以正文不把它的字段说死。Rime 的 `c/d/t` 已经够用。
- 各 ASR 权重的再分发条款以各模型卡为准。推理框架允许商用，不表示权重允许再分发。
- FUTO Source First 的逐条限制以 FUTO 许可证原文为准，这里只确认它不是 OSI 开源许可证。
