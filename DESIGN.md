---
name: Simple Voice
description: 原生 Windows 输入浮层的三套内置皮肤与声核单色品牌，统一文字、身份与状态层级。
colors:
  brand-ink: "#000000"
  brand-paper: "#ffffff"
  brand-badge-outline: "#00000080"
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
  preferences-title:
    fontFamily: "Microsoft YaHei UI"
    fontSize: "23px"
    fontWeight: 600
  preferences-control:
    fontFamily: "Microsoft YaHei UI"
    fontSize: "14px"
    fontWeight: 400
  preview-editor:
    fontFamily: "Microsoft YaHei UI"
    fontSize: "15px"
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
  brand-profile-badge:
    backgroundColor: "{colors.brand-paper}"
    textColor: "{colors.brand-ink}"
  brand-mark-on-dark:
    textColor: "{colors.brand-paper}"
  brand-mark-on-light:
    textColor: "{colors.brand-ink}"
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
  preferences:
    backgroundColor: "{colors.titanium-background}"
    textColor: "{colors.titanium-text}"
    typography: "{typography.preferences-control}"
    width: "680px"
    height: "690px"
  preview-editor:
    typography: "{typography.preview-editor}"
    width: "640px"
    height: "400px"
---

# Design System: Simple Voice

## Overview

**Creative North Star: "精密输入仪表"**

Simple Voice 的原生 Windows 浮层以可读文字和明确状态形成干净、克制的科技感。语音预览与拼音候选使用同一套字体、色彩职责和边界细节，保持输入内容在视觉上的主导地位。科技感来自精确的分区、细线与低饱和强调色。

用户已确认钛银、极昼、深空三套皮肤全部内置并可切换。默认钛银的冷银灰与深青呈现仪表分节；极昼用净白、蓝色和连续圆角表达轻盈；深空用石墨、冰青和细轮廓适应暗色编辑环境。三套共用布局与行为，Windows 高对比度颜色始终优先。

原生设置与语音预览编辑器延续同一中文字体和明确状态层级。设置使用固定钛银中性背景与正文色，选择的皮肤用于输入浮层；表单控件、预览编辑器和窗口框沿用 Windows 原生外观。它们是可激活的工作窗口，允许用户配置和编辑，不继承浮层的禁止激活行为。

用户于 2026-11-15 选定 A「声核」作为 Simple Voice / 轻音输入法的品牌标记。极简、本地的定位落在单色开口圆框与中央胶囊上：圆框表达本机空间，胶囊表达声音。它补充产品身份，不改变三套皮肤、文字层级或状态图标；品牌保持静态，与独立的「中 / A」输入模式分开。

**Key Characteristics:**

- 文字为主，状态与操作提示分层清楚。
- 同一输入浮层结构承载三套完整皮肤。
- 实色表面、细线图标与适量圆角。
- 浮层不激活，换肤保留正在进行的输入。
- 设置保持中性，编辑与保存使用原生控件及明确文字反馈。
- 声核在小尺寸保留开口与中央胶囊，品牌身份与输入模式各自表达。

本规范提取自 `src/ui_theme.hpp`、`src/confirm_box.cpp`、`src/candidate_bar.cpp`、`src/preferences_window.cpp` 和 `src/engine_main.cpp`，记录已实现的原生组件。前置 YAML 为规范 token；其中可移植的 `px` 长度表示 Windows DIP（96 DPI 基准逻辑像素），原生实现通过 `UiPx` 按窗口 DPI 换算。`docs/design/skin-preview/index.html` 是浏览器比较原型，其列式排版与展示尺寸不规定原生应用布局。

品牌几何与资源另据 `tools/build-brand-icons.ps1`、`assets/brand/*.svg`、`src/brand_resources.h`、`src/brand.rc`、`src/brand_icon.hpp` 和 `src/input_indicator.*` 提取。矢量坐标使用 24 单位设计空间；ICO 尺寸是实际像素，不与浮层 DIP token 混用。

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
| `brand-ink` / `brand-paper` | 声核的黑白前景；系统品牌徽标为黑标白底，引擎托盘按系统主题使用透明底黑标或白标。 |
| `brand-badge-outline` | 静态系统品牌徽标的半透明黑色细边；不扩展为浮层描边角色。 |

**The Shared Roles Rule.** 换肤改变一整组色彩角色，语音与候选同步使用；状态意义、文字层级和操作位置不随皮肤变化。

**The Neutral Utilities Rule.** 设置客户区与静态文字复用钛银背景和正文角色，不随输入浮层皮肤改变；按钮、标签页、输入框、列表及预览编辑器保留 Windows 控件配色、焦点和禁用状态。系统高对比度仍优先，不把系统颜色固化为新皮肤。

**The System Contrast Rule.** Windows 高对比度开启时，背景取 `COLOR_WINDOW`，正文与次要文字取 `COLOR_WINDOWTEXT`，选中与 hover 底色取 `COLOR_HIGHLIGHT`，其上的候选正文、序号和设置文字取 `COLOR_HIGHLIGHTTEXT`。系统配色覆盖皮肤配色，并恢复可见外轮廓。

普通小字以实际使用的背景检查对比度，包括 hover。极昼次要文字的已复核结果为白底 5.54:1、hover 底 4.95:1；不能以降低文字对比度取得轻盈感。高对比度的系统颜色是运行时值，不固定为第三套或第四套 hex 调色板。

## Typography

**Body Font:** Microsoft YaHei UI。原生绘制统一调用 `UiFont`，浮层使用正常字重与 ClearType；设置标题使用半粗体，其他设置及编辑控件使用正常字重。字体不可用时由 Windows 字体映射处理，没有另设网页字体栈。

**Character:** 中文字形清楚、紧凑，正文与候选词处于同一视觉层级。此输入浮层没有展示型大标题，也不依赖字重变化强调状态。

### Hierarchy

| token | 用途 |
| --- | --- |
| `body` | 语音正文、空内容引导、候选词。 |
| `status` | 语音浮层顶部的录音、校准、待确认、写入及异常状态。 |
| `label` | 操作提示、长文滚动提示、候选序号和设置入口。 |
| `preferences-title` | 设置窗口中的产品标题；不扩展为输入浮层标题。 |
| `preferences-control` | 设置标签页、字段、说明、状态与按钮。 |
| `preview-editor` | 原生预览编辑器的正文、状态与按钮。 |

行高取 GDI 实际字体度量，语音正文采用自然换行；不指定网页式行高或字距。候选词单行垂直居中，超出可用空间时显示省略号并保留完整内容的工具提示。

**The Readable Small Text Rule.** 操作提示与候选序号使用同一小字层级；空间不足时先换行或限制容器，而不是继续缩小文字。

## Layout

布局单位为 DIP，随所在窗口 DPI 缩放。两个浮层都跟随插入点，使用当前显示器工作区约束位置，并保持宿主编辑器焦点；没有网页断点或移动端列布局。

- **语音浮层：** 宽度取 `voice-*` token，工作区更窄时收缩到工作区宽度；高度随正文和提示实测，最多 320 DIP 且不超过工作区高度。左右正文留白取 `surface-inset`，正文起点为顶部 44 DIP；状态在顶部，操作提示在底部。
- **长文本：** 正文视口按完整字体行限制高度，底部另留滚动说明空间。右侧滚动条保持在正文区；鼠标滚轮、拖动滑块及点击轨道可查看全文，末尾显示“已到末尾”。
- **候选条：** 单行最多五项，高度取 `candidate-*` token。候选区上下与起始留白取 `candidate-inset`，宽度随文字测量，整条最大为 960 DIP 且受工作区约束；右侧保留 52 DIP 设置区域。候选序号和词之间有固定测量区，词内容按剩余宽度省略。
- **定位留白：** 语音与插入点的首选间隔为 6 DIP，候选为 5 DIP；下方空间不足时尝试放在插入点上方，最终限制在工作区内。无法取得有效插入点时，先按宿主客户区右下角内缩 16 DIP 定位，再进行工作区约束。
- **设置窗口：** 客户区目标大小取 `preferences` token，窗口整体受当前显示器工作区约束。六个标签页共享标题、状态区及底部操作；主内容左边距 28 DIP。高度不足时以整页垂直滚动访问底部，滚轮、滚动条和 Tab 焦点移入可见区共同保证操作可达；切换标签页回到页顶，底部操作不固定悬浮。
- **预览编辑器：** 初始窗口外框大小取 `preview-editor` token，可调整尺寸，最小外框为 560 × 330 DIP。客户区内缩 20 DIP，多行编辑区随窗口伸展，状态在正文下方；恢复原文与复制位于底部左侧，保存位于右侧，按钮高 34 DIP。编辑区使用原生垂直滚动。

**The Stable Input Rule.** 相同内容和锚点不重复移动、显隐或重绘；换肤只更新圆角与绘制，不重新定位浮层、不重启录音、不清空预览、不改变候选选择或滚动位置，也不让隐藏浮层重新出现。

## Elevation & Depth

深度由实色表面、候选色块、细分隔及 Windows 原生窗口阴影共同表达。两个窗口类均请求 `CS_DROPSHADOW`，实际阴影由 Windows 决定，没有项目自定义的模糊半径、偏移或透明度 token。钛银与深空用细外轮廓稳定边界；极昼省略外描边和语音内分隔，以净白连续表面与系统抬升形成层次。

绘制采用完整离屏帧提交，避免只出现背景的闪烁。录音、校准和待确认状态通过文字与线条图标立即更新；当前原生组件没有装饰性循环动画、渐变过渡、透明玻璃或背景模糊。

设置与预览编辑器使用普通 Windows 顶层窗口框、原生控件边界与焦点反馈；其非客户区和系统控件深度由 Windows 绘制，不新增自定义阴影或浮层圆角 token。

## Shapes

外轮廓使用各皮肤的 `*-surface` 圆角，并应用于语音与候选窗口的实际区域裁切。钛银偏紧凑，极昼更柔和，深空居中。选中与 hover 候选使用 `candidate` 圆角，极昼单独使用 `daylight-candidate`；这是局部高亮形状，不改变候选条尺寸。

图标由 GDI 细线绘制：麦克风、校准滑杆及待确认勾号各自承担状态含义。钛银采用方形状态点和选中项细下沿；极昼与深空采用圆形状态点，极昼额外使用浅蓝圆角图标底。外描边、分隔与图标线宽均以 1 DIP 为基准，并至少绘制一个物理像素。

声核品牌另用固定矢量几何：24 单位画布上的圆角围合在右下方留开口，中央为竖向胶囊，端点与连接均为圆角。外框描边为 3.4 单位，中央描边为 4.4 单位；黑白版本共用相同路径。系统品牌徽标在白色圆角底板内将标记等比缩至 75%，并在两轴各平移 3 单位。小尺寸保留这组轮廓关系，不叠加文字、渐变、发光或录音动画。

## Components

### Sound core identity and input mode

**性格：** 小尺寸下可立即辨认的安静标记。品牌图形与输入模式使用各自的资源，字体和三套皮肤沿用既有规范。

- **系统品牌徽标：** `brand-profile-badge` 使用黑标白底与细边，固定为资源 100；注册项 `IconIndex` 使用 `-100`，按明确的负资源 ID 引用。底板与标记整体作为资源缩放，不把透明底黑标直接用于未知背景。本机桌面任务栏已实拍确认独立「中」模式与声核品牌并列显示。
- **品牌注册链路。** 通过 `ITfCategoryMgr::RegisterCategory` 与 `ITfInputProcessorProfileMgr::RegisterProfile` 注册 keyboard、display attribute 和 system tray 类别。`rc.4` 增加 Windows 终端所需的 `IMMERSIVESUPPORT`。输入服务在 AppContainer 中放行键盘输入，将本地词库和语音服务的使用范围限定在桌面进程。HKLM 保存 COM 与 profile，迁移清理本产品旧 HKCU 镜像，由 `EnableLanguageProfile` 保存用户启用状态。
- **引擎托盘：** 深色系统主题使用 `brand-mark-on-dark`，浅色主题使用 `brand-mark-on-light`，均为透明底。高对比度开启时，根据 `COLOR_WINDOWTEXT` 的明暗在黑白资源之间选择；这不是把任意系统前景色直接写入品牌资源。托盘大小按任务栏 DPI 从 16 逻辑像素换算，皮肤选择不改变品牌形状或主题判断。
- **输入模式：** 独立 TSF 按钮显示「中」或「A」，程序通过 `ShellUsesLightIcons` 显式载入黑色或白色模式资源，主题与高对比判断和托盘一致。左键点击与 Shift 切换同一输入状态；不可编辑时按钮禁用。模式改变更新图标、文字和提示，可用状态改变通知系统刷新禁用状态。隐藏、不激活的 TSF 顶层窗口接收主题、系统设置、显示与 DPI 变更消息后通知模式图标刷新，不显示额外窗口或任务栏按钮。
- **原生资源：** 七组 ICO 均提供 16、20、24、28、32、40、48、64、128、256 px 的 32 位透明图像；黑白声核与黑白「中 / A」另有可编辑 SVG。白色模式资源为 `mode-chinese-white`（105）与 `mode-english-white`（106）。16–48 px 是品牌小尺寸识别的主要检查范围，ICO 的其余尺寸仍使用同一几何。

**The Identity and Mode Rule.** 声核只表达产品身份，保持静态；「中 / A」单独表达输入模式。品牌不代表正在录音、识别进度或已完成写入，不将模式字叠进声核。

### Voice preview

**性格：** 清楚、安静的输入确认仪表。状态、正文、操作提示保持固定层级，换肤不改变语音流程。

| 状态 | 视觉与提示 |
| --- | --- |
| 正在听 | 麦克风线图标、临时识别正文；提示停顿自动校准、Enter 停录预览、Esc 取消。 |
| 正在校准文字 | 校准线图标与当前文字；提示校准后预览并确认。 |
| 待确认 | 勾号、最终预览文字；提示 Enter 写入、右键编辑 / 复制、Esc 取消。 |
| 正在写入正文 | 校准线图标；提示等待应用确认。 |
| 语音暂不可用 | 明确状态文字与失败原因；底部提供右键查看设置 / 编辑预览、Esc 关闭，图标采用次要前景。 |
| 写入结果待核实 | 提醒先核对正文，确认未写入后再重试，避免把未知结果画成成功。 |
| 录音已结束且无正文 | 空内容说明和重新开始提示。 |

录音中的第一次 Enter 触发停录与校准，最终文字保留在待确认浮层；校准中 Enter 不预约自动提交。异常含义通过文字表达，不新增独立告警颜色。浮层正文仍为只读预览；右键的“编辑预览…”打开独立原生编辑器，保存只更新待确认内容，仍需用户回到输入位置确认写入。

### Candidate strip

**性格：** 紧凑的单行选词工具。当前选中项以完整底色和对应文字颜色表达；钛银另加细下沿。鼠标 hover 与键盘选择可同时存在，选中项优先使用选中样式。高对比模式的 hover 前景遵守系统高亮规则。

候选支持现有键盘选词与鼠标选择；无障碍对象提供完整候选名、选中状态及“选择并写入”动作。视觉省略不裁掉可访问名称。浮层收到鼠标操作时不激活，也不接管编辑器焦点。

### Settings and skin menu

**性格：** 安静、可识别的原生入口。候选条右侧“设置”采用次要文字、细竖线分隔和 hover 底色。托盘与此入口打开同一 Windows 菜单，“皮肤”子菜单列出钛银、极昼、深空，并用系统单选标记当前选择。菜单、快捷键对话框及其按钮继续遵守 Windows 原生控件外观，不套用浮层的自绘按钮样式。

选择成功后写入 `%LOCALAPPDATA%\VoiceIme\appearance.json`，通知已存在的新版浮层同步刷新；后续新窗口读取该选择。缺失或无效偏好回退钛银。保存失败时保留原皮肤并提示重试。皮肤偏好独立于快捷键 `config.json`，不增设自动跟随系统深浅色或其他未批准开关。

### Native preferences

**性格：** 明确、可检索的原生设置工具。固定钛银背景承载“输入”“我的词语”“语音设备”“使用诊断”“本地模型”“备份恢复”六个原生标签页；各字段使用可见标签，列表、下拉框、按钮和焦点反馈由 Windows 绘制。已选择深空或极昼时，设置仍保持中性。

- **导航与键盘：** Tab / Shift + Tab 遍历原生控件（包括只读多行输出），Ctrl + Tab / Ctrl + Shift + Tab 循环切换标签页，带助记字母的标签和按钮提供原生快捷访问，Esc 关闭。焦点因 Tab 移到视口外时滚动到该控件。默认动作按页区分：输入、语音设备保存设置，我的词语保存词语，本地模型启用所选模型，其他页面关闭。
- **搜索与空态：** 词语或输入码变更即重新搜索并回到第一页，每页最多 100 条。无词语显示“还没有保存词语”，有查询但无结果显示“没有找到匹配词语”。结果范围与词库总数分开表述为“匹配结果 …，词库共 … 条”，不把词库总数当作命中数量；上一页、下一页依实际页边界禁用。
- **字段与操作：** 词语和输入码并列，输入码生成后允许核对修改。未选词时删除禁用，撤销只针对最近一次删除。词语保存与设置保存是各自明确的动作；“保存设置”仅在相关两个标签页显示。
- **反馈与错误：** 页面底部使用可选择、复制和滚动的原生只读文字区显示已保存、已删除、已恢复或具体失败原因，长路径不因固定高度而丢失。输入无效、读取或保存失败时保留可处理上下文；部分保存成功须明确指出“输入设置已保存，外观保存失败”。缺失的已选麦克风保留为“当前不可用”选项并要求重新选择，不伪装为正常设备。
- **模型选择：** 固定型号下拉框配一个“启用所选模型”主操作，缺失文件由启用流程下载。“使用已有文件”和“恢复推荐模型”为次要入口；状态明确区分当前模型和待选型号。下载/验证期间禁用重复启用并保留取消，焦点随任务状态留在可用按钮；闲置时取消禁用。轮询相同状态文字不重置选择或滚动位置。
- **诊断：** 使用步骤在前，原生只读多行诊断框在后，另有复制动作。诊断可滚动，复制结果在状态区说明；诊断正文是否过滤敏感数据由业务层负责，视觉审查不替代数据检查。

### Native preview editor

**性格：** 直接、可撤回的文字校对窗口。系统背景和原生多行编辑区承载当前预览，状态文字紧随其下；“恢复识别原文”“复制”“保存预览”分开呈现。打开时焦点落在正文，用户主动编辑时窗口可以激活。

正文中的 Enter 换行；Ctrl + Enter 或“保存预览”保存修改，Tab 在原生控件间移动，Esc 取消本次编辑并关闭。保存只更新待确认预览，不能表现为已经写入宿主应用。恢复操作显示已恢复原文的反馈；复制失败明确提示重试。空文本或含无法识别占位内容时，窗口保留并提示“请补全无法识别的内容，再保存预览”，不显示成功。

**The Explicit Completion Rule.** 状态文字区分预览已保存、已复制、正在写入和结果待核实；恢复、校对及重试保留明确入口，未完成的动作不显示为完成。

### Native evidence

`docs/design/skins/` 中的 `titanium-voice.png`、`titanium-candidate.png`、`daylight-voice.png`、`daylight-candidate.png`、`deep-space-voice.png`、`deep-space-candidate.png` 是本项目 UI 测试工具对实际原生窗口的截图，均非生成图或第三方素材。状态、长文、DPI 和 hover 的完整证据在 `build/skins-2026-11-15/`；最终审查见该目录的 `native-skin-review-verdict.md`。

本次设置、编辑器与新增浮层提示的原生 fixture 截图在 `.impeccable/review/{titanium,daylight,deep-space}/`，审查范围与复审状态见 `docs/research/ui-review-2026-11-15.md`。`preferences-bottom-dpi-192.png` 是合成 `WM_DPICHANGED` 后的客户区截图，只证明所捕获客户区的布局与操作可达；不验证 DWM 非客户区、真实缩放切换或物理跨屏。

rc.6 的设置页审查与修复记录见 `docs/research/code-review-2026-11-16.md`，三套皮肤的确认截图位于 `build-review/public-sprint/review-ui-{titanium,daylight,deep-space}/`。沿用既定皮肤，原生 Win32 按键和控件检查不替代完整屏幕阅读器或物理多屏验收。

截图和注入的高对比度分支验证用于核对原生外观；真人麦克风、用户应用现场、物理多显示器与屏幕阅读器验收仍按产品既定边界单独进行。浏览器比较原型只辅助理解三套方向。

品牌证据在 `docs/design/brand/sound-core-preview.png`：`tools/render-brand-preview.ps1` 从构建后的 DLL 载入图标，通过 Windows `DrawIconEx` 绘制实际像素尺寸；其中模式示例为黑色版本。它证明所示资源渲染与静态图形表现，**不是实际任务栏截图**；系统安装后的入口显示、真实主题切换与交互由安装验收单独核对。资源来源、用途及复现方法见 `docs/design/brand/README.md`。

`rc.3` 的桌面实拍 `build-professional/rc3-taskbar-supported-registration.png` 显示独立「中」模式与声核品牌图标，当时 `GetActiveProfile` 返回 `caps=0x20000`。`rc.4` 的终端兼容验证另见 [`rc.4` 验证记录](docs/releases/v0.2.0-rc.4-validation.md)。

先前「中 简体」未显示品牌的故障范围收敛到旧注册与配置刷新链路：更换图标、索引或仅重启 `TextInputHost` 均未解决，清理并刷新配置后恢复。仅写回旧镜像未立即复现，不能把单个 HKCU 键认定为唯一根因。静态资源预览、注册成功与真实任务栏显示继续分开验证。

## Do's and Don'ts

### Do:

- **Do** 同步使用同一皮肤的语音与候选角色，保留共享的字号、布局和状态语义。
- **Do** 以原生 DIP 与实际字体测量布局，检查正常、选中、hover、长文和高对比状态。
- **Do** 让 Windows 高对比度颜色覆盖预设皮肤，并保持系统高亮底上的对应文字颜色。
- **Do** 换肤时保留输入、预览、选词、滚动位置和窗口可见状态。
- **Do** 以真实原生截图核对实现，保留截图来源与测试范围。
- **Do** 保留中性设置表面及系统控件状态，把输入浮层换肤与原生编辑窗口分开应用。
- **Do** 让键盘焦点和底部操作在滚动后可达，并用文字区分空结果、保存失败和等待确认。
- **Do** 共用已选 A「声核」的黑白几何，让静态品牌、系统主题适配与「中 / A」模式各守其职责。

### Don't:

- **Don't** 将浏览器比较板的宽度、三列排版或按钮设计当作原生输入法规范。
- **Don't** 用更浅的小字、大面积霓虹、装饰动效或透明模糊表达未来感。
- **Don't** 为换肤添加新的确认步骤、重启要求或未经批准的自动深浅切换。
- **Don't** 把高对比度测试分支或截图审查写成真人使用验收结果。
- **Don't** 把保存预览写成已经插入正文，或把词库总数写成搜索命中总数。
- **Don't** 给声核加渐变、发光、模式叠字或录音动画，或把资源预览写成实际任务栏验收。
