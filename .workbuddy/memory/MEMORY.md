# ZlPhotoEdit — 项目速查

> 只留**速查**。规格在 `docs/`（架构规划 / **实现铁律** / 构建与运行 / 自检与验证 / 图标体系 /
> 实现要点详解 / 功能覆盖审计 / 曲线 / 画布与文件），阶段记录在 `.workbuddy/memory/YYYY-MM-DD.md`。
> **动任何模块前先读 `实现铁律.md` 对应章。** README 只做功能简介，别往里塞实现细节。

**范围**：Qt 5.15.16 + MSVC 2022 轻量图片编辑器，**纯 Qt、严禁第三方库**。多图层 + 16 混合模式、
矩形/椭圆/羽化选区、14 个工具、调整与滤镜、旋转/翻转/裁剪/缩放、撤销重做、打开为图层、
**多标签页**、**标尺 / 参考线**、**另存为 ICO**（自写编码器）、78 张 SVG。
不做图层组/蒙版/调整图层/通道、钢笔、矢量形状层、动作/批处理、PSD/ICC/液化/插件/`.zlpe`。
**没有文字/形状图层**（文字工具直写像素层）→ 无需栅格化。

## 构建
Qt `C:\Qt\5.15.16-VS2022-rel`（Release-only）、VS 2022 Pro。Git Bash 先 `source build_env.sh`。
**双路径都要编过**：qmake+nmake、cmake+ninja。
★ **`.pro` 不能有 BOM**；**`.cpp/.h/.md` 必须有 BOM**。
★ `/utf-8` 只能用 `target_compile_options`；报错"分散、原因各异"时**先怀疑全局因素**。
★ **qmake 增量会掩盖错误** → **收口时**用 CMake `--clean-first` 全量兜底（与全量自检同频，别每改一处就跑）。

## 版本控制
本地 git 仓库（`git init` 于 2026-09-21，分支 **`main`**）。仓库级账户
`user.name=邹磊` / `user.email=zoulei.csu@gmail.com`；`core.autocrlf=false`（保住 `.sh` 的 LF）。
`.gitignore` 已挡住 `build*/`、`Makefile*`、`debug/`、`release/`、`.qmake.stash`、`.buildlog/`。
★ `.workbuddy/memory/` **入库**（是项目笔记，不是缓存）；`.buildlog/` 不入库。

## 架构
`ui → tools → commands → core → engine` 单向；engine 不碰 QtWidgets、全无副作用纯函数；
`Document` 是唯一像素源；工具只产 Command、不 include `CanvasView.h`（屏幕坐标走 `ToolEvent`）。
共享代码不知放哪层 → 仿 `tools/MarchingAnts.*`。

## 约定（最易踩）
- 新增文件补 BOM + **同步四处**：`ZlPhotoEdit.pro` / `.buildlog/shot/shot.pro` /
  `run_tests.sh` 的 `run_one mainwin` **和** `moc_headers_for mainwin_test`（漏最后一处 = `LNK2019`）。
- ★ **菜单动作四件套** `new`→`setObjectName`→`connect`→`addAction`；`addAction(nullptr)` 合法且**静默**
  → 守门 `./run_tests.sh actions`。
- ★ **同一文件一次只做一个 Edit**（连发会基于旧内容写回、**静默覆盖**）。
- ★ **图标三方对账**（代码引用 ⇄ `resources/icons/*.svg` ⇄ `resources.qrc`）；`gen_icons.py` 生成、
  产物勿手改；**禁用 `QIcon(":/…")`**，用 `QSvgRenderer` + `isValid()`。
- ★ **有名菜单动作必须有图标**；刻意不给 `objectName` 的项（如标尺单位 5 项）靠**子菜单**定位。

## Qt5 陷阱
- ★ **`QList<T>` ≠ `QVector<T>`**（无 `resize()`/`(size,value)`/`data()`）→ 像素缓冲/LUT 一律 `QVector`。
- **禁字符串式连接**（`SLOT(...)` 静默失败）→ 一律 lambda。
- ★★ **`mapTo(parent,…)` 的 parent 必须是自己的祖先** ★★（两个真 bug 的根因）。标尺与画布是**兄弟**
  （都在中央网格宿主下）→ 传兄弟进去父链走到 `nullptr` → **段错误**，且构造/resize/show 全正常、
  崩在 **`show()` 之后第一次重绘**里。兄弟之间换算一律 `b.mapFromGlobal(a.mapToGlobal(p))`。
- `QWheelEvent` 无 `localPos()`；`Layer::image()` 返 const 引用要改先拷贝；版本判断只在 `compat/QtCompat.h`。

## 撤销 / 压命令的唯一入口
图层 `ARGB32_Premultiplied`；选区 `Grayscale8` 蒙版（**不用 QRegion**）；`QUndoStack` + `QUndoView`；
`setUndoLimit(30)`。画笔改**脏矩形**、**整笔一条**；滤镜/调整/变换/裁剪 → **整层快照**。
- ★ 图像级命令的内存预检**只有 `MainWindow::commitGeometryChange()` 一个入口**（工具不得自己 new）。
- ★ **面板/标尺只发信号**，改动一律 `MainWindow` 包命令（拖动只预览、松手压**一条**）。
  **新增任何改文档的操作，先问"会不会开出第二个压命令的地方"。**
- ★ `DrawCommand` **描边编号每条必须不同**（撞号 → `QUndoStack` 静默 `mergeWith`）→ 一律 `standalone()`。
- ★ 合成写回**不再二次预乘**：`mixR/G/B` 已是预乘量 → 直接 `qBound(0,x,1)*255`。

## 铁律索引（细节全在 `docs/实现铁律.md`）
| 章 | 一句话 |
| --- | --- |
| 十一 图层面板 | 只读文档 + 只发信号；**行序倒序**（只许 `layerpanel::rowToLayerIndex/layerIndexToRow`）；delegate 自绘（`setItemWidget` 毁拖拽）；缩略图 **300ms 节流**（`if(!isActive()) start()`） |
| 十三 形状工具 | 蒙版**取 alpha**、禁 `convertToFormat(Grayscale8)`；预览**幂等**；重算区 = 上帧 ∪ 本帧包围盒 |
| 十四 视图工具 | 参数名带 `…Widget`/`…Image`；增量 = 本次 − 上次；阈值用**控件坐标**；退化**必须回落成单击** |
| 十五 文字工具 | **浮层归画布、内容归工具**；`lineHeight` 一起传；锚点 = **首行顶部**；**"蒙版没墨"单独判** |
| 十六 跨图导入 | "图→图层"**只有 `addImageAsNewLayer()` 一份**；新层**必须自己填满画布**；放不下**必须问**；居中 |
| 十七 多标签页 | 三指针只有 `activateTab()` 重指（`-1` = 空状态）；容器只报告不业务；★ 销毁裸指针消费者所指之物前先**让它松手**；★ **信号只传变化不传现状** → 换页显式补推 |
| 十八 标尺/参考线 | 刻度**纯函数**（判据用**屏幕**像素；小数位从步长自身推）；参考线存 `CanvasView`（**每页一份**，不进 `Document`）→ 不进导出是**结构性**的；**方向 == 尺子方向**、位置取垂直位移；命中容差 **5 控件像素**；切页 `setView` 重指 |
| 十九 ICO 导出 | `IcoWriter` **纯函数**；≤256 走 BMP 条目、257~512 走 **PNG 条目**（目录项宽高各 1 字节、`0`=256，装不下更大）；`biHeight` = **2×高**、掩码**置 1=全透明**、预乘**必须还原**；**分流点只有 `writeImage()` 一处**；上限 512 是**产品定案**、`>` 不是 `>=`；文案由引擎层出 |

## ★ 三条"静默失效"（前两章之外最常复发）★
1. **装配顺序**：凡"构造期就改过状态、UI 要跟着走"的 → 问"连接建在首次变更**之前**吗"，不是就**显式补推**。
2. **自绘控件**：每条自绘管线都要有"**量它画出的像素**"断言，且**先植入 bug 证明抓得住**。
   数学全绿而画面不对时**别读代码猜**，在 `paintEvent` 打中间量。
   ★ 采样行要用**控件坐标**；线落在**亚像素**位置会被抗锯齿摊到相邻两行 → 别钉死一行。
3. **通用兜底分支**：`if(!mode.isEmpty())` 这类**必须先排除专用模式**（否则专用分支永远走不到）。

## ★ 带对话框的功能 → "干活的那半"必须做成公开入口 ★
模态框在**无人环境永久挂死**（不是报错）。故 `openAsLayer()`（选文件）与 **public**
`importImageAsLayer(path, GrowPolicy)`（干活）**必须拆开** → 截图工具能驱动、自检能断言。
与 `TextTool::placeText()` 绕过浮层同一套路。
★ 截图工具拿撤销栈**不能 `findChild<CommandStack*>()`**（每文档一个栈）→ 从 `ToolManager` 的工具取。

## 已定案（产品）
1. **不做 `.zlpe`**，只导出扁平图（多图层时提示"无法保存图层信息"）。
2. `Ctrl+V` 粘贴为新图层（`Ctrl+Shift+V` 到当前层），插活动层**上方**、带序号、自动选中。
3. 画布缩小走**智能适配**（只在缩小启用）；`geometry::canvasResizeOffset()` 是唯一来源。
4. 自写 bicubic；尺寸对话框共用 `SizeChangeDialogBase`；**不做实时像素预览**。
5. 尺寸单位**像素/百分比并存**，百分比相对**原尺寸**；换算只有 `percentToPixels/pixelsToPercent`。
6. **任意角度旋转**：画布按包围盒扩大、**一像素都不裁**；90° 倍数走无损重排；取消时**连视图一起还原**。
7. **裁剪是破坏性的**；「图像 → 裁剪」裁到选区包围盒、无选区置灰。
8. **跨图复制两条路并存**：①「文件 → 打开为图层…」（比画布大时弹三选一、**默认扩大**）；② **多标签页**。
9. **标尺 / 参考线**：单位 px/cm/mm/inch/pt（按 DPI）；拖出在**尺子**、移动/删除在**画布**。
10. **另存为 ICO**：`≤512` 硬上限，**超限拒绝**（不默默缩小）；**不用 `QImageWriter`** ——
    实测 qico 插件会把 300×300 **静默缩到 256×256**（返回 true、`errorString()` 还报成功）。

## ★★ 自检节奏（用户明确要求，2026-09-21，务必遵守）★★
**改一个功能不要跑全量！** 全量 19 程序 ≈ 6 分钟，会把功能推进整个卡死。
- 改一处 → **只跑相关短名**（`./run_tests.sh ico` / `actions` / `geometry` …），秒级～十秒级。
- 只有这三种时机才跑全量：① **一个阶段收口**；② 改动**跨模块 / 动了公共头**；
  ③ 用户主动要求。否则**由我攒着，到点提醒用户**跑一次全量。
- ★ 推论：CMake `--clean-first` 同属"慢关卡"，也别每改一处就跑；与全量自检同频（收口时一起）。
- ★ 单测通过 ≠ 可提交，但**不必**用全量去补这个差 —— 收口那次全量兜底即可。

## 现状
`src/` = `{compat, core, engine, commands, tools, ui}` + `main.cpp`；全 UTF-8 BOM；**双构建 0 错 0 警**。
自检 **2515 项**（`./run_tests.sh [短名]`，`mainwin` 最慢 ~2.5min）；另有不编译的 `action_check.py`。
`pro_check.py` 现 **66 cpp / 65 h**。
★ 刻意不链：`layer_panel`→`commands/`；`texttool`/`text`/`handzoom`/`ruler`/`ico`→`ui/`；`tabs` 只链 `ui/DocumentTabs`。
★ **防假绿**：脚本**不删旧产物**，改**比对 mtime 是否变新**。
★ 截图 `shot.exe <图> <out.png> <模式>` → `.buildlog/*.png`（`layers_panel/layers/fill/shape*/handzoom*/
text*/openaslayer*/multitab*/ruler*/ico*`）；★ 它输出**混编码**（`const char*` UTF-8、`qPrintable()` GBK）→ **逐行**试 utf-8 再回落 gb18030。
★ 植入脚本 `.buildlog/implant_*.py`；"解析不到汇总行"也要算**抓住了**（进程可能已段错误）。

## 协作
中文；**迭代式**（先规划/初稿 → 逐步具体化）。会追问"XX 设计了吗" → **答前必须实际检索文档**。
决策后要把「待确认」改成「已定案」并展开成实现规格。

## 阶段
骨架 ✅ → 画笔与撤销 ✅ → 选区 + 剪贴板 ✅ → 调整与滤镜 ✅ → 多图层 ✅ → **绘制工具 ✅（14 个全上线）** →
跨图导入 ✅ → 多标签页 ✅ → **标尺 / 参考线 ✅** → **另存为 ICO ✅** →
打磨打包 🔄（图标体系 ✅ 78 张；剩**网格对齐吸附**、导航器面板、新建文档对话框、打包）
