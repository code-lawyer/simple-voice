<div align="center">

# Simple Voice · 轻音输入法

**把话说出来，核对后再落笔。**

Windows 本地拼音与中英语音输入法。干净的界面，明确的确认，留在本机的声音。

[公开版本](https://github.com/code-lawyer/simple-voice/releases) · [使用说明](#怎么用) · [源码构建](https://github.com/code-lawyer/simple-voice/blob/main/docs/development.md) · [反馈问题](https://github.com/code-lawyer/simple-voice/issues)

![钛银语音预览浮层](docs/design/skins/titanium-voice.png)

</div>

**v0.2.0-rc.6** 是 **Windows Intel/AMD x64** 发布候选。
[本轮变化](docs/releases/v0.2.0-rc.6.md) · [验证结果与限制](docs/releases/v0.2.0-rc.6-validation.md)。

切换到轻音输入法后，Windows 任务栏会显示「声核」标识，旁边的「中」或「A」表示当前输入模式。点击模式图标可以切换中英文。[任务栏实拍](docs/design/brand/windows-taskbar.png)

## 能做什么

- **拼音与英文**：Rime 全拼、简拼、部分错拼容错；中文词组、英语单词与短语补全。Enter 保留原始输入，空格选择候选。
- **可选本地语音模型**：默认 Qwen3-ASR；设置中可选择 SenseVoiceSmall 或 Whisper Turbo，启用时自动下载并验证。停顿后先预览最终文本，再确认写入。
- **保留表达**：取消无上下文同音替换，不用旧整句覆盖新数字或专名。可编辑预览、恢复识别原文、复制文字；无法识别的占位符需要补全后再确认。
- **三层词库**：公共基础数据、后台拼音偏好、明确添加的“我的词语”。普通语音确认不再自动收集常用词或整句。
- **可管理的设置**：词语搜索、分页、修改、删除与单次撤销；麦克风选择、输入电平、停顿时长、模型准备与重试。
- **试用和备份**：在设置页练习录音、预览和确认；备份个人词语与输入设置，恢复前自动保存旧数据。模型加载失败保留原方案，取消会停止计算，闲置自动释放模型。
- **三套内置皮肤**：钛银、极昼、深空；输入浮层随皮肤切换，设置使用统一中性色和原生控件，高对比度优先。

模型识别仍可能出错。“最终预览”指本次将写入的文本已经确定，不代表内容保证正确。
没有云端润色、自动补写或 R2T2。个人词语目前帮助键盘候选，不承诺改善语音热词识别。

## 安装

1. 从 [Releases](https://github.com/code-lawyer/simple-voice/releases) 下载对应 Windows ZIP，完整解压到准备长期保留的新目录。
2. 安装 [Microsoft Visual C++ x64 运行库](https://aka.ms/vc14/vc_redist.x64.exe)（已安装可跳过）。
3. 双击 `install.cmd`。首次从 sherpa-onnx 官方地址下载运行时和模型并校验 SHA-256，然后请求 Windows 管理员注册。
4. 注销 Windows 再登录，按 `Win + Space` 选择 **轻音输入法**。

首次需要下载较大的模型。建议 16 GB 内存、至少 8 GB 可用磁盘（含下载缓存和解压暂存）；
CPU 即可运行。中英模型在本机集成样本测试中峰值工作集约 2.33 GiB，中文优先约 0.92 GiB，
其他机器和音频可能不同。当前实测环境为 Windows 11 x64；Windows 10 1903+ 为代码目标，尚未做本轮现场验证。

`check.cmd` 验证依赖并分别加载两套语音配置，不开麦、不注册、不访问个人词库。
下载失败可重试，已校验文件会复用。安装后请保留程序目录；UAC 使用同一 Windows 账号。
模型权重不放入应用 ZIP，来源、许可和校验见 [第三方说明](THIRD_PARTY_NOTICES.md)。

## 怎么用

| 操作 | 按键 / 入口 |
| --- | --- |
| 中文输入 | 全拼或简拼，空格选首项，数字 1–5 / 鼠标选候选 |
| 英语输入 | 输入单词或前缀后选英语候选；Enter 原样写入字母 |
| 编辑拼音组合 | 左右方向键、Home、End、Backspace、Delete |
| 暂时遗忘一个学习候选 | 选中该候选后 Ctrl + Delete（不删除基础词表） |
| 中英文切换 | 单独按 Shift |
| 开始语音 | Ctrl + Alt + Space，可修改 |
| 停录并定稿 | 停顿默认 800 毫秒（可设 500–2000），或录音时按 Enter / 再按语音快捷键 |
| 写入最终文本 | 浮层待确认后按 Enter |
| 修改 / 复制 / 恢复原文 | 待确认浮层右键菜单；编辑器 Ctrl + Enter 保存预览 |
| 取消本次语音 | Esc |
| 设置与词语 | 候选条设置入口或托盘菜单 |

录音中的第一次 Enter 只停录。定稿过程中按 Enter 不会预约写入。核对预览后再按 Enter，
正文收到的就是预览文字。写入回执不明时保留预览并提示检查正文，避免自动重复输入。
每次录音最长 60 秒；长段会在较安静处切分定稿，缺少停顿时提示核对分段位置。

“我的词语”输入码只使用小写英文字母。可点击生成，再核对多音字、姓名或自定义缩写。
删除后普通语音不会重新收录；修改会在下一次拼音组合生效，已经开始的组合保持稳定。
英语短语的码可省略空格，如 `machinelearning`；任意中英整句和任意错拼不属于当前能力。

## 三套皮肤

以下为原生窗口截图，三套皮肤共用交互与排版，默认钛银。

| 皮肤 | 语音浮层 | 拼音候选条 |
| --- | --- | --- |
| 钛银 | ![钛银语音框](docs/design/skins/titanium-voice.png) | ![钛银候选条](docs/design/skins/titanium-candidate.png) |
| 极昼 | ![极昼语音框](docs/design/skins/daylight-voice.png) | ![极昼候选条](docs/design/skins/daylight-candidate.png) |
| 深空 | ![深空语音框](docs/design/skins/deep-space-voice.png) | ![深空候选条](docs/design/skins/deep-space-candidate.png) |

[视觉规范](https://github.com/code-lawyer/simple-voice/blob/main/DESIGN.md)。设置和预览编辑器使用 Windows 原生可键盘操作控件。

## 本地数据与词库

日常输入不需要账号或网络，不上传音频、不保存录音文件。确认和编辑前的识别文本仅在内存中。
语音控制管道校验当前登录身份、输入法宿主及前台话轮；这不隔离已进入受信任宿主的同账号
恶意代码。具体范围及升级兼容要求见 [本地 IPC 约定](https://github.com/code-lawyer/simple-voice/blob/main/docs/ipc-security.md)。
为兼容旧版，数据继续保存在 `%LOCALAPPDATA%\VoiceIme\`：

| 文件 / 目录 | 用途 |
| --- | --- |
| `config.json` | 语音快捷键 |
| `appearance.json` | 浮层皮肤 |
| `preferences.ini` | 学习开关、设备、停顿与引导状态；保留旧版模型迁移字段 |
| `speech-models.ini` | 当前语音型号、已有模型目录、闲置释放时间与显式专名提示 |
| `models\` | 从设置页下载的本地模型；安装目录中已有的模型也会复用 |
| `backups\` | 恢复前自动保存的个人词语与输入设置备份 |
| `lexicon.sqlite` | 显式个人词语及兼容保留的旧记录 |
| `rime\` | 后台拼音偏好、编译索引和显式词语的候选快照 |

基础词表由版本维护。Rime 选词偏好在后台管理，可以暂停读取和学习，不需要逐条整理。
“我的词语”只显示自己明确保存的条目，上限 10,000 条，每页最多 100 条。
显式词语及旧语音记录按 **365 天未使用**在启动/使用时清理；旧无日期记录从首次升级计时。
旧自动记录不会参与改写，也不会冒充用户明确添加的术语。Rime 原生学习数据独立管理，
不宣称已经实现同样的 365 天期限。没有新增完整语音历史。

## 更新、回滚、卸载

- **更新**：解压到新目录，运行该目录的 `install.cmd`，注销再登录。个人数据沿用，不覆盖正在加载的 DLL。
- **回滚**：保留旧程序目录和更新前自行备份的个人数据；重新运行旧目录安装并注销登录。旧程序不会读取新版显式词语表，且可能恢复旧版自动学习和同音改写行为。
- **卸载**：从当前注册目录运行 `uninstall.cmd`，注销再登录后删除程序目录。个人数据默认保留。
- **语音不可用**：先运行 `check.cmd`，在设置里刷新/选择麦克风、查看电平并重试模型，再检查 Windows 桌面应用麦克风权限。
- **应用仍显示旧版**：已打开的应用可能保留旧 DLL，注销再登录后统一刷新。

已知限制：很短的英语数字可能漏识别；噪声中文数字有时中文优先配置更好；纯词库整句候选仍有歧义。
本轮真人麦克风、浏览器/Codex 实际编辑、物理跨屏与屏幕阅读器验收后置。
当前未代码签名，不支持 32 位应用、ARM64、macOS 或 Linux。特殊控件、游戏及跨权限输入未完成兼容验证。
提交问题请注明版本、Windows 与应用、步骤；不要上传私密录音或个人词库。

## 开发与许可

[构建和测试](https://github.com/code-lawyer/simple-voice/blob/main/docs/development.md) · [统一产品方案](https://github.com/code-lawyer/simple-voice/blob/main/PRODUCT.md) · [当前发行计划](https://github.com/code-lawyer/simple-voice/blob/main/docs/plans/public-release-sprint.md)

代码使用 [Apache-2.0](LICENSE)。第三方代码、数据和模型分别遵循其原有许可，
详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。感谢 Rime、sherpa-onnx、Qwen、
万象数据、ESDB、ONNX Runtime、cppjieba 等上游项目。
