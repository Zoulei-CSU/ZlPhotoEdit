# ZlPhotoEdit — 项目速查

> **速查**；规格全在 `docs/`（**实现铁律** / 架构规划 / 构建与运行 / 自检与验证 / 图标体系 / 实现要点详解 /
> 性能优化评估 / 功能覆盖审计 / 曲线 / 画布与文件）；阶段记录在 `.workbuddy/memory/YYYY-MM-DD.md`。
> **动任何模块前先读 `docs/实现铁律.md` 对应章。**

**范围**：Qt 5.15.16 + MSVC 2022 轻量图片编辑器，**纯 Qt、严禁第三方库**。多图层 + 22 混合模式、
矩形/椭圆/羽化选区 + **魔术棒**、15 个工具、调整与滤镜、旋转/翻转/裁剪/缩放、撤销重做、打开为图层、
**多标签页**、**标尺/参考线**、**网格（间距/线色/对齐吸附）**、**另存为 ICO**（自写编码器）、**屏幕截图**（子菜单 + **全局快捷键**，不落盘）、84 张 SVG。不做图层组/蒙版/调整图层/通道、
钢笔、矢量形状层、动作/批处理、PSD/ICC/液化/插件/`.zlpe`、**套索/快速选择**。
**没有文字/形状图层**（文字工具直写像素层）→ 无需栅格化。

## 构建 / 版本控制
Qt `C:\Qt\5.15.16-VS2022-rel`（Release-only）+ VS 2022 Pro；Git Bash 先 `source build_env.sh`
（它把 PortableGit `usr/bin` 补进 PATH —— Agent 会话的 bash 缺 coreutils）。**双路径都要编过**：qmake+nmake、cmake+ninja。
★ `.pro`/`.sh` **不能有 BOM**；`.cpp/.h/.md` **必须有 BOM**（BOM 让 shebang 失效 → 刷 `No such file or directory`）。
★ `/utf-8` 只能用 `target_compile_options`；报错"分散、原因各异"→ **先怀疑全局因素**；**qmake 增量会掩盖错误** → 收口用 CMake `--clean-first` 兜底。
★ **CMake 报 `LNK1104 bin\ZlPhotoEdit.exe`** = **用户正开着这个实例**（`Get-Process ZlPhotoEdit`）→ 死在这一步**不是代码问题**。

git 本地（`main`；`user.name=邹磊` / `user.email=zoulei.csu@gmail.com`；`core.autocrlf=false`）；
`.workbuddy/memory/` **入库**、`.buildlog/` 不入库。
★★ **行尾一律 LF**；已知 CRLF 且已入库的只有 `src/ui/MainWindow.cpp`、`.workbuddy/memory/2026-09-20.md`，**别去"修"**。
★★ **程序化编辑会把整个文件翻成 CRLF** → `git diff` 变"整文件重写"、没法审。查：CR 数 ≈ 该文件总行数；
修 `tr -d '\r' < f > f.tmp && mv -f f.tmp f`（**别用 `sed -i`/`rm`**：删除走 safe-delete，会拦 `rm` 甚至 FAIL_CLOSED 打断整条命令）。改完复核 BOM + 重跑构建自检。

## 架构
`ui → tools → commands → core → engine` 单向；engine 不碰 QtWidgets、全无副作用纯函数；`Document` 是唯一像素源；
工具只产 Command、不 include `CanvasView.h`（屏幕坐标走 `ToolEvent`）。共享代码不知放哪层 → 仿 `tools/MarchingAnts.*`。

## 约定（最易踩）
- 新增文件补 BOM + **同步四处**：`ZlPhotoEdit.pro` / `.buildlog/shot/shot.pro` / `run_tests.sh` 的
  `run_one mainwin` **源文件清单** **和** `moc_headers_for mainwin_test`。★★ **漏任一处都 `LNK2019`，但缺的符号不同**：
  漏 moc → `moc_X.h` 没生成；漏 `.cpp` → **构造函数符号**解析不了（2026-09-23 魔术棒栽在这）。
- ★ **新增测试短名**同样四处：`run_one` 分派 / `moc_headers_for`（按**可执行文件名**）/ `extra_libs_for`（要 Qt5Widgets 的加）/
  末尾**全量清单** + 用法注释。★★ **2026-09-23 实测漏的正是"末尾全量清单"** → 退出码 0、无失败，但**汇总里根本没这个测试**。
  落盘后数一遍（`grep -c` 数的是**行**）：`magicwand` 应 3 行、`magic_wand_test)` 应 2 行。
- ★ **菜单动作四件套** `new`→`setObjectName`→`connect`→`addAction`；`addAction(nullptr)` 合法且静默 → 守门 `./run_tests.sh actions`。
- ★ **同一文件一次只做一个 Edit**（连发会基于旧内容写回、静默覆盖）；落盘后必须读回复核。
- ★ **图标三方对账**（代码引用 ⇄ `resources/icons/*.svg` ⇄ `resources.qrc`）；`gen_icons.py` 生成、产物勿手改；
  禁用 `QIcon(":/…")`、用 `QSvgRenderer` + `isValid()`；有名菜单动作必须有图标。
- ★★ **`resources.qrc` 是 `gen_icons.py` 整体重写的生成物** → 手加的行会被**静默抹掉**；
  要进 qrc 的非生成文件登记在脚本的 **`FIXED_RESOURCES`** 表里（`logo.svg` 就是）。
  ⇒ 一般化：**文件会不会被某个脚本整体重写？会，就永不许手改**，去改那个脚本的数据表。
- ★★ **程序名 / 图标都只有一个来源：`ui/AppInfo.h`**。
  · 显示名 = `逐鹿 图片编辑器`（中文，给人看）；英文名 = `ZlPhotoEdit`。
  · ★ **`kApplicationName`/`kOrganizationName` 是 QSettings 的路径，一旦发布不许改** ★
    改它 ≠ 改名，是**把用户全部设置重置**。中文名**只许**用于显示（标题/关于/退出提示）。
  · 标题拼法只在 `appinfo::windowTitleFor()` 一处（以前手写两遍 `" - ZlPhotoEdit"`）。
  · 图标**两层**：窗口图标（qrc + `setWindowIcon`，运行时）/ **exe 图标**（`resources/app.rc`
    → `rc.exe` → PE 资源段，**链接期，运行时改不了**）。`.rc` 里 **ID 必须是 1**、
    路径写裸文件名 `logo.ico`、**不加 BOM**；两条构建路径指向**同一个** `.rc`。
  · `ZLPE_HAS_EXE_ICON=1` 在 `.pro`/`CMakeLists`/`run_tests.sh` 的 `defines_for()` **三处**都要有。
  · ★ 图标失效是**静默**的 → 自检必须**量像素**（`opaquePixelCount`）+ 数**不同颜色数**；
    别断言覆盖率（这张 logo 16px 覆盖 0.98 是正常的）。
- ★★ **预览态两条事件通路**：画布按"此刻有没有按键"分派（无键 → `hoverMove`，有键 → `mouseMove`）。只在 `hoverMove`
  更新 = **按住拖动时预览不动**（用户 2026-09-21 报的）→ `mouseMove` 那句必须在**所有提前 return 之前**，
  "本帧没落笔"也要 `repaint()`。守门 `./run_tests.sh brushtool` 的 [6] 组（**量叠加层圆心**）。

- ★ **文档不抄第二份真相**：目录树 / `.pro` / `CMakeLists.txt` 一律**引用**或现取 `ls`，别整段手抄 ——
  会随仓库漂移，且**过期时没人发现**。要写就写"要点"（依赖方向 / 纯函数层 / 为什么某类逆流放置）。

## Qt5 陷阱
- ★ **`QList<T>` ≠ `QVector<T>`**（无 `resize()`/`(size,value)`/`data()`）→ 像素缓冲/LUT 一律 `QVector`。
- **禁字符串式连接**（`SLOT(...)` 静默失败）→ 一律 lambda。
- ★★ **`mapTo(parent,…)` 的 parent 必须是自己的祖先** ★★（两个真 bug 的根因）。标尺与画布是**兄弟** → 传兄弟进去
  父链走到 `nullptr` → **段错误**，且构造/resize/show 全正常、崩在 **`show()` 之后第一次重绘**里。
  兄弟换算一律 `b.mapFromGlobal(a.mapToGlobal(p))`。
- `QWheelEvent` 无 `localPos()`；`Layer::image()` 返 const 引用要改先拷贝；版本判断只在 `compat/QtCompat.h`。
- ★ **Qt 类的前置声明必须写在全局命名空间**（放在 `namespace zl {` **之前**）。写在里面 = 声明了 `zl::QTimer` →
  报 `C2027 使用了未定义类型 zl::QTimer` / `C2065 timeout 未声明`，位置指向 `new`/信号那一行，**看起来像头文件没包含**。
- ★ **`const QKeySequence s(QLatin1String(x));` 是 most vexing parse**（编译器当**函数声明**）→ 报错指向**调用处**
  "cannot convert `QKeySequence (__cdecl*)(QLatin1String)`" → 用花括号 `{QLatin1String(x)}`。
- ★ **Win32 API 的链接**用 `#pragma comment(lib, "user32.lib")` 写在**用到它的那个 `.cpp`** 里，
  不进 `.pro` / `shot.pro` / `run_tests.sh` 三份手工清单（漏一处只在链接期报 `LNK2019 __imp_XXX`）。

## 撤销 / 压命令的唯一入口
图层 `ARGB32_Premultiplied`；选区 `Grayscale8` 蒙版（**不用 QRegion**）；`QUndoStack` + `QUndoView`；`setUndoLimit(30)`。
画笔改**脏矩形**、**整笔一条**；滤镜/调整/变换/裁剪 → **整层快照**。
- ★ 图像级命令的内存预检**只有 `MainWindow::commitGeometryChange()` 一个入口**（工具不得自己 new）。
- ★ **面板/标尺只发信号**，改动一律 `MainWindow` 包命令（拖动只预览、松手压一条）。**新增任何改文档的操作，
  先问"会不会开出第二个压命令的地方"。**
- ★ `DrawCommand` **描边编号每条必须不同**（撞号 → 静默 `mergeWith`）→ 一律 `standalone()`。
- ★ 合成写回**不再二次预乘**：`mixR/G/B` 已是预乘量 → 直接 `qBound(0,x,1)*255`。

## ★ 三条"静默失效"（最常复发）★
1. **装配顺序**：凡"构造期就改过状态、UI 要跟着走"的 → 问"连接建在首次变更**之前**吗"，不是就**显式补推**。
2. **自绘控件**：每条自绘管线都要有"**量它画出的像素**"断言，且**先植入 bug 证明抓得住**；数学全绿而画面不对，
   别读代码猜 → 在 `paintEvent` 打中间量。★ 采样行用**控件坐标**；亚像素的线会被抗锯齿摊到相邻两行。
3. **通用兜底分支**：`if(!mode.isEmpty())` 这类**必须先排除专用模式**（否则专用分支永远走不到）。

## ★ 带对话框的功能 → "干活的那半"做成公开入口 ★
模态框在**无人环境永久挂死**（不是报错）。`openAsLayer()`（选文件）与 public `importImageAsLayer(path, GrowPolicy)`
（干活）**必须拆开** → 截图工具能驱动、自检能断言。同套路：`TextTool::placeText()`、`MagicWandTool::selectAt()`。
★ 截图工具拿撤销栈**不能 `findChild<CommandStack*>()`**（每文档一个栈）→ 从 `ToolManager` 的工具取。

## 铁律索引（细节全在 `docs/实现铁律.md`）
| 章 | 最易复发的一句 |
| --- | --- |
| 三 画笔/橡皮 | 幂等重算（快照 + 累积蒙版取最大）；轮廓圆悬停缓存 **`hoverMove`/`mouseMove` 两条通路都要更新**；`paintScreenOverlay` 的 `view` **可为空** |
| 十一 图层面板 | 只读文档 + 只发信号；**行序倒序**（只许 `rowToLayerIndex/layerIndexToRow`）；delegate 自绘；缩略图 **300ms 节流** |
| 十三 形状 | 蒙版**取 alpha**、禁 `convertToFormat(Grayscale8)`；预览**幂等**；重算区 = 上帧 ∪ 本帧 |
| 十四 视图工具 | 参数名带 `…Widget`/`…Image`；增量 = 本次 − 上次；阈值用**控件坐标**；退化**必须回落成单击** |
| 十五 文字 | **浮层归画布、内容归工具**；`lineHeight` 一起传；锚点 = **首行顶部**；"蒙版没墨"单独判 |
| 十六 跨图导入 | "图→图层"**只有 `addImageAsNewLayer()` 一份**；新层**必须填满画布**；放不下**必须问** |
| 十七 多标签页 | 三指针只有 `activateTab()` 重指（`-1` = 空状态）；**销毁裸指针消费者所指之物前先让它松手**；**信号只传变化不传现状** → 换页显式补推 |
| 十八 标尺/参考线 | 刻度**纯函数**（判据用**屏幕**像素）；参考线存 `CanvasView`（每页一份）；**方向 == 尺子方向**；命中容差 **5 控件像素** |
| 十九 ICO | `IcoWriter` **纯函数**；≤256 走 BMP、257~512 走 **PNG 条目**；`biHeight` = **2×高**、掩码**置 1=全透明**、预乘**必须还原** |
| 二十 增量合成 | 性能主战场（20.0~20.17）→ 见下节 |
| 二十一 魔术棒 | 复用 `floodFill()` 的**覆盖蒙版** → `applyMask()` + `SelectionCommand`（**零新增 engine 代码**）；判据默认**合成图**、可切"仅当前层"；**先羽化新蒙版**再 applyMask；减/交后空 → `sel.clear()` |
| 二十二 屏幕截图 | 平台 `#if` **只许待在 `compat/`**；抓屏 = **设备像素 + dpr 归一 1.0 + 失败空图**；★ **不猜 `grabWindow(0)` 抓的是哪一块 → 量尺寸再裁** ★；延迟截图 = **先最小化自己、到点先抓后还原**，且"是不是我们干的"要**记住**（连排两次别重判）；登记热键**必须响亮失败**（`MOD_NOREPEAT`、拒裸单键、`simulateActivation()` 投真 `WM_HOTKEY` 验通路）；"图 → 一页"只有 `createImagePage()` 一份 |
| 二十三 应用名/图标 | 显示名**只**来自 `ui/AppInfo.h`；`kApplicationName`/`kOrganizationName` 是 **QSettings 路径**，改了 = **重置用户全部设置**；`resources.qrc` 是生成物 → 非生成文件登记在 `gen_icons.py` 的 **`FIXED_RESOURCES`**；exe 图标走 `app.rc`（**ID 必须 1**、不加 BOM）；失效是**静默**的 → 自检**量像素** |
| 二十四 网格/吸附 | 吸附**纯函数**在 `engine/GridMetrics`；**"显示"与"吸附"两个独立开关**；容差是**屏幕**像素、在 `makeToolEvent()` 折算成图像像素塞进 `ToolEvent`（工具不 include `ui/`）；★ **超容差就不动**（"临近才吸"）；移动吸**内容包围盒**的边、选区**起点也吸**；吸附放 **Shift 约束之后**；★ 横线会让**每一列**都有变化 → 量竖线要断"**该列变化行数 > 半**"，再**把相邻列并成一条线**取中心 |

## ★★ 性能（口径与数据全在 `docs/性能优化评估.md` + 铁律二十章）★★
- **根因**：每个画笔/形状事件都调 `Document::markDirty()` → **整画布全图层重新合成**；画笔只算/只写脏矩形，
  **把"只动一小块"这个信息丢了**。实测 4K×3 层 **431ms**，而**只算笔尖 64px = 0.25ms** → 唯一值得做的优化。
- ★ **正确性基石**：22 种混合模式全是**逐像素、无邻域运算** → **矩形内合成 == 全图对应子区域（逐位相等）**；
  **像素断言全项目 0 容差**，不为任何优化放宽。
- ★ **已实施**：方案 A 区域合成（批一/批二）+ **§4.5 不透明上层直接覆盖**（照片场景 **7.8~9.4×**）。
  **已否决**：方案 B 倒数查表（天花板 1.26×、只作用冷路径、要 512KB 常驻表）—— **别重开这个坑**。
- ★★ **三个花过真代价的坑**：① `movetool` —— **移动要脏"上帧 ∪ 本帧"**（只脏本帧 → 拖动期残影、松手消失）；
  ② §4.5 的门**必须带全**（漏 mode 门 = 算错；漏不透明度门 = opacity<1 也覆盖），判据在**像素**上**不是"整层"**；
  ③ **20.14 假绿**：批次一宣称"画笔已接入区域失效"，其实 `BrushTool::applyRegion()` 收尾仍是 `markDirty()`，
  而 `perf` ⑤ 组**直接调 `Document::markDirtyRegion()`**、没经过 `BrushTool` → **遥测计数只有接到真实调用链才有效**。

## ★★ 自检节奏（用户明确要求）★★
**改一个功能不要跑全量！** 全量 ≈ 6 分钟，会把功能推进整个卡死。改一处 → **只跑相关短名**
（`./run_tests.sh ico` / `actions` / `magicwand` …），秒级～十秒级。跑全量只有三种时机：① **阶段收口**；
② 改动**跨模块 / 动了公共头**；③ **用户主动要求**（收口那次必须真跑）。★ CMake `--clean-first` 与全量自检**同频**。

## 已定案（产品，逐条规格见 `docs/画布与文件操作设计.md` / `架构规划.md` §10）
**不做** `.zlpe`（只导出扁平图）；`Ctrl+V` 为新图层；画布缩小走**智能适配**；尺寸单位**像素/百分比并存**（相对原尺寸）；
**取消旋转要连视图一起还原**；**裁剪是破坏性的**（裁到选区包围盒）；跨图两条路并存（**打开为图层** + **多标签页**）；
标尺单位 px/cm/mm/inch/pt；**ICO `≤512` 硬上限、超限拒绝**（**不用 `QImageWriter`** —— qico 会把 300×300 **静默缩到 256**）；
**性能 A ✅ / B 否决 / §4.5 ✅**；判据 = **全局参数化操作可降采样预览、局部像素操作必须全分辨率但只需重算改动区域**；
**屏幕截图**：立即/延迟 3·5·10 秒四项，**立即挂全局快捷键 `Ctrl+Alt+S`**（最小化也能按）；
★★ **这个键只是"首选"，必须有回落** ★★ —— 全局热键全系统只能一个程序登记，且**占用者时变**
（实测同机几分钟内出现过"能登记"↔"被占用"两种结果）→ 按**候选表**逐个试、第一个成功即生效键，
**并把生效键写回菜单**（菜单写的=能按的）；整表全失败才退回窗口内快捷键并说明原因；
截**光标所在屏**（不做多屏拼接）、结果 = **新建一页内存文档**（不落盘、走 `titleHint` 而**不伪造 `filePath`**）；
**不做**区域截图 / 标注涂鸦 / "截图直接贴到当前画布"（后者已由「粘贴为新图层」覆盖）。

## 现状
`src/` = `{compat, core, engine, commands, tools, ui}` + `main.cpp`；全 UTF-8 BOM；**双构建 0 错 0 警**。
自检 **3649 项 / 0 失败**（2026-10-08 全量：**24 个测试程序**，`perf` 728 / `mainwin` 518 / `ico` 317 / `magicwand` 125 / `grid` 37 / `ruler` 51 / `icon` 29）；
`mainwin` ~2.5min、`perf` ~25s；另有不编译的 `action_check.py`。**阶段进度见 README 的阶段表**。
★ **防假绿**：脚本**不删旧产物**、改**比对 mtime** → ★ **还原文件时别保留 mtime**（否则静默复用旧二进制）。
★ 截图 `shot.exe <图> <out.png> <模式>` → `.buildlog/*.png`；输出**混编码** → **逐行**试 utf-8 再回落 gb18030。
  （本轮又证一次：`python -c` 里直接 `.decode("utf-8")` 会因混编码抛异常 → 必须**逐行**试。）
  ★ **Python 在 Git Bash 里不认 `/tmp`**（`open("/tmp/x")` 报 FileNotFound，而 `ls /tmp/x` 在）
    → 先 `cp` 进工程再读。★ **`python -c` 里的反引号会被 bash 当命令替换** →
    要写含反引号的文本（文档！）用 `python3 << 'PYEOF'` heredoc。
  ★★ **多标签后取画布不能用 `findChild<CanvasView*>()`**（返回**第一页**，工具持有的是**当前页** → 症状像工具坏了）
  → 走 `DocumentTabs::currentTab()->canvas`；★ 同理 `setActiveTool()` 必须在 `loadAllOptions()` **之后**。
  ★ 新增模式要加进 `kReservedModes` 表（**通用兜底会把任何非空 mode 当动作名**）；本轮加了 `appicon`。
★ 植入脚本 `.buildlog/implant_*.py`；**长脚本一律 `run_in_background`**。
★ **"工具说成功了" ≠ "改动在文件里"** → 依赖的改动落盘后必须有独立复核动作（协作约定见 `~/.workbuddy/MEMORY.md`）。
