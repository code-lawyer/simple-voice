# Simple Voice 拼音基础数据与个人词表：本机选择验证

日期：2026-11-15。范围：隔离的 librime 1.17.0 引擎、数据转换和 schema 验证。没有修改生产源码、生产 rime-data、系统注册或真实用户词库；未向当前应用输入文字。本报告供主任务集成，不能替代 TSF 按键、候选窗、安装版及真人验收。

后续集成状态：主任务已把选定词表和 schema 纳入生产 `rime-data`；上句“没有修改”描述的是本研究实验本身的操作范围。可复现工具已迁入源码 [`tools/pinyin-data/`](../../tools/pinyin-data/README.md)，不再依赖本机未提交的旧 `build` 脚本。新工具仍只生成隔离副本、使用新的测试用户目录，生产宿主和安装版验收另行进行。

## 结论与交付

可搬入生产的冻结数据在 `build/pinyin-evaluation-2026-11-15/deliverable/rime-data/`。选择单一中文 script translator，使用经保守筛选的万象数据；英语及少量显式中英词组分别使用 table translator。无需 Lua、Grammar 或额外模型。源文件约 4.37 MB；主要数据为 49,354 字音行、67,163 词音行、91,595 英语编码行和 12 条中英编码行。英语编码行包含同一输出文本的大小写别名，不能当作独立单词数量。

同一 70 例开发诊断语料中，普通全拼前 5 命中从 17/20 到 20/20，简拼从 0/8 到 7/8；原有整句首选 6/7 得以保留，正确句在前 5 为 7/7。最终另外冻结的 20 句保留集只测一次，首选从 15/20 到 17/20、前 5 从 15/20 到 20/20。仍有一个旧版首选正确、现排第 5 的实例，不能声称逐句绝无退步，更不能把这组小样本称为通用准确率。

个人显式术语使用 `stabledb` 可行，暂停自动学习的独立 schema 也已验证。更新词表必须处理进程内数据库缓存和配置对象寿命，详见下文；简单地改 txt、重建一个 session 不完整。

## 数据选择、许可与可追溯性

### 中文：只取万象数据，不集成其工具代码

固定来源为 [amzxyz/rime-wanxiang-data，7de2f3d0](https://github.com/amzxyz/rime-wanxiang-data/tree/7de2f3d0b4677d1e058eecdd48ac412d0314edf9)，只使用 `dicts/zi.dict.yaml`、`jichu.dict.yaml`、`duoyin.dict.yaml`。该提交的 [LICENSE-SCOPE.md](https://github.com/amzxyz/rime-wanxiang-data/blob/7de2f3d0b4677d1e058eecdd48ac412d0314edf9/LICENSE-SCOPE.md) 将原创词库/拼音数据置于 CC BY 4.0，同时明确其原创程序代码没有独立复制、修改、再分发或商业使用许可。因此转换脚本由本项目另写，没有复制或运行万象 Python/Kotlin/PowerShell 工具；没有搬入雾凇 GPL 方案文件或 Lua。

上游 [README](https://github.com/amzxyz/rime-wanxiang-data/blob/7de2f3d0b4677d1e058eecdd48ac412d0314edf9/README.md) 说明字词权重经过处理，并强调词库与语言模型协作。这里保留的是上游数值权重，不把它们称为未经处理的真实使用次数。

转换步骤由 `prepare_data.py`、`curate_core.py` 可重复执行：

1. 带调拼音先把 `ü/ǖ/ǘ/ǚ/ǜ` 统一成 `v`，再 Unicode NFD 去音调；代码转小写，音节以单空格分隔。
2. 相同 `(文本, 无调拼音)` 合并取上游最大权重，避免因不同声调折叠而重复累加。
3. 字表保留可用的正权重字音行。基础词表先按权重保留前 200,000 个 2–8 字词音项，另保留脚本中明列的现代计算机术语目录及已有词的兼容读音。
4. 面向**无上下文模型**的最终核心，对 3 字以上条目再设通用门槛：上游权重至少 5,000，或同样文本/拼音已在旧 `pinyin_simp` 词库出现，或属于明列技术术语目录。保留条目的权重不改、不拼接旧库权重。最终 67,163 行。

这项门槛有明确取舍：减少低权重长片段参与整句分词，也减少长尾词覆盖。初版 20 万词直接用于纯 unigram 整句时，`我们明 + 田间`、`情报 + 存文件` 等组合抢占正常句；这是实测发现，不是词数越多越好的推断。没有把测试整句补进词库。新增 `大语言模型` 等低权重现代词来自预先列出的技术目录，属于显式基础数据补充；不能把这些命中当作未见术语泛化能力。

旧词成员白名单来自 [rime/rime-pinyin-simp，0c6861ef](https://github.com/rime/rime-pinyin-simp/tree/0c6861ef7420ee780270ca6d993d18d4101049d0)，Apache-2.0；只用于保留成熟长词，不把其数值权重与万象混合。固定旧库快照位于 `sources/baseline-rime-data/`，避免生产集成之后复现脚本误读新配置。交付中保留两方 attribution、完整许可、旧库 AUTHORS、来源记录和转换说明。

### 英文：ESDB 常用分级子集，权重与中文隔离

固定来源为 [en-wl/wordlist，1e5b7d3a](https://github.com/en-wl/wordlist/tree/1e5b7d3a72f47a71da5d28686c1dd4b397178485)。该提交 [Copyright](https://github.com/en-wl/wordlist/blob/1e5b7d3a72f47a71da5d28686c1dd4b397178485/Copyright) 允许使用、修改和分发 ESDB 及生成词表，要求保留声明；澳大利亚来源、更大词表和其他数据库用途有附加说明。交付附完整 Copyright，而非只凭主仓许可证推断所有派生数据。

用上游 `combine.py create-db scowl.db` 构建本地数据库，选择 US/A、variant level ≤1、默认类别、非缩写；单词至 size 50，含空格的复合词至 size 60。只保留 3–40 字符 ASCII 拼写、去除所有格；编码去内部空格、连字符和撇号，输出保持原拼写。size class 用于基础排序，不是假称使用频率。80,488 个原始文本/小写码项在补充大小写别名后为 91,595 编码行。

28 个产品/开发术语与英语常用短语由本项目显式列出，如 `Codex`、`GitHub`、`ChatGPT`、`Simple Voice`、`machine learning`。中英混输另列 11 个输出、12 个编码，例如 `AI模型/aimoxing`、`Python脚本/pythonjiaoben`、`3D打印/sanDdayin`。这些是透明的基础补充，不是 ESDB 原条目证明，也不是自动学习或任意中英句子生成能力。具体清单均在 `prepare_data.py`。

没有使用许可说明仅限研究/教育或商用边界不清的常见网络英语词表。没有把中文、英文和手写补充的权重粗暴合并在同一个词频表内。

## 已验证的配置与能力边界

交付的主配置为 `voiceime.schema.yaml`，依赖 `voiceime_english`、`voiceime_mixed` 两个纯 table schema，另含 `voiceime_private`。

| 模块 | 本轮设置 | 作用与限制 |
|---|---|---|
| 中文 | 一个 `script_translator`，initial_quality 1.2 | 保持一套中文权重、一个自动用户词库 |
| 简拼 | 首字母及 zh/ch/sh 首音节缩写 | `nh/rgzn/jsj` 可用，缩写歧义仍靠候选选择 |
| 错拼 | ng→gn、ao→oa、少量 i/u 介音位置交换 | `zhognwen/nihoa` 可用；`xiangum` 未覆盖。没有全开模糊音 |
| 原生纠错 | `enable_correction: false` | 不把未启用的邻键搜索、通用编辑距离或英语纠错当作已有能力 |
| 整句 | `max_sentences:5`、`max_homophones:8`、cutoff 1.0 | 扩展候选路径；仍是无语境模型的词图组合，错误首选可能保留 |
| 英语 | table，quality 1.1、补全开、自动用户词库关 | 单词、合并空格的短语及常见大小写编码；无英语语法理解 |
| 中英混输 | 独立 table，quality 1.3、补全关 | 只支持随数据提供及用户显式添加的词组，不是任意中英组合 |
| 显式个人词 | stabledb，quality 2.0、补全开 | 精确词优先；避免 quality 100 让未完成的个人词压过所有正常词 |

英语 schema 还有首字母大写、全大写派生；数据保留 `ChatGPT`、`GitHub` 这类 canonical 混合大小写别名。`ChatGPT/GitHub` 已通过真实引擎调用验证；随机大小写不能等价承诺。宿主仍需把正确 keycode/Shift 传入引擎，否则数据支持不等于 TSF 可达。

原生表候选对 completion、user table 的加减分可追溯到 [table_translator.cc](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/gear/table_translator.cc#L84-L108)。整句搜索与 Grammar 钩子见 [poet.cc](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/gear/poet.cc)。本轮不是另写一个“智能纠词”系统。

曾考虑双中文 script translator 保留旧整句，但没有采用：相同 user_dict 名会导致两套 Memory 认为语言相同，同时字表的 syllable ID 可能不同，存在重复学习和错码风险。这是根据 [Memory 的语言归属/提交处理](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/gear/memory.cc#L59-L145) 与 [用户字典编码转换](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/dict/user_dictionary.cc#L510-L530) 作出的设计排除，不冒充已运行双翻译器故障实验。

## 评测方法与全部边界

`evaluate.cpp` 静态链接项目现有 librime 1.17.0；直接逐键调用 Rime API，不经过 TSF。每个配置用新的独立用户目录，每条输入后读首屏前 5 个候选，清组合但不提交。没有已学习词频帮助；没有向用户当前窗口注入输入。测试只证明纯引擎候选能力，不能证明生产键盘路由、窗口视觉或实际系统注册。

`corpus.tsv` 的 70 例先于数据选择写好，包括原来已能输入的普通词和句子，不只收集坏例。下表是开发/诊断集，参与过选择门槛，不能称为保留集。

| 类别 | 例数 | 旧首选 / 前5 | 最终首选 / 前5 |
|---|---:|---:|---:|
| 普通完整拼音 | 20 | 16 / 17 | 19 / 20 |
| 中文整句 | 7 | 6 / 6 | 6 / 7 |
| 简拼 | 8 | 0 / 0 | 5 / 7 |
| 错拼及正常拼写对照 | 5 | 2 / 2 | 4 / 4 |
| 英语词/大小写 | 14 | 0 / 0 | 14 / 14 |
| 英语前缀 | 2 | 0 / 0 | 2 / 2 |
| 英语短语 | 4 | 0 / 0 | 4 / 4 |
| 显式中英词组 | 5 | 0 / 0 | 5 / 5 |
| 短码歧义对照 | 5 | 4 / 5 | 4 / 5 |

五个“错拼及对照”中只有 `zhognwen`、`nihoa`、`xiangum` 是错拼；`shurufa`、`jintian` 是正常拼写对照。英语短语和中英组都包含显式补充清单，验证的是编码/候选通道，不是陌生术语泛化。短码歧义部分允许预先写明的多个合理答案。没有据这 70 例计算营销性质的总准确率。

初次 `heldout.tsv` 20 句在 2,000 门槛下出现 `我想和一杯咖啡` 的首选回归。它随后用于把通用门槛选为 5,000，所以**降为诊断集**，没有继续称其为独立保留验证。该文件还有一条数据质量问题：`重新` 被手写为 `zhongxin`；原始输入和结果完整保留，不能把该项缺失当作正确全拼能力下降。

最终 `validation.tsv` 另写 20 句，词表和门槛冻结之后只运行一次：旧首选 15/20、前 5 为 15/20；新首选 17/20、前 5 为 20/20。没有用该组继续调参。新正确首选包括 `请给我一杯热水`、`会议结束以后再决定`、`我已经收到了你的消息`；三个仍非首选的目标是：

| 目标 | 新首选 | 正确项位置 | 与旧版关系 |
|---|---|---:|---|
| 我们周五下午再开会 | 我们周五下午在开会 | 2 | 旧版同样首选“在” |
| 明天早上记得带雨伞 | 明天早上记得待遇三 | 3 | 旧版同样首选错误 |
| 重新启动电脑以后再试一次 | 重新启动电脑以后再是一次 | 5 | 旧首选正确，本次单例回归 |

这组结果支持“基础可用性显著补齐、整体候选覆盖改善”，不支持“静态词表已经解决上下文”。另外 `jttqbc` 没有在前 5 得到目标整句，`xiangum` 没有纠成“项目”；英语/中英能力也只覆盖已收录词和规则。

本机单次性能记录：70 例新配置每例耗时除按键数后的中位数约 65.9 µs/键，旧版 12.4 µs/键；新保留长句约 256.4 µs/键，旧版 39.7 µs/键。新保留集中最慢整个句子逐键查询为 15.58 ms。新干净目录部署约 1.5–1.65 秒，旧版约 0.334 秒。它们是单次、引擎内、按句摊销测量，不是每键 P95、端到端延迟或运行内存。编译后的核心/英文索引约 15 MB；没有做浏览器/Codex UI 性能承诺。

## 显式个人词、暂停学习与缓存刷新

验证配置如下，见交付 schema：

```yaml
personal:
  dictionary: ""
  user_dict: voiceime_personal
  db_class: stabledb
  enable_sentence: false
  enable_completion: true
  initial_quality: 2.0
```

文件位于 Rime `user_data_dir`，格式为 UTF-8 无 BOM，每行 `输出文本<TAB>连续编码<TAB>正整数权重`。探针用权重 100、编码 `qysyx`。宿主导出拼音时应提供连续编码，例如 `qingyinshurufa`，不要依赖在 table translator 中自动移除音节空格；英语与混合大小写别名应显式保留。当前 speller 字母集为 A–Z/a–z，数字键有选词职责，不能不经键盘路由设计就把数字当作个人编码。文本不能含 Tab/换行。`stabledb` 从 txt 读入并只读打开，不会把用户点击产生的词频写回这个 txt。[解析、只读打开与扩展名](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/dict/table_db.cc#L11-L87)

实测结果保存在 `personal-refresh.tsv`：

| 操作 | 读取新文件内容 |
|---|---|
| 改 txt，继续现有 session | 否 |
| 进程只有一个 session，销毁后重建 | 是 |
| 另一个 session 仍持有同名 DB，只重建一个 | 否 |
| 所有持有 session 释放后重建 | 是 |
| finalize 后初始化 | 是 |

原因是同名 user DB 在进程内共享，池使用弱引用；活跃 session 持有内存数据。来源为 [user_dictionary.cc 缓存池](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/dict/user_dictionary.cc#L573-L612)，文件数据载入见 [text_db.cc](https://github.com/rime/librime/blob/33e78140250125871856cdc5b42ddc6a5fcd3cd4/src/rime/dict/text_db.cc)。

### 不打断旧组合的代际快照方式：已实测

写不可变 `voiceime_personal_<内容代号>.txt`，在目标 schema 的 `personal/user_dict` 指向新名字。使用公开 Rime API `schema_open` / `config_set_string`，**保持 RimeConfig handle 存活直到目标 session create/select 完成，再 config_close**。每次创建/切换都应用当前代号；main/private 分别处理。

不能只在进程启动时改一次配置就关闭 handle：若目标 schema 没有活跃 session，修改会随弱缓存释放消失；全部 session 释放或 finalize 后也不能假定仍保留。`personal-generation-v2.tsv` 保留了这个失败顺序，`personal-generation-v3.tsv` 验证修正后的完整边界：

- 旧 session A 保留旧代候选，同时新 main B、新 private C 均读到新代。
- 删除旧 txt 后，已经加载的 A 仍可正常查询旧代内存；已加载对象不依赖保持原文件存在。
- A 在组合结束后重建读到新代。
- 所有 session 释放后，不重新设置会回到磁盘 schema 旧名字；按上面顺序重新应用代号则恢复新代。
- runtime 重启同样需要应用当前代号。

上述实验只涉及一个进程内的 Rime 实例。生产的每个宿主进程仍需检测源 txt/代号变化、在受锁保护的调用中应用新代，并在安全组合边界重建自身 session。删除旧 snapshot 不影响已加载对象，但尚未加载且仍试图用旧代创建的新 session 会缺词；代号读取、快照建立与清理顺序必须由集成层统一。

### 暂停学习：独立 schema，个人术语仍可用

`voiceime_private.schema.yaml` 继承正式配置，只禁用中文 `translator/enable_user_dict`。文件必须保留顶层 `schema/schema_id`、name/version 元信息供部署器识别；仅有 `__include` 加 patch 的最初版本未编译出目标 schema，已在交付中修正。

通过 `personal_probe.cpp` 各用新的 user dir，提交个人词、`你好` 和 `今天天气不错`，再用 levers API 导出自动词库：

| 模式 | 自动词库导出行数 | tick | 显式个人词完整/前缀可查询 | 个人 txt 被改写 |
|---|---:|---:|---|---|
| 正式 | 5 | 2 | 是 | 否 |
| 暂停学习 | 0 | 0 | 是 | 否 |

正式模式的五行包括两个提交项和整句内部词条；它们并非“五个用户添加的新词”。暂停模式中仍出现空的 `voiceime.userdb` 目录，因为 `create_session` 先短暂加载默认正式 schema，再切换 private；因此“目录存在”不能作为发生学习的证据。以导出记录和 tick 检验，而非看到文件就认定泄漏。没有依赖引擎不支持的 `_no_learning` 选项。

本轮未实现或验证 Rime 原生用户库的 365 天保留期；它与产品自己的 SQLite 术语到期策略不能混为一谈。显式术语删改、自动学习暂停/清空、旧版迁移和跨宿主刷新由主任务实现与回归。

## Grammar、Lua 与后续边界

当前构建没有 Octagram 具体组件，仅提供 Grammar 接口。单独加 `grammar:` 或放入 `.gram` 文件不会让本引擎获得上下文能力；[Octagram 组件注册入口](https://github.com/lotem/librime-octagram/blob/57d18b9f58e5284bd891d559f6bdd16cf60341e9/src/grammar_module.cc#L7-L19) 属于独立插件。本轮没有下载/跑大模型，没有把万象上游“带模型”的统计移用到本产品。

合理分层仍是：参考雾凇的模块组织和候选体验；选择许可清晰且可复现的实际词表；将上下文 Grammar 作为独立的后续选项。此次交付已经在无 Lua/Grammar 的现有引擎上运行，但不会硬改 ASR 文本，也不能解决所有整句同音歧义。若以后评估 Grammar，需另行固定插件、模型许可、模型版本与机器实测，重新验证这份核心筛选策略；不能把当前门槛永远视为模型方案最优参数。

## 复现与证据目录

公开的复现入口优先使用源码中的 [`tools/pinyin-data/README.md`](../../tools/pinyin-data/README.md)。例如从仓库根目录运行：

```powershell
python -X utf8 tools/pinyin-data/pipeline.py reproduce --source-dir build/pinyin-reproduction/sources --work-dir build/pinyin-reproduction/work --output-dir build/pinyin-reproduction/generated
```

以下 `build/pinyin-evaluation-2026-11-15/` 文件列表保存的是原始实验的历史证据，不是公开复现的前置依赖：

- `fetch_sources.py`：固定提交的数据获取；`source-manifest.json` 记录下载输入哈希。
- `sources/baseline-rime-data/`：原版数据/配置快照；不会随生产集成改变。
- `prepare_data.py`：自写声调转换、词表筛选、ESDB 子集与显式补充。
- `prepare_schemas.py`：自写纯引擎 schema；`curate_core.py 5000`：最终长词门槛。
- `evaluate.cpp/.exe`、`run_evaluation.py`：直接引擎候选评测；每次必须使用新 label/user dir。
- `corpus.tsv`、`heldout.tsv`、`validation.tsv`：开发集、已转诊断用途的旧保留集、最终冻结保留集。
- `baseline.json`、`core5000.json`、`validation-baseline.json`、`validation-core5000.json`：逐例原始前 5 与时间；`*-summary.json` 有计数和全部缺失项。
- `personal_probe.cpp/.exe`、`personal-*-export/learned-export.tsv`、`personal-generation-v3.tsv`：个人术语、暂停学习、缓存代际证据。
- `deliverable/rime-data/SHA256SUMS`：每份交付文件的 SHA-256；`complete-source-manifest.json`：源数据、旧库、ESDB DB 与实际 librime 静态库哈希；`artifact-manifest.json`：脚本、探针、语料、结果的哈希。

原研究验证环境为 Python 3.13.14，命令使用 `python -X utf8`。ESDB 构建必须开启 UTF-8，默认 Windows 编码无法读部分源词；新工具已自动为其子进程传入 `-X utf8`。上游构建有若干 compounds-auto 未匹配分组跳过提示，命令成功生成数据库；没有通过删除源错误来凑成功。

历史研究脚本顺序为：下载固定来源；在 ESDB 源目录执行 `python -X utf8 combine.py create-db scowl.db`；随后依次执行 `prepare_data.py`、`prepare_schemas.py`、`curate_core.py 5000`、`finalize_assets.py`。这些步骤现已固化为上述源码内 `reproduce` 命令；旧脚本仅保留为实验记录。

当前候选评测使用源码工具的 `compile`、`evaluate` 和 `personal` 子命令，完整命令见 README。历史 C++ 探针以 `-std=c++17 -O2 -static -DRIME_STATIC` 编译，include 来自所选 Rime 依赖前缀，全量链接 `librime.a`，再链接 yaml-cpp、leveldb、opencc、marisa 及 bcrypt/ws2_32/wsock32/winpthread。新工具以参数接收编译器和依赖前缀，并记录实际库哈希。

生产搬入后应重新做实际宿主回归，特别是 Shift/大小写、数字选词、翻页、退格、Enter 与语音状态协调，和多 session/多进程个人词代际切换。本报告不替主任务宣称这些项目已通过。

## 源码内复现工具的迁移验证

维护入口现为 [`tools/pinyin-data/README.md`](../../tools/pinyin-data/README.md) 和 `pipeline.py`。命令提供独立的 `--source-dir`、`--work-dir`、`--output-dir`、编译器/依赖前缀以及评测共享数据/语料参数；没有写死开发者机器路径，也不读取生产 `rime-data` 来重新生成旧对照。下载、UTF-8 ESDB 数据库构建、声调转换、两阶段筛选、许可归档、冻结校验均由 `reproduce` 自动串联；原先手工执行 `combine.py`、复制旧配置的步骤已固化。

源码保存固定的 `source-lock.json`、`selection.json`、原研究 schema/旧对照模板、三份语料、C++ 引擎/个人词探针及原冻结 `expected-sha256.txt`。大词表、上游 ZIP、SQLite 数据库、探针二进制仍留在用户指定的工作目录。原 `build/pinyin-evaluation-2026-11-15/` 是历史实验记录，不再是复现所必需的唯一来源；它可以作为经过 SHA 验证的离线下载缓存使用。

迁移时核实了两个容易漏掉的来源边界：

- 本机旧 `pinyin_simp` 快照已被转换为 CRLF，不能把其 SHA-256 当作官方 raw 文件的 SHA。已按固定 GitHub 提交核对官方 blob SHA，锁文件固定原始 LF 字节；离线导入旧快照时明确转换 CRLF→LF 后再严格验证。
- ESDB 固定 ZIP 内有两个 PostgreSQL SQL 文件符号链接。工具验证目标仍在同一归档中，并将其物化为普通文件，无需系统符号链接权限；没有运行万象工具程序。

在 `build/pinyin-toolcheck-2026-11-15/` 用新的参数目录（含空格）做了实际完整重建：离线复用校验后的上游源，重新解压并从头生成 ESDB。最终 **19 份资产的 SHA-256 全部一致，连同原 CRLF 的 SHA256SUMS 共 20 个文件与旧交付逐字节一致**。生成器显式保留既有 CRLF/LF 格式；生产冻结资产用 Git `-text` 保存，而不是靠当前机器的 autocrlf 恰好得到相同换行。

两次新建 SQLite 中间文件的布局/哈希不同，派生词表和所有冻结资产完全一致。工具因此记录每次 DB 的真实哈希，而不把 SQLite 页布局误当作必须稳定的最终产物；完整来源、工具与版本记录位于各次 `work/generation.json`。

迁移后的新编译探针重新运行开发集 70 例、新数据验证集 20 例、旧数据对照 20 例，**110 条逐例前 5 候选与 preedit 均与原研究对应结果一致**，不是只比较聚合数字。正式/暂停学习、同名 stabledb 多 session 缓存、main/private 新代切换及删除旧快照后的内存持有也重新通过。

此外验证了实际 CLI/裸探针的拒绝路径：禁止向生产目录写入、禁止复用已有工作目录、拒绝已损坏的固定来源、拒绝不匹配的生成资产、两种 C++ 探针均拒绝已存在的用户目录、畸形语料在创建用户目录前失败。已有用户目录中的测试哨兵保持原样。总计 14 项迁移检查通过，记录在 `build/pinyin-toolcheck-2026-11-15/verification.json`；最后完整生成记录位于 `final work/generation.json`，产物为 `final generated/`。

这次是研究工具的可复现性回归，不能当作再次取得了一组新的统计保留样本，也不替代已经集成的生产宿主代码回归。
