---
name: Simple Voice
description: 原生 Windows 输入浮层的三套内置皮肤，统一文字与状态层级。
colors:
  titanium-background: "#f0f3f5"
  titanium-text: "#22333d"
  titanium-secondary: "#536976"
  titanium-selected: "#d4e2e8"
  titanium-selected-text: "#164e60"
  titanium-hover: "#e3eaef"
  titanium-border: "#b8c6cf"
  titanium-accent: "#166879"
  titanium-rule: "#cdd7de"
  daylight-background: "#ffffff"
  daylight-text: "#202e3c"
  daylight-secondary: "#5c6a78"
  daylight-selected: "#e5edfe"
  daylight-selected-text: "#234e9b"
  daylight-hover: "#edf2fe"
  daylight-rule: "#e0e6ed"
  daylight-accent: "#2a62ce"
  deep-space-background: "#101b23"
  deep-space-text: "#edf4f7"
  deep-space-secondary: "#9bafb9"
  deep-space-selected: "#263d49"
  deep-space-selected-text: "#c4eef3"
  deep-space-hover: "#20313c"
  deep-space-border: "#3a4f5b"
  deep-space-rule: "#293c47"
  deep-space-accent: "#92d0db"
typography:
  body:
    fontFamily: "Microsoft YaHei UI"
    fontSize: "16px"
    fontWeight: 400
  status:
    fontFamily: "Microsoft YaHei UI"
    fontSize: "13px"
    fontWeight: 400
  label:
    fontFamily: "Microsoft YaHei UI"
    fontSize: "12px"
    fontWeight: 400
rounded:
  titanium-surface: "10px"
  daylight-surface: "14px"
  deep-space-surface: "12px"
  candidate: "6px"
  daylight-candidate: "9px"
  daylight-status-badge: "7px"
spacing:
  surface-inset: "16px"
  candidate-inset: "6px"
  candidate-text-leading: "8px"
  candidate-text-trailing: "5px"
components:
  voice-titanium:
    backgroundColor: "{colors.titanium-background}"
    textColor: "{colors.titanium-text}"
    typography: "{typography.body}"
    rounded: "{rounded.titanium-surface}"
    width: "380px"
  voice-daylight:
    backgroundColor: "{colors.daylight-background}"
    textColor: "{colors.daylight-text}"
    typography: "{typography.body}"
    rounded: "{rounded.daylight-surface}"
    width: "380px"
  voice-deep-space:
    backgroundColor: "{colors.deep-space-background}"
    textColor: "{colors.deep-space-text}"
    typography: "{typography.body}"
    rounded: "{rounded.deep-space-surface}"
    width: "380px"
  candidate-titanium:
    backgroundColor: "{colors.titanium-background}"
    textColor: "{colors.titanium-text}"
    typography: "{typography.body}"
    rounded: "{rounded.titanium-surface}"
    height: "40px"
  candidate-daylight:
    backgroundColor: "{colors.daylight-background}"
    textColor: "{colors.daylight-text}"
    typography: "{typography.body}"
    rounded: "{rounded.daylight-surface}"
    height: "40px"
  candidate-deep-space:
    backgroundColor: "{colors.deep-space-background}"
    textColor: "{colors.deep-space-text}"
    typography: "{typography.body}"
    rounded: "{rounded.deep-space-surface}"
    height: "40px"
  candidate-titanium-selected:
    backgroundColor: "{colors.titanium-selected}"
    textColor: "{colors.titanium-selected-text}"
    typography: "{typography.body}"
    rounded: "{rounded.candidate}"
    height: "28px"
  candidate-titanium-hover:
    backgroundColor: "{colors.titanium-hover}"
    textColor: "{colors.titanium-text}"
    typography: "{typography.body}"
    rounded: "{rounded.candidate}"
    height: "28px"
  candidate-daylight-selected:
    backgroundColor: "{colors.daylight-selected}"
    textColor: "{colors.daylight-selected-text}"
    typography: "{typography.body}"
    rounded: "{rounded.daylight-candidate}"
    height: "28px"
  candidate-daylight-hover:
    backgroundColor: "{colors.daylight-hover}"
    textColor: "{colors.daylight-text}"
    typography: "{typography.body}"
    rounded: "{rounded.daylight-candidate}"
    height: "28px"
  candidate-deep-space-selected:
    backgroundColor: "{colors.deep-space-selected}"
    textColor: "{colors.deep-space-selected-text}"
    typography: "{typography.body}"
    rounded: "{rounded.candidate}"
    height: "28px"
  candidate-deep-space-hover:
    backgroundColor: "{colors.deep-space-hover}"
    textColor: "{colors.deep-space-text}"
    typography: "{typography.body}"
    rounded: "{rounded.candidate}"
    height: "28px"
---

# Design System: Simple Voice

## Overview

**Creative North Star: "精密输入仪表"**

Simple Voice 的原生 Windows 浮层以可读文字和明确状态形成干净、克制的科技感。语音预览与拼音候选使用同一套字体、色彩职责和边界细节，保持输入内容在视觉上的主导地位。科技感来自精确的分区、细线与低饱和强调色。

用户已确认钛银、极昼、深空三套皮肤全部内置并可切换。默认钛银的冷银灰与深青呈现仪表分节；极昼用净白、蓝色和连续圆角表达轻盈；深空用石墨、冰青和细轮廓适应暗色编辑环境。三套共用布局与行为，Windows 高对比度颜色始终优先。

**Key Characteristics:**

- 文字为主，状态与操作提示分层清楚。
- 同一输入浮层结构承载三套完整皮肤。
- 实色表面、细线图标与适量圆角。
- 浮层不激活，换肤保留正在进行的输入。

本规范提取自 `src/ui_theme.hpp`、`src/confirm_box.cpp`、`src/candidate_bar.cpp` 和 `src/engine_main.cpp`，记录已实现的原生组件。前置 YAML 为规范 token；其中可移植的 `px` 长度表示 Windows DIP（96 DPI 基准逻辑像素），原生实现通过 `UiPx` 按窗口 DPI 换算。`docs/design/skin-preview/index.html` 是浏览器比较原型，其列式排版与展示尺寸不规定原生应用布局。

## Colors

三套配色都以中性色承载正文，以低饱和冷色标记状态和选择；每套独立成组，语音框与候选条共同使用选中的一组。

### Primary

- **钛银深青**（`titanium-accent`）：正常语音状态图标、方形状态点和选中候选的细下沿。
- **极昼冷蓝**（`daylight-accent`）：正常语音状态图标和圆形状态点；浅蓝图标底复用 `daylight-hover`。
- **深空冰青**（`deep-space-accent`）：在石墨表面上标记正常语音状态，保持低饱和的明亮焦点。

### Neutral

| 职责 | 应用方式 |
| --- | --- |
| `*-background` / `*-text` | 浮层实色背景与正文、候选词。 |
| `*-secondary` | 状态文字、操作提示、未选中的候选序号、设置入口和滚动条；异常与待核实状态的图标也使用此色。 |
| `*-selected` / `*-selected-text` | 选中候选背景与文字；序号和正文共同使用选中前景。 |
| `*-hover` | 未选中候选及设置入口的鼠标悬停底色；普通模式保留正文或次要文字原本的前景职责。 |
| `*-border` | 钛银与深空的细外轮廓。极昼不绘制常规外描边。 |
| `*-rule` | 语音分隔及候选设置入口的竖线；极昼仅保留设置入口分隔。 |

**The Shared Roles Rule.** 换肤改变一整组色彩角色，语音与候选同步使用；状态意义、文字层级和操作位置不随皮肤变化。

**The System Contrast Rule.** Windows 高对比度开启时，背景取 `COLOR_WINDOW`，正文与次要文字取 `COLOR_WINDOWTEXT`，选中与 hover 底色取 `COLOR_HIGHLIGHT`，其上的候选正文、序号和设置文字取 `COLOR_HIGHLIGHTTEXT`。系统配色覆盖皮肤配色，并恢复可见外轮廓。

普通小字以实际使用的背景检查对比度，包括 hover。极昼次要文字的已复核结果为白底 5.54:1、hover 底 4.95:1；不能以降低文字对比度取得轻盈感。高对比度的系统颜色是运行时值，不固定为第三套或第四套 hex 调色板。

## Typography

**Body Font:** Microsoft YaHei UI。原生绘制统一调用 `UiFont`，使用正常字重与 ClearType；字体不可用时由 Windows 字体映射处理，没有另设网页字体栈。

**Character:** 中文字形清楚、紧凑，正文与候选词处于同一视觉层级。此输入浮层没有展示型大标题，也不依赖字重变化强调状态。

### Hierarchy

| token | 用途 |
| --- | --- |
| `body` | 语音正文、空内容引导、候选词。 |
| `status` | 语音浮层顶部的录音、校准、待确认、写入及异常状态。 |
| `label` | 操作提示、长文滚动提示、候选序号和设置入口。 |

行高取 GDI 实际字体度量，语音正文采用自然换行；不指定网页式行高或字距。候选词单行垂直居中，超出可用空间时显示省略号并保留完整内容的工具提示。

**The Readable Small Text Rule.** 操作提示与候选序号使用同一小字层级；空间不足时先换行或限制容器，而不是继续缩小文字。

## Layout

布局单位为 DIP，随所在窗口 DPI 缩放。两个浮层都跟随插入点，使用当前显示器工作区约束位置，并保持宿主编辑器焦点；没有网页断点或移动端列布局。

- **语音浮层：** 宽度取 `voice-*` token，工作区更窄时收缩到工作区宽度；高度随正文和提示实测，最多 320 DIP 且不超过工作区高度。左右正文留白取 `surface-inset`，正文起点为顶部 44 DIP；状态在顶部，操作提示在底部。
- **长文本：** 正文视口按完整字体行限制高度，底部另留滚动说明空间。右侧滚动条保持在正文区；鼠标滚轮、拖动滑块及点击轨道可查看全文，末尾显示“已到末尾”。
- **候选条：** 单行最多五项，高度取 `candidate-*` token。候选区上下与起始留白取 `candidate-inset`，宽度随文字测量，整条最大为 960 DIP 且受工作区约束；右侧保留 52 DIP 设置区域。候选序号和词之间有固定测量区，词内容按剩余宽度省略。
- **定位留白：** 语音与插入点的首选间隔为 6 DIP，候选为 5 DIP；下方空间不足时尝试放在插入点上方，最终限制在工作区内。无法取得有效插入点时，先按宿主客户区右下角内缩 16 DIP 定位，再进行工作区约束。

**The Stable Input Rule.** 相同内容和锚点不重复移动、显隐或重绘；换肤只更新圆角与绘制，不重新定位浮层、不重启录音、不清空预览、不改变候选选择或滚动位置，也不让隐藏浮层重新出现。

## Elevation & Depth

深度由实色表面、候选色块、细分隔及 Windows 原生窗口阴影共同表达。两个窗口类均请求 `CS_DROPSHADOW`，实际阴影由 Windows 决定，没有项目自定义的模糊半径、偏移或透明度 token。钛银与深空用细外轮廓稳定边界；极昼省略外描边和语音内分隔，以净白连续表面与系统抬升形成层次。

绘制采用完整离屏帧提交，避免只出现背景的闪烁。录音、校准和待确认状态通过文字与线条图标立即更新；当前原生组件没有装饰性循环动画、渐变过渡、透明玻璃或背景模糊。

## Shapes

外轮廓使用各皮肤的 `*-surface` 圆角，并应用于语音与候选窗口的实际区域裁切。钛银偏紧凑，极昼更柔和，深空居中。选中与 hover 候选使用 `candidate` 圆角，极昼单独使用 `daylight-candidate`；这是局部高亮形状，不改变候选条尺寸。

图标由 GDI 细线绘制：麦克风、校准滑杆及待确认勾号各自承担状态含义。钛银采用方形状态点和选中项细下沿；极昼与深空采用圆形状态点，极昼额外使用浅蓝圆角图标底。外描边、分隔与图标线宽均以 1 DIP 为基准，并至少绘制一个物理像素。

## Components

### Voice preview

**性格：** 清楚、安静的输入确认仪表。状态、正文、操作提示保持固定层级，换肤不改变语音流程。

| 状态 | 视觉与提示 |
| --- | --- |
| 正在听 | 麦克风线图标、临时识别正文；提示停顿自动校准、Enter 停录预览、Esc 取消。 |
| 正在校准文字 | 校准线图标与当前文字；提示校准后预览并确认。 |
| 待确认 | 勾号、最终预览文字；提示 Enter 写入正文、Esc 取消。 |
| 正在写入正文 | 校准线图标；提示等待应用确认。 |
| 语音暂不可用 | 明确状态文字与输入设备、权限重试提示，图标采用次要前景。 |
| 写入结果待核实 | 提醒先核对正文，确认未写入后再重试，避免把未知结果画成成功。 |
| 录音已结束且无正文 | 空内容说明和重新开始提示。 |

录音中的第一次 Enter 触发停录与校准，最终文字保留在待确认浮层；校准中 Enter 不预约自动提交。异常含义通过文字表达，不新增独立告警颜色。正文预览不是可编辑文本框。

### Candidate strip

**性格：** 紧凑的单行选词工具。当前选中项以完整底色和对应文字颜色表达；钛银另加细下沿。鼠标 hover 与键盘选择可同时存在，选中项优先使用选中样式。高对比模式的 hover 前景遵守系统高亮规则。

候选支持现有键盘选词与鼠标选择；无障碍对象提供完整候选名、选中状态及“选择并写入”动作。视觉省略不裁掉可访问名称。浮层收到鼠标操作时不激活，也不接管编辑器焦点。

### Settings and skin menu

**性格：** 安静、可识别的原生入口。候选条右侧“设置”采用次要文字、细竖线分隔和 hover 底色。托盘与此入口打开同一 Windows 菜单，“皮肤”子菜单列出钛银、极昼、深空，并用系统单选标记当前选择。菜单、快捷键对话框及其按钮继续遵守 Windows 原生控件外观，不套用浮层的自绘按钮样式。

选择成功后写入 `%LOCALAPPDATA%\VoiceIme\appearance.json`，通知已存在的新版浮层同步刷新；后续新窗口读取该选择。缺失或无效偏好回退钛银。保存失败时保留原皮肤并提示重试。皮肤偏好独立于快捷键 `config.json`，不增设自动跟随系统深浅色或其他未批准开关。

### Native evidence

`docs/design/skins/` 中的 `titanium-voice.png`、`titanium-candidate.png`、`daylight-voice.png`、`daylight-candidate.png`、`deep-space-voice.png`、`deep-space-candidate.png` 是本项目 UI 测试工具对实际原生窗口的截图，均非生成图或第三方素材。状态、长文、DPI 和 hover 的完整证据在 `build/skins-2026-11-15/`；最终审查见该目录的 `native-skin-review-verdict.md`。

截图和注入的高对比度分支验证用于核对原生外观；真人麦克风、用户应用现场、物理多显示器与屏幕阅读器验收仍按产品既定边界单独进行。浏览器比较原型只辅助理解三套方向。

## Do's and Don'ts

### Do:

- **Do** 同步使用同一皮肤的语音与候选角色，保留共享的字号、布局和状态语义。
- **Do** 以原生 DIP 与实际字体测量布局，检查正常、选中、hover、长文和高对比状态。
- **Do** 让 Windows 高对比度颜色覆盖预设皮肤，并保持系统高亮底上的对应文字颜色。
- **Do** 换肤时保留输入、预览、选词、滚动位置和窗口可见状态。
- **Do** 以真实原生截图核对实现，保留截图来源与测试范围。

### Don't:

- **Don't** 将浏览器比较板的宽度、三列排版或按钮设计当作原生输入法规范。
- **Don't** 用更浅的小字、大面积霓虹、装饰动效或透明模糊表达未来感。
- **Don't** 为换肤添加新的确认步骤、重启要求或未经批准的自动深浅切换。
- **Don't** 把高对比度测试分支或截图审查写成真人使用验收结果。
