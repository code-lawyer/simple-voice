# 轻音输入法与四个开源项目的对照

审计版本为 `v0.2.0-rc.12`（`115cbf9`）。轻音通过 Windows TSF 提供中文拼音，也能在用户核对后把本地语音定稿写入当前应用。以下选择三个支持 Windows 本地听写的项目，以及一个同属 Rime 生态的 Windows 中文输入法。对照依据为官方仓库、发行说明、配置与使用文档。本次未安装并运行这四款产品，识别率和延迟还需同机同样本对测。[轻音产品约定](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/PRODUCT.md)

## 社区关注度与可比范围

2026 年 11 月 22 日 19:11（洛杉矶时间）逐一查询 GitHub REST API 的 `stargazers_count` 和 `/releases/latest`。表中日期同用洛杉矶时间，版本是当时 API 返回的最新正式 Release。星标反映仓库累计关注度，统计范围涵盖各仓库支持的全部平台和功能。

| 项目 | 星标 | 最近正式发行 | 能与轻音直接对照的体验 |
| --- | ---: | --- | --- |
| Handy | 39,749 | v0.10.1 · 2026-11-14 | Windows 本地听写，可选按住说话或按键启停，转写后粘贴。新版分别提示无模型、麦克风只有静音和 Windows 高权限窗口，并修复把自定义词原样误转写的问题。[项目介绍](https://handy.computer/) · [发行说明](https://github.com/cjpais/Handy/releases/tag/v0.10.1) · [星标](https://api.github.com/repos/cjpais/Handy) |
| OpenWhispr | 12,045 | v1.12.0 · 2026-11-17 | Windows 可选本地或云端转写。官方按模型列下载大小，并单独说明普通编辑框和终端的文本投递与手工恢复。这里比较本地听写部分。[本地模型](https://docs.openwhispr.com/guides/local-models) · [应用内输入](https://docs.openwhispr.com/platform/dictating-into-other-apps) · [发行说明](https://github.com/OpenWhispr/openwhispr/releases/tag/v1.12.0) · [星标](https://api.github.com/repos/OpenWhispr/openwhispr) |
| Rime 小狼毫 | 8,331 | 0.17.4 · 2025-06-03 | Windows 中文输入法，提供拼音方案、用户词库及设置入口，官方配置分别控制候选字体、标签和布局方向。可直接比较长词显示、候选选择和如何找到设置。该库最近一次推送为 2026-10-31。[使用说明](https://github.com/rime/weasel/blob/0.17.4/README.md) · [发行配置](https://github.com/rime/weasel/blob/0.17.4/output/data/weasel.yaml) · [发行说明](https://github.com/rime/weasel/releases/tag/0.17.4) · [星标及推送时间](https://api.github.com/repos/rime/weasel) |
| CapsWriter-Offline | 7,145 | v2.6 · 2026-05-30 | 面向 Windows 中文听写，按住 CapsLock 或鼠标侧键讲话、松开上屏。提供热词与数字处理，对不同硬件给出模型选择建议。其模糊热词强制替换与轻音已确认的原文保护约定不同。[使用说明](https://github.com/HaujetZhao/CapsWriter-Offline/blob/v2.6/readme.md) · [发行说明](https://github.com/HaujetZhao/CapsWriter-Offline/releases/tag/v2.6) · [星标](https://api.github.com/repos/HaujetZhao/CapsWriter-Offline) |

## 对轻音的改进建议

| 优先级 | 本项目差距和影响 | 可实施的改动与验收 |
| --- | --- | --- |
| 高 · 首次安装与模型选择 | `setup.ps1` 先下载模型，再检查 VC++ 运行库。缺运行库的用户可能等到大文件拉完才知道装不了。设置已有自动下载、校验、本地目录和取消，启用前主要提示“将自动下载”，缺少作决定所需的体积信息。 | 将运行库检查前移。启用前显示本次尚需下载的大小、临时磁盘需求和模型适用情形，缓存命中时扣除已有文件。用无运行库、磁盘不足、已有缓存和正常安装四种情形验收。Handy 与 OpenWhispr 的模型说明展示了用大小和用途解释选择的做法。[轻音安装顺序](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/tools/setup.ps1#L30-L43) · [当前模型状态](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/src/engine_main.cpp#L1099-L1113) · [Handy 模型说明](https://github.com/cjpais/Handy/blob/v0.10.1/README.md#manual-model-installation-for-proxy-users-or-network-restrictions) · [OpenWhispr 模型说明](https://docs.openwhispr.com/guides/local-models) |
| 高 · 真实编辑器写入与恢复 | 轻音已有 TSF 写入回执、待核对预览和手动复制。隔离宿主的通过记录没有覆盖浏览器、Codex 与物理跨屏的现场表现。用户最关心的是这句话最终到了哪里、能否取回。 | 用同一句含数字和专名的文本，分别验证浏览器输入框、网页富文本、Codex 编辑器及一个普通 Win32 编辑框。登记系统、宿主版本、切换焦点、是否恰好写入一次、失败时预览与复制能否使用。对已知受限宿主给出就地操作提示，优先保证长文本可以手工取回。OpenWhispr 分应用说明失败恢复，Handy 为高权限目标给单独提示。[轻音回执和确认](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/README.md#怎么用) · [轻音验收范围](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/README.md#更新回滚卸载) · [OpenWhispr 应用差异](https://docs.openwhispr.com/platform/editors-and-terminals) · [Handy Windows 行为](https://github.com/cjpais/Handy/blob/v0.10.1/README.md#administrator-apps-windows) |
| 中 · 长候选可辨性 | 小狼毫将候选布局与字号作为独立设置。轻音在单行显示五项，并通过工具提示与无障碍名称提供完整词语。计算每项宽度时，靠前的长词先被平均宽度截断，后面短词留下的空间无法再分给它。首两项共享前缀时，区分它们的尾字容易消失。 | 首先测量所有候选的自然宽度，仅在合计超出工作区预算时分配省略。用现有“今天我们一起讨论接下来的……”夹具同时核对普通与高 DPI 下的词尾、数字键顺序、鼠标命中区域。可在现有三套皮肤内实施。[轻音分宽计算](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/src/candidate_bar.cpp#L198-L225) · [长词夹具](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/tools/ui_preview.cpp#L207-L215) · [小狼毫候选配置](https://github.com/rime/weasel/blob/0.17.4/output/data/weasel.yaml#L22-L44) |
| 中 · 数字和专名质量 | 轻音的 rc.12 报告记录，120 段公开真人英语单数字中 61 段含义完全正确、14 段为空、45 段输出错误。三个普通话合成音色读三个专名，填写提示后 5/9 命中。CapsWriter 展示了中文用户对数字和专名的需求，Handy 的发行记录也说明提示词可能被原样误输出。 | 在原回放集增加相近专名、无关词、静音，以及包含小数点、连续编号和中英混说的片段。对同一音频做启用/关闭专名提示对照，分别记录正确、空白、误认、误插入提示词，以及停录到预览的 P50/P95 耗时。所有结果仍由用户确认，避免把错误映射固化进词库。上述数字引用项目既有实测。[轻音基线](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/docs/releases/v0.2.0-rc.12-validation.md) · [CapsWriter 数字与热词](https://github.com/HaujetZhao/CapsWriter-Offline/blob/v2.6/readme.md) · [Handy 提示词修复](https://github.com/cjpais/Handy/releases/tag/v0.10.1) |

对长词另做了一个本机离屏测量。用 Windows `TextRenderer`、16 像素 Microsoft YaHei UI 和现有夹具的五个词，按源码在 96 DPI、960 像素上限下计算，候选可用宽度为 896 像素。五项自然宽度合计为 832 像素，可完整放下。现算法给首两项各 179 像素，分别低于其 288 和 256 像素需求，最后有 250 像素预算闲置。这组数来自文字测量和实际分宽公式。[字体与度量](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/src/ui_theme.hpp#L32-L36) · [分宽算法](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/src/candidate_bar.cpp#L198-L220)

轻音值得保留的组合是拼音与语音共用一个输入法入口、语音可改可复制、写入必须由用户确认。把同类项目的资源提示、宿主验证和词例测试移植到现有流程，能直接改善这三项体验。自动粘贴、云端润色、强制同音替换和保存整段历史均不适配当前产品约定。[轻音产品约定](https://github.com/code-lawyer/simple-voice/blob/115cbf9fd7e9576f9fc452ed7b949122eb897c9e/PRODUCT.md#confirmed-workflow)
