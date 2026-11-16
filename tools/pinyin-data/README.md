# 拼音数据复现与隔离评测

这套工具把 2026-11-15 词表研究中自写的下载、声调转换、筛选、冻结和 Rime 探针保存在源码中。对应数据已由主任务集成到产品；这里始终生成独立副本并使用全新的测试用户目录，**不会更新生产 `rime-data`、系统注册或用户正在使用的词库，也不会往当前输入框打字**。

详细选型、许可与已知失败项见 [研究报告](../../docs/research/pinyin-selection-2026-11-15.md)。本目录不分发万象程序代码、雾凇 GPL 方案、Lua 或 Grammar 模型。

## 从干净目录生成

需要 Python 3.10+ 及其标准库（本机验证为 Python 3.13.14）。不需要 pip 安装。以下命令在仓库根目录执行；`python` 应指向实际可运行的 Python，而非 Windows 商店占位程序。

```powershell
python -X utf8 tools/pinyin-data/pipeline.py reproduce --source-dir build/pinyin-reproduction/sources --work-dir build/pinyin-reproduction/work --output-dir build/pinyin-reproduction/generated
```

一个命令完成以下步骤：

1. 下载 `source-lock.json` 中固定提交的 11 个源文件，逐个核对长度、SHA-256；旧 `pinyin_simp` 另核对官方 Git blob SHA。
2. 在工作目录安全解压 ESDB；其两个指向仓内 SQL 的链接转换成普通文件，不创建系统符号链接。
3. 使用当前 Python 的 `-X utf8` 自动运行上游 `combine.py create-db scowl.db`；输出留在 `work/esdb-build.log`。
4. 按 `selection.json` 转换带调拼音、去重、选取核心、应用长词门槛，生成英语分级子集和显式术语补充。
5. 放入本项目 schema、完整许可及来源说明，写 `SHA256SUMS`，与源码内 `expected-sha256.txt` 的 **19 份冻结资产**逐字节比对，同时检查词条编码/权重/重复项。
6. 在 `work/generation.json` 记录输入、工具、派生 DB 的实际哈希、Python/SQLite 版本、条目计数和验证结果。另生成 `work/baseline-rime-data`，用于复现旧版本对照。

`work` 和 `output` 必须是尚不存在的目录；工具不删除或覆盖旧工作目录。失败时保留日志，修复后用新的目录名重试。输入缓存可复用；缓存有坏文件会直接失败，不自动接受新哈希。输出根目录可按需指定，但工具拒绝在仓库生产/源码目录下写入，也拒绝输入缓存与工作/输出目录互相包含。

复用已有研究缓存，无需重复下载大文件：

```powershell
python -X utf8 tools/pinyin-data/pipeline.py reproduce --source-dir build/pinyin-reproduction/sources --cache-dir build/pinyin-evaluation-2026-11-15/sources --offline --work-dir build/pinyin-reproduction/work --output-dir build/pinyin-reproduction/generated
```

也可以把首次生成的 `sources` 缓存带到另一台机器。`--offline` 只读取本地已校验来源，缺任何文件就失败。旧研究快照的 `pinyin_simp` 使用 CRLF，缓存导入会先明确还原为官方 LF 再验证；这不是忽略哈希差异。`fetch`、`build` 可以分别运行，参数见 `--help`；不存在额外手工数据库构建或“先运行旧研究脚本”的隐藏步骤。

## 冻结字节、许可与源码边界

`source-lock.json` 固定万象 `7de2f3d0…`、ESDB `1e5b7d3a…`、Rime pinyin-simp `0c6861ef…`。万象数据按 CC BY 4.0；只读取数据，没有复制/运行其未单独授权的原创工具程序。ESDB 的 Copyright、旧词白名单的 Apache-2.0 LICENSE/AUTHORS 和万象的 LICENSE/SCOPE/NOTICE 均随产物输出。

冻结资产原本包含不同换行形式：本项目生成的 YAML/说明、旧库许可和 `SHA256SUMS` 为 CRLF；万象及 ESDB 的上游许可文件保留原始 LF。工具显式恢复这些字节，模板被 Git 转换换行也不会影响生成哈希。生产 `rime-data/**` 应按仓库 `.gitattributes` 的 `-text` 保存冻结字节，**不要批量转换换行或让 Git checkout 改变冻结资产哈希**。

派生 SQLite 数据库的页布局可能变化，因此记录其本次 SHA 而不要求跨运行相同；最终 19 份数据/配置/许可和校验清单必须保持冻结内容。更换词库版本、词项或筛选政策需要另一次明确的研究和冻结，工具不会自动更新锁文件。

```powershell
python -X utf8 tools/pinyin-data/pipeline.py verify --output-dir build/pinyin-reproduction/generated
```

`build` 已自动冻结。`freeze --output-dir ...` 只在 19 项内容先通过原冻结哈希之后重写该隔离目录的校验清单；不能用它给修改过的内容自动背书。

本目录只提交少量模板、政策、语料和自写脚本/探针。大词库、上游源码 ZIP、数据库及二进制留在参数指定的隔离目录，不依赖未提交的旧 `build` 内容。生产集成可额外调整宿主配置；本工具保留原研究 schema，因此不能用研究配置哈希替代当前生产版本的审查。

## 编译并运行真实 Rime 探针

数据生成不需要 C++；候选和个人词缓存评测需要项目已构建的 librime 1.17.0 静态依赖及 Windows MinGW g++。编译命令不内置开发者机器的编译器路径，可传 `g++` 或实际 exe 路径。这里的 MinGW 链接方式只在 Windows 验证。

```powershell
python -X utf8 tools/pinyin-data/pipeline.py compile --cxx g++ --rime-prefix build/rime-prefix --output-dir build/pinyin-reproduction/probes

python -X utf8 tools/pinyin-data/pipeline.py evaluate --probe build/pinyin-reproduction/probes/evaluate.exe --shared-dir build/pinyin-reproduction/generated --corpus tools/pinyin-data/corpora/development.tsv --work-dir build/pinyin-reproduction/eval-development

python -X utf8 tools/pinyin-data/pipeline.py evaluate --probe build/pinyin-reproduction/probes/evaluate.exe --shared-dir build/pinyin-reproduction/generated --corpus tools/pinyin-data/corpora/validation.tsv --work-dir build/pinyin-reproduction/eval-validation

python -X utf8 tools/pinyin-data/pipeline.py evaluate --probe build/pinyin-reproduction/probes/evaluate.exe --shared-dir build/pinyin-reproduction/work/baseline-rime-data --corpus tools/pinyin-data/corpora/validation.tsv --work-dir build/pinyin-reproduction/eval-baseline
```

`compile` 保存编译器版本、参数和实际静态库/探针哈希。每次评测使用新的 `work/user`，输出逐例 `results.json`、分类 `summary.json`、`probe.log` 和输入/可执行文件哈希 `run-info.json`。结果记录首屏前 5，而不是遍历后续所有候选；输入后清组合、不确认学习。底层 C++ 即使绕过 Python 直接运行，也拒绝使用已存在的用户目录。

语料用途应如实区分：

| 文件 | 数量 | 用途 |
|---|---:|---|
| `development.tsv` | 70 | 参与选择的开发诊断集；含原本正常的对照、3 个错拼 + 2 个正常拼写、显式补充术语 |
| `diagnostic.tsv` | 20 | 最初作为保留集，随后参与长词门槛选择，已降级为诊断集；其中 `重新` 的输入误写为 `zhongxin`，保留历史原样 |
| `validation.tsv` | 20 | 当初冻结选型后一次验证的保留集；现在作为固定回归语料，重复跑不等于新的独立保留验证 |

原研究验证集新数据首选 17/20、前 5 为 20/20，旧数据为 15/20、15/20。包含一条首选回归（“再试一次”排到第 5）；不要只报告平均提升，也不要把显式英文/中英补充命中称为陌生术语泛化。完整限制见研究报告。

## 个人词、暂停学习和代际刷新

```powershell
python -X utf8 tools/pinyin-data/pipeline.py personal --probe build/pinyin-reproduction/probes/personal_probe.exe --shared-dir build/pinyin-reproduction/generated --mode normal --work-dir build/pinyin-reproduction/personal-normal

python -X utf8 tools/pinyin-data/pipeline.py personal --probe build/pinyin-reproduction/probes/personal_probe.exe --shared-dir build/pinyin-reproduction/generated --mode private --work-dir build/pinyin-reproduction/personal-private

python -X utf8 tools/pinyin-data/pipeline.py personal --probe build/pinyin-reproduction/probes/personal_probe.exe --shared-dir build/pinyin-reproduction/generated --mode refresh --work-dir build/pinyin-reproduction/personal-refresh

python -X utf8 tools/pinyin-data/pipeline.py personal --probe build/pinyin-reproduction/probes/personal_probe.exe --shared-dir build/pinyin-reproduction/generated --mode generation --work-dir build/pinyin-reproduction/personal-generation
```

这些模式只操作自己新建的 `work/user`：正式/暂停学习导出条目；单文件修改与多 session 缓存；保持配置 handle 到 create/select 完成的代际切换；删除旧测试快照后旧 session 的内存持有；全部 session 释放与 runtime 重启后的重设边界。

预期原研究结果：正式模式 5 条导出记录、tick 2；暂停模式 0 条、tick 0；显式个人词仍可查且原 txt 未改写。`generation` 中新 main/private 都读新代，旧 session 保持旧代，删除旧文件不破坏已经加载的旧 session。全部会话释放/重启之后，没有重新应用代号会回到磁盘 schema 默认名字。生产跨宿主协调、365 天期限、TSF 按键和 UI 仍由产品测试覆盖。

## 本次工具迁移验证

本次使用已验证缓存、全新工作目录和重新构建的 ESDB，生成的 19 份资产 SHA 与旧研究冻结值完全一致。路径包含空格，输入/输出均通过命令行参数提供。编译与评测、拒绝写生产目录、拒绝复用用户目录和校验失败路径的实测结果记录在研究报告的迁移补充说明中。
