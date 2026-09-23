// ============================================================================
//  magic_wand_test.cpp —— 魔术棒的离线自检
// ----------------------------------------------------------------------------
//  engine/Fill 那一组（tests/fill_test.cpp）已经把**算法**钉住了：
//  扫描线连通性、容差刻度、透明与不透明黑的区分、边缘过渡带。
//  魔术棒没有引入任何新算法 —— 它就是"把那张蒙版接到选区上"。
//  所以本组要盯的**全部**是"接错了"这一类错误：
//
//    ① **判据取错了图**（★ 本组最核心的一条）
//       油漆桶必须取活动图层，吸管必须取合成图，魔术棒可以选（默认合成图）。
//       取反了不会报错、不会崩，表现只是"选出来的东西和眼睛看到的对不上"。
//       唯一能一眼看出取哪张图的构造是：**活动图层整层全透明、下层左右两半不同色**。
//       取合成图 → 只选中左半边；取活动图层 → 整张画布（因为整层是同一种透明色）。
//
//    ② **容差的边界判据写歪**（差一个等号）
//       Fill 的口径是 `d <= tolerance` 才算区域内。写成 `<` 的话
//       "容差 30 选中灰阶差 30 的邻块"就不成立 —— 而且只在**恰好等于**
//       的时候才暴露，用户几乎不可能报出来。
//       本组用三段灰阶（100 / 110 / 130）与容差 0 / 10 / 29 / 30 钉住它。
//
//    ③ **第一次点击无效**（漏了 prepareCanvas）
//       新文档的选区蒙版是**空**的，而 Selection::applyMask() 在拿不到
//       "尺寸参考"时会静默 return。症状是"刚打开的图片上点魔术棒毫无反应"，
//       没有报错、日志干净。本组第一条断言就是"第一次点击必须建立选区"。
//
//    ④ **退化状态没归一化**
//       "减去"或"交集"把选区清空时，会留下一份**全 0 的活动蒙版**。
//       语义上什么都没选中，但 isActive() 返回 true → 之后每次写像素
//       都要白算一遍逐像素乘法。本组断言"清空后 isActive() == false"。
//
//    ⑤ **羽化顺序反了**
//       必须先羽化"新选中的那块"再与旧选区合入。反过来做会把**老边界**
//       也糊一遍，用户每做一次"添加"旧边界就更糊一层。
//       本组的判据是：添加之后，**老边界那一侧仍然精确等于 255**。
//
//    ⑥ **与油漆桶的口径漂移**
//       两个工具共用 engine/Fill 的内核，在单图层文档上"同样的参数应当
//       选中同一片"。这条断言是防止有人日后给魔术棒另写一份判据。
//
//  ★ 关于画布与配色 ★
//    一律用不透明灰阶：灰阶上色差与灰阶差是 1:1（见 engine/Fill.h），
//    于是"容差 30 能不能吃掉灰阶差 30"这类断言可以手算验证。
//    需要造透明像素时直接写**预乘值**（qRgba(0,0,0,0) 就是全透明），
//    不用 setPixel 绕一圈（那会隐式预乘/反预乘，量到的不是写进去的值）。
//
//  ★ 抗锯齿默认关掉 ★
//    除了专门测抗锯齿那一组，其余全部 setAntialias(false)。开着的话
//    扩展判据会变成 `d <= tolerance + 32`，过渡带的灰阶值会混进
//    "选中/没选中"的断言里，把手算变得没有意义。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 magicwand 分支。需要 QApplication（ToolOptions.cpp
//    与工具都链 QtWidgets），用 offscreen 插件在无显示器环境里跑。
// ============================================================================

#include "tools/MagicWandTool.h"
#include "tools/BucketTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "commands/CommandStack.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QtGlobal>

#include <cstdio>

using namespace zl;

static int g_failed = 0;
static int g_passed = 0;
static int g_group  = 0;

static void beginGroup(const char *name)
{
    ++g_group;
    std::printf("[%d] %s\n", g_group, name);
    std::fflush(stdout);
}

static void check(bool ok, const char *what)
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s\n", what);
    }
}

static void checkEqInt(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkRect(const QRect &actual, const QRect &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 (%d,%d %d×%d)，实际 (%d,%d %d×%d)\n", what,
                    expected.x(), expected.y(), expected.width(), expected.height(),
                    actual.x(), actual.y(), actual.width(), actual.height());
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  ★ 一律走扫描线，不用 QImage::pixel() / setPixel() ★
//    setPixel() 对预乘格式会"先预乘再写"，pixel() 又会"反预乘再读"，
//    两次隐式转换叠在一起，量到的就不是我们写进去的那个值了。
// ---------------------------------------------------------------------------
static QRgb rawPixel(const QImage &img, int x, int y)
{
    return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
}

static void rawSet(QImage &img, int x, int y, QRgb c)
{
    reinterpret_cast<QRgb *>(img.scanLine(y))[x] = c;
}

static void rawFillRect(QImage &img, const QRect &r, QRgb c)
{
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            rawSet(img, x, y, c);
}

static void layerFillRect(Document *doc, int layerIndex, const QRect &r, QRgb c)
{
    Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return;
    rawFillRect(l->mutableImage(), r, c);
}

// ---------------------------------------------------------------------------
//  构造辅助
// ---------------------------------------------------------------------------

static const int kW = 64;
static const int kH = 48;

// 一张单图层文档，图层底色为 fill（**直接写预乘字节**）
static Document *makeDoc(QRgb fill)
{
    auto *doc = new Document(QSize(kW, kH), Qt::transparent);
    Layer *l = doc->activeLayer();
    if (l) {
        QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        rawFillRect(img, QRect(0, 0, kW, kH), fill);
        l->setImage(img);
        doc->markDirty();
    }
    return doc;
}

static const QRgb kGray100 = qRgba(100, 100, 100, 255);
static const QRgb kGray110 = qRgba(110, 110, 110, 255);
static const QRgb kGray130 = qRgba(130, 130, 130, 255);
static const QRgb kGray200 = qRgba(200, 200, 200, 255);

// 左右两半：左 gray100、右 gray200（灰阶差 100，手算无歧义）
static Document *makeTwoStep()
{
    Document *doc = makeDoc(kGray100);
    layerFillRect(doc, 0, QRect(kW / 2, 0, kW - kW / 2, kH), kGray200);
    doc->markDirty();
    return doc;
}

// 三段： [0,20) gray100 ／ [20,42) gray110 ／ [42,64) gray130
// 与落点 gray100 的灰阶差分别是 0 / 10 / 30。
// ★ 30 这个值是为了钉住 `<=`：容差 30 必须选中它，容差 29 必须选不中 ★
static Document *makeThreeStep()
{
    Document *doc = makeDoc(kGray100);
    layerFillRect(doc, 0, QRect(20, 0, 22, kH), kGray110);
    layerFillRect(doc, 0, QRect(42, 0, 22, kH), kGray130);
    doc->markDirty();
    return doc;
}

// 灰 100 底 + 两块互不相连的灰 200 方块（A: x=4..19，B: x=44..59，均 y=4..19）
static Document *makeTwoIslands()
{
    Document *doc = makeDoc(kGray100);
    layerFillRect(doc, 0, QRect(4, 4, 16, 16), kGray200);
    layerFillRect(doc, 0, QRect(44, 4, 16, 16), kGray200);
    doc->markDirty();
    return doc;
}

// ---------------------------------------------------------------------------
//  参数与选区读取
// ---------------------------------------------------------------------------

// 把魔术棒的参数配成"判据最干脆"的一档：容差 0、无抗锯齿、连续、
// 取样合成图、不羽化、新建模式。这样"哪些像素被选中"完全由像素值决定。
static MagicWandOptions *prepare(MagicWandTool &tool,
                                 int tolerance = 0,
                                 bool contiguous = true,
                                 bool antialias = false,
                                 bool allLayers = true)
{
    auto *opts = qobject_cast<MagicWandOptions *>(tool.options());
    if (!opts)
        return nullptr;
    opts->setTolerance(tolerance);
    opts->setContiguous(contiguous);
    opts->setAntialias(antialias);
    opts->setSampleAllLayers(allLayers);
    opts->setFeatherRadius(0.0);
    opts->setMode(SelectionMode::Replace);
    return opts;
}

// 取选区在 (x,y) 处的覆盖度。
//
// ★ "没有选区" = 全选 = 处处 255 ★
//   这是本工程的核心语义（见 core/Selection.h）：蒙版为空不是"什么都没选"，
//   而是"没有限制"。若这里返回 0，所有断言的期望值都要反着写，极易看错。
static int selValue(const Document *doc, int x, int y)
{
    const Selection &sel = doc->selection();
    if (!sel.isActive() || sel.mask().isNull())
        return 255;

    const QImage &m = sel.mask();
    if (x < 0 || y < 0 || x >= m.width() || y >= m.height())
        return 0;
    return int(m.constScanLine(y)[x]);
}

// ===========================================================================
//  [1] 身份证与"第一次点击必须有效"
// ===========================================================================
static void testIdentity()
{
    beginGroup("身份证 / 第一次点击");

    Document *doc = makeDoc(kGray100);
    CommandStack stack(doc);

    MagicWandTool tool(doc, &stack);
    check(tool.id() == QStringLiteral("magicwand"),
          "id 是 magicwand（QSettings 键名 + iconForTool 映射键，定了就不能改）");
    check(tool.shortcutKey() == Qt::Key_W, "快捷键是 W（对齐 Photoshop）");
    check(!tool.displayName().isEmpty(), "有显示名");
    check(qobject_cast<MagicWandOptions *>(tool.options()) != nullptr,
          "参数对象就是 MagicWandOptions");

    // ★ 第一次点击（新文档、选区蒙版还是空的）★
    //   漏了 prepareCanvas() 的话这一条会红：applyMask 会因为拿不到
    //   尺寸参考而静默返回，选区仍是「全选」。
    auto *opts = prepare(tool);
    check(opts != nullptr, "拿得到参数对象");
    const bool ok = tool.selectAt(QPoint(10, 10));
    check(ok, "第一次点击建立了选区");
    check(doc->selection().isActive(), "选区处于「有选区」状态");
    checkEqInt(selValue(doc, 10, 10), 255, "落点像素被选中");
    check(doc->selection().mask().size() == QSize(kW, kH),
          "选区蒙版尺寸与画布一致（不一致的话后续所有操作都会被静默拒绝）");
    check(doc->selection().mask().format() == QImage::Format_Grayscale8,
          "选区蒙版是 Grayscale8");

    // 没有撤销栈时直接拒绝，且**不能**留下痕迹
    {
        Document *doc2 = makeDoc(kGray100);
        MagicWandTool noStack(doc2, nullptr);
        prepare(noStack);
        check(noStack.selectAt(QPoint(10, 10)) == false,
              "没有撤销栈时拒绝执行");
        check(!doc2->selection().isActive(),
              "拒绝时没有改动选区（不先改后说）");
        delete doc2;
    }

    delete doc;
}

// ===========================================================================
//  [2] 判据取哪张图（★ 本组最核心）
// ===========================================================================
static void testCriterionSource()
{
    beginGroup("判据取哪张图：取样合成图 vs 只取样当前图层");

    // 下层：左半纯红、右半纯蓝（全不透明）。
    // 上层：**整层全透明**，并设为活动图层。
    //   ★ 这个构造是唯一能一眼看出"取哪张图"的 ★
    //   活动图层整层是同一种颜色（预乘后 (0,0,0,0)），于是"只取样当前图层"
    //   会把**整张画布**选进来，而"取样合成图"只选中用户看到的那半边红色。
    Document *doc = makeDoc(qRgba(255, 0, 0, 255));
    layerFillRect(doc, 0, QRect(kW / 2, 0, kW - kW / 2, kH), qRgba(0, 0, 255, 255));

    const int top = doc->addLayer(QStringLiteral("全透明上层"));
    check(top > 0, "成功加了一层");
    layerFillRect(doc, top, QRect(0, 0, kW, kH), qRgba(0, 0, 0, 0));
    doc->setActiveLayerIndex(top);
    doc->markDirty();

    CommandStack stack(doc);
    MagicWandTool tool(doc, &stack);
    auto *opts = prepare(tool);

    // ── 默认：取样合成图（所见即所得）──
    opts->setSampleAllLayers(true);
    check(tool.selectAt(QPoint(10, 20)), "默认（对所有图层取样）点左半边，建立了选区");
    checkEqInt(selValue(doc, 10, 20), 255, "左半边（红色）被选中");
    checkEqInt(selValue(doc, 50, 20), 0, "右半边（蓝色）没被选中");
    checkRect(doc->selection().bounds(), QRect(0, 0, kW / 2, kH),
              "选区包围盒就是左半边");

    // ── 关掉开关：只取样当前图层（该层整层全透明）──
    doc->selection().clear();
    opts->setSampleAllLayers(false);
    check(tool.selectAt(QPoint(10, 20)), "只取样当前图层时也能建立选区");
    checkEqInt(selValue(doc, 50, 20), 255,
               "整层全透明 → 整张画布同色 → 统统被选中（这正是「只看当前层」的语义）");
    checkRect(doc->selection().bounds(), QRect(0, 0, kW, kH),
              "包围盒是整张画布");

    delete doc;
}

// ===========================================================================
//  [3] 容差：钉住 `d <= tolerance` 这个边界
// ===========================================================================
static void testTolerance()
{
    beginGroup("容差（含恰好相等的那一档）");

    struct Case { int tolerance; int expectedWidth; const char *what; };
    // 三段灰阶与落点（gray100）的差是 0 / 10 / 30。
    // Fill 的口径是 `d <= tolerance`，所以：
    //   容差 0  → 只有第 1 段（宽 20）
    //   容差 10 → 第 1、2 段（宽 42）
    //   容差 29 → 仍然只有第 1、2 段（30 > 29）
    //   容差 30 → 三段全中（宽 64）★ 这一档专门抓「写成 < 的差一个等号」★
    const Case cases[] = {
        {  0, 20, "容差 0 只选中完全同色的第 1 段" },
        { 10, 42, "容差 10 一并选中灰阶差 10 的第 2 段" },
        { 29, 42, "容差 29 选不中灰阶差 30 的第 3 段（d <= tol 的严格侧）" },
        { 30, 64, "容差 30 选中灰阶差 30 的第 3 段（恰好相等必须算选中）" },
        {255, 64, "容差 255 选中整张画布" },
    };

    for (const Case &c : cases) {
        Document *doc = makeThreeStep();
        CommandStack stack(doc);
        MagicWandTool tool(doc, &stack);
        prepare(tool, c.tolerance);

        check(tool.selectAt(QPoint(5, 10)), c.what);
        checkRect(doc->selection().bounds(), QRect(0, 0, c.expectedWidth, kH), c.what);
        delete doc;
    }
}

// ===========================================================================
//  [4] 连通 / 非连通
// ===========================================================================
static void testContiguous()
{
    beginGroup("连通（连续）");

    // 两块同色、互不相连的方块，背景是第三种灰。
    //   连续   → 只选中落点所在的那一块
    //   非连续 → 两块都选中（Photoshop 的"取消勾选连续"就是这个行为）
    {
        Document *doc = makeTwoIslands();
        CommandStack stack(doc);
        MagicWandTool tool(doc, &stack);
        prepare(tool, 0, /*contiguous*/ true);

        check(tool.selectAt(QPoint(10, 10)), "连续模式：点进方块 A");
        checkRect(doc->selection().bounds(), QRect(4, 4, 16, 16),
                  "只选中方块 A（16×16）");
        delete doc;
    }
    {
        Document *doc = makeTwoIslands();
        CommandStack stack(doc);
        MagicWandTool tool(doc, &stack);
        prepare(tool, 0, /*contiguous*/ false);

        check(tool.selectAt(QPoint(10, 10)), "非连续模式：点进方块 A");
        checkRect(doc->selection().bounds(), QRect(4, 4, 56, 16),
                  "方块 A 与远处的方块 B 都被选中（x 从 4 到 59）");
        delete doc;
    }
}

// ===========================================================================
//  [5] 四种布尔运算
// ===========================================================================
static void testBooleanOps()
{
    beginGroup("四种运算：新建 / 添加 / 减去 / 交叉");

    // 左半 gray100（记作 A，x=0..31）、右半 gray200（记作 B，x=32..63）。
    Document *doc = makeTwoStep();
    CommandStack stack(doc);
    MagicWandTool tool(doc, &stack);
    auto *opts = prepare(tool);          // 容差 0、连续、无抗锯齿

    const QPoint inA(5, 20);
    const QPoint inB(40, 20);

    // ── 新建（选项条上的模式）──
    opts->setMode(SelectionMode::Replace);
    check(tool.selectAt(inA), "新建：点 A");
    checkEqInt(selValue(doc, 5, 20), 255, "A 被选中");
    checkEqInt(selValue(doc, 40, 20), 0, "B 没被选中");
    checkRect(doc->selection().bounds(), QRect(0, 0, 32, kH), "选区 = A");

    // ── 添加（选项条上的模式）──
    opts->setMode(SelectionMode::Add);
    check(tool.selectAt(inB), "添加：点 B");
    checkEqInt(selValue(doc, 5, 20), 255, "A 仍在");
    checkEqInt(selValue(doc, 40, 20), 255, "B 并了进来");
    checkRect(doc->selection().bounds(), QRect(0, 0, kW, kH), "选区 = A ∪ B");

    // ── 交叉（选项条上的模式）──
    opts->setMode(SelectionMode::Intersect);
    check(tool.selectAt(inA), "交叉：点 A");
    checkEqInt(selValue(doc, 5, 20), 255, "A 保留（A ∩ (A ∪ B) = A）");
    checkEqInt(selValue(doc, 40, 20), 0, "B 被切掉");
    checkRect(doc->selection().bounds(), QRect(0, 0, 32, kH), "选区回到 A");

    // ── 减去（选项条上的模式；先把 B 并回来，好让"减去"留下非空结果）──
    //   ★ 这里刻意**不**做"把选区减到空"的那一步 ★
    //     减空会触发退化归一化 → 文档变成"没有选区"，而"没有选区 == 全选"
    //     的语义会被 selValue() 折算成 255 → 断言看到 255，看起来像"根本没减掉"。
    //     那条边界归 [6] 组（退化归一化）管；修饰键那条通路归 [8] 组管。
    opts->setMode(SelectionMode::Add);
    check(tool.selectAt(inB), "减去之前先把 B 并进来");
    checkRect(doc->selection().bounds(), QRect(0, 0, kW, kH), "选区 = A ∪ B");

    opts->setMode(SelectionMode::Subtract);
    check(tool.selectAt(inA), "减去：点 A");
    checkEqInt(selValue(doc, 5, 20), 0, "A 被挖掉");
    checkEqInt(selValue(doc, 40, 20), 255, "B 留下来");
    checkRect(doc->selection().bounds(), QRect(32, 0, 32, kH), "选区 = B");

    delete doc;
}

// ===========================================================================
//  [6] 退化归一化 + "全选打底"的语义
// ===========================================================================
static void testDegenerate()
{
    beginGroup("退化情况：全选打底 / 清空即归一化");

    Document *doc = makeTwoStep();
    CommandStack stack(doc);
    MagicWandTool tool(doc, &stack);
    auto *opts = prepare(tool);

    // ── "没有选区" = 全选 = 蒙版处处 255，于是"减去"得到补集 ──
    //   这条同时验证了 prepareCanvas() 用 255 打底的语义：
    //   若打的是 0，从全选里减去一块会得到"什么都没有"。
    check(!doc->selection().isActive(), "起始状态：没有选区（= 全选）");
    opts->setMode(SelectionMode::Subtract);
    check(tool.selectAt(QPoint(40, 20)), "减去：从全选里挖掉 B");
    checkEqInt(selValue(doc, 5, 20), 255, "A 一侧仍是全选（255）");
    checkEqInt(selValue(doc, 40, 20), 0, "B 被挖空");
    checkRect(doc->selection().bounds(), QRect(0, 0, 32, kH),
              "非零区域就是 A（包围盒 32×48）");

    // ── 再挖掉 A → 选区全空 → 必须归一化回"没有选区" ──
    //   ★ 这一条盯的是**性能退化**：全 0 的活动蒙版会让 isActive() 返回 true，
    //     所有工具在写像素前都要白走一遍逐像素乘法，而结果永远是"什么都不写"。
    check(tool.selectAt(QPoint(5, 20)), "再减去 A");
    check(!doc->selection().isActive(),
          "选区被清空后回到「没有选区」（isActive() == false，快路径恢复）");
    checkEqInt(selValue(doc, 40, 20), 255, "此时语义上又是全选");

    // ── 交集在"全选"上的行为：全选 ∩ X = X ──
    opts->setMode(SelectionMode::Intersect);
    check(tool.selectAt(QPoint(40, 20)), "交叉：全选 ∩ B");
    checkRect(doc->selection().bounds(), QRect(32, 0, 32, kH), "结果就是 B 本身");

    delete doc;
}

// ===========================================================================
//  [7] 撤销粒度与逐条还原
// ===========================================================================
static void testUndoGranularity()
{
    beginGroup("撤销粒度：一次点击 = 一条记录");

    Document *doc = makeTwoStep();
    CommandStack stack(doc);
    MagicWandTool tool(doc, &stack);
    prepare(tool);

    check(tool.selectAt(QPoint(5, 20)), "第一次点击（选中 A）");
    checkEqInt(stack.undoStack()->count(), 1, "一次点击 = 一条撤销记录");

    check(tool.selectAt(QPoint(40, 20)), "第二次点击（替换成 B）");
    checkEqInt(stack.undoStack()->count(), 2, "两次点击 = 两条撤销记录（没有被合并）");
    checkEqInt(selValue(doc, 5, 20), 0, "A 已不在选区里");
    checkEqInt(selValue(doc, 40, 20), 255, "B 在选区里");

    stack.undoStack()->undo();
    checkEqInt(selValue(doc, 5, 20), 255, "撤销一步 → 回到 A");
    checkEqInt(selValue(doc, 40, 20), 0, "撤销一步 → B 退出选区");
    stack.undoStack()->undo();
    check(!doc->selection().isActive(), "撤销两步 → 回到「没有选区」");

    stack.undoStack()->redo();
    checkEqInt(selValue(doc, 5, 20), 255, "重做一步 → A 回来");
    stack.undoStack()->redo();
    checkEqInt(selValue(doc, 40, 20), 255, "重做两步 → B 回来");

    // 点击画布外：不产生任何记录（手抖点偏是常事，不该污染撤销栈）
    const int before = stack.undoStack()->count();
    check(tool.selectAt(QPoint(-1, 5)) == false, "画布左侧外：拒绝");
    check(tool.selectAt(QPoint(kW, 5)) == false, "画布右侧外：拒绝");
    check(tool.selectAt(QPoint(5, kH)) == false, "画布下方外：拒绝");
    checkEqInt(stack.undoStack()->count(), before, "越界点击没有留下任何记录");

    delete doc;
}

// ===========================================================================
//  [8] 修饰键覆盖选项条上的模式
// ===========================================================================
static void testModifierOverride()
{
    beginGroup("修饰键：Shift=添加 / Alt=减去 / Shift+Alt=交叉");

    Document *doc = makeTwoStep();
    CommandStack stack(doc);
    MagicWandTool tool(doc, &stack);
    auto *opts = prepare(tool);
    opts->setMode(SelectionMode::Replace);      // 选项条上一律「新建」

    const QPoint inA(5, 20);
    const QPoint inB(40, 20);

    check(tool.selectAt(inA), "先选中 A");

    ToolModifiers shift; shift.shift = true;
    check(tool.selectAt(inB, shift), "Shift + 点 B");
    checkEqInt(selValue(doc, 40, 20), 255, "Shift 起了「添加」的作用（B 并了进来）");

    ToolModifiers alt; alt.alt = true;
    check(tool.selectAt(inA, alt), "Alt + 点 A");
    checkEqInt(selValue(doc, 5, 20), 0, "Alt 起了「减去」的作用（A 被挖掉）");

    ToolModifiers both; both.shift = true; both.alt = true;
    check(tool.selectAt(inB, both), "Shift+Alt + 点 B");
    checkEqInt(selValue(doc, 40, 20), 255, "交集：B ∩ B = B，仍然选中");
    checkRect(doc->selection().bounds(), QRect(32, 0, 32, kH), "选区只剩 B");

    delete doc;
}

// ===========================================================================
//  [9] 与油漆桶**有意不同**的一条：不检查图层可写性
// ===========================================================================
static void testNoWriteCheck()
{
    beginGroup("不检查图层可写性（与油漆桶的有意差异）");

    // 油漆桶会拦"活动图层不可写"，因为它是往图层里写像素的。
    // 魔术棒只改选区（文档级元数据），隐藏/锁定活动图层**不该**拦住它 ——
    // 否则用户会遇到"隐藏了图层之后连选区都拉不出来"。
    Document *doc = makeTwoStep();

    // 上面再加一层（隐藏的），并设为活动图层
    const int top = doc->addLayer(QStringLiteral("隐藏层"));
    layerFillRect(doc, top, QRect(0, 0, kW, kH), kGray130);
    Layer *topLayer = doc->layers().layer(top);
    check(topLayer != nullptr, "拿得到上面那一层");
    topLayer->setVisible(false);
    doc->setActiveLayerIndex(top);
    // ★ 改了图层属性必须让合成缓存失效 ★
    //   否则 composite() 可能返回"隐藏之前"的旧缓存，本组后面的断言
    //   会在一个错误的画面上做判断（而且看起来全对）。
    doc->markDirty();

    // 对照：油漆桶在同样的情况下必须拒绝
    {
        CommandStack stack(doc);
        BucketTool bucket(doc, &stack);
        check(bucket.fillAt(QPoint(10, 20)) == false,
              "对照：活动图层隐藏时油漆桶拒绝（它要写像素）");
        checkEqInt(stack.undoStack()->count(), 0, "油漆桶没有留下记录");
    }

    // 魔术棒：仍然能建选区
    {
        CommandStack stack(doc);
        MagicWandTool tool(doc, &stack);
        prepare(tool, 0, true, false, /*allLayers*/ true);
        check(tool.selectAt(QPoint(10, 20)),
              "活动图层隐藏时魔术棒仍然建立选区（它不写像素）");
        checkEqInt(stack.undoStack()->count(), 1, "压了一条选区命令");
        checkEqInt(selValue(doc, 10, 20), 255, "A 被选中");

        // ★ 顺带钉住"合成图不含隐藏图层"★
        //   隐藏层是 gray130；它若被算进合成图，左半（gray100）的连通范围
        //   与右半的判定都会变。这里断言右半确实没被选中 ——
        //   说明判定是按**可见内容**走的。
        checkEqInt(selValue(doc, 40, 20), 0,
                   "右半没被选中 → 合成图确实跳过了隐藏层");
    }

    delete doc;
}

// ===========================================================================
//  [10] 抗锯齿：过渡带的确切灰阶值
// ===========================================================================
static void testAntialias()
{
    beginGroup("抗锯齿：过渡带按色差线性衰减");

    // 关掉抗锯齿时，只有色差为 0 的像素进入选区。
    {
        Document *doc = makeThreeStep();
        CommandStack stack(doc);
        MagicWandTool tool(doc, &stack);
        prepare(tool, 0, true, /*antialias*/ false);

        check(tool.selectAt(QPoint(5, 10)), "无抗锯齿：点第 1 段");
        checkEqInt(selValue(doc, 25, 10), 0, "灰阶差 10 的第 2 段完全不在选区里（0）");
        checkEqInt(selValue(doc, 50, 10), 0, "灰阶差 30 的第 3 段也不在（0）");
        checkRect(doc->selection().bounds(), QRect(0, 0, 20, kH), "包围盒只有第 1 段");
        delete doc;
    }

    // 打开抗锯齿：扩展判据变成 d <= 0 + softBand(32)，
    // 覆盖度按 (limit - d) / band 线性衰减 —— 手算如下：
    //   第 1 段 d=0  → 255
    //   第 2 段 d=10 → qRound(255 * 22 / 32) = qRound(175.31) = 175
    //   第 3 段 d=30 → qRound(255 *  2 / 32) = qRound(15.94)  =  16
    // ★ 这三个值是"实现契约"而不是"算法细节" ★
    //   灰阶上色差与灰阶差是 1:1（Fill.h），所以 10 与 30 是精确值，
    //   除法也没有浮点不确定性 —— 可以要求逐位相同，不需要放容差。
    {
        Document *doc = makeThreeStep();
        CommandStack stack(doc);
        MagicWandTool tool(doc, &stack);
        prepare(tool, 0, true, /*antialias*/ true);

        check(tool.selectAt(QPoint(5, 10)), "有抗锯齿：点第 1 段");
        checkEqInt(selValue(doc, 5, 10), 255, "种子所在段：完全选中（255）");
        checkEqInt(selValue(doc, 25, 10), 175, "灰阶差 10 的邻段：覆盖度 175（手算）");
        checkEqInt(selValue(doc, 50, 10), 16, "灰阶差 30 的远段：覆盖度 16（手算）");
        checkRect(doc->selection().bounds(), QRect(0, 0, kW, kH),
                  "三段都在过渡带范围内 → 包围盒是整张画布");
        delete doc;
    }
}

// ===========================================================================
//  [11] 羽化：顺序必须是"先羽化新蒙版，再合入"
// ===========================================================================
static void testFeatherOrder()
{
    beginGroup("羽化：先羽化新蒙版再合入（老边界不能被糊）");

    Document *doc = makeTwoStep();
    CommandStack stack(doc);
    MagicWandTool tool(doc, &stack);
    auto *opts = prepare(tool);

    // 第 1 步：不羽化，选中 A（硬边）→ 边界在 x=31 / x=32 之间
    check(tool.selectAt(QPoint(5, 20)), "先硬边选中 A");
    checkEqInt(selValue(doc, 31, 20), 255, "A 的最右一列被选中");
    checkEqInt(selValue(doc, 32, 20), 0, "B 的最左一列没被选中");

    // 第 2 步：改成"添加 + 羽化 3px"，再点 B
    opts->setMode(SelectionMode::Add);
    opts->setFeatherRadius(3.0);
    check(tool.selectAt(QPoint(40, 20)), "羽化 3px 添加 B");

    // ★ 关键判据 ★
    //   老边界那一侧（x=31）必须**精确**等于 255：
    //   新蒙版在 x=31 处被羽化成了某个中间值，Add 取 max(255, v) = 255。
    //   如果实现写成"先合入、再对最终选区羽化"，x=31 会被模糊到 255 以下 ——
    //   这一条就是抓那个顺序错误的。
    checkEqInt(selValue(doc, 31, 20), 255,
               "老边界（x=31）仍然是精确的 255 → 羽化只作用在新蒙版上");

    //   新边界那一侧（从 x=32 开始）必须是软的
    const int v32 = selValue(doc, 32, 20);
    check(v32 > 0 && v32 < 255,
          "新边界（x=32）是 1~254 的过渡值 → 新蒙版确实被羽化了");

    //   远离边界的内部不受羽化影响
    checkEqInt(selValue(doc, 5, 20), 255, "A 的内部仍是 255");
    checkEqInt(selValue(doc, 60, 20), 255, "B 的内部仍是 255");

    delete doc;
}

// ===========================================================================
//  [12] 与油漆桶的口径一致（同一个内核）
// ===========================================================================
static void testSameKernelAsBucket()
{
    beginGroup("与油漆桶同源：同样的参数选中同一片");

    // ★ 这条断言的用意是"防止日后分家" ★
    //   两个工具共用 engine/Fill 的 floodFill()，在单图层文档上
    //   （两者的判据图相同）同样的参数必须得到同一片区域。
    //   哪天有人给魔术棒另写一份判据，这条会立刻红。
    //   只比包围盒：那已经足够抓住"判据/连通性/容差口径"的漂移，
    //   又不依赖两个工具内部各自怎么用蒙版。
    struct Case { int tolerance; bool contiguous; const char *what; };
    const Case cases[] = {
        {  0, true,  "容差 0 + 连续" },
        { 10, true,  "容差 10 + 连续" },
        { 30, true,  "容差 30 + 连续（恰好吃掉第三段）" },
        {  0, false, "容差 0 + 非连续" },
    };

    for (const Case &c : cases) {
        const QPoint seed(5, 10);

        // ── 油漆桶：填完之后，"不再是那三种灰"的像素就是被填过的地方 ──
        QRect bucketBounds;
        {
            Document *doc = makeThreeStep();
            CommandStack stack(doc);
            BucketTool bucket(doc, &stack);
            auto *bo = qobject_cast<BucketOptions *>(bucket.options());
            check(bo != nullptr, "拿得到油漆桶的参数对象");
            bo->setTolerance(c.tolerance);
            bo->setContiguous(c.contiguous);
            bo->setAntialias(false);
            bo->setOpacity(100);
            bo->setColor(QColor(0, 0, 0));

            check(bucket.fillAt(seed), c.what);

            const QImage &img = doc->layers().layer(0)->image();
            int minX = kW, minY = kH, maxX = -1, maxY = -1;
            for (int y = 0; y < kH; ++y) {
                for (int x = 0; x < kW; ++x) {
                    const QRgb px = rawPixel(img, x, y);
                    if (px == kGray100 || px == kGray110 || px == kGray130)
                        continue;               // 没被填过
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                }
            }
            bucketBounds = (maxX < 0) ? QRect()
                                      : QRect(QPoint(minX, minY), QPoint(maxX, maxY));
            delete doc;
        }

        // ── 魔术棒：同样参数下的选区包围盒 ──
        QRect wandBounds;
        {
            Document *doc = makeThreeStep();
            CommandStack stack(doc);
            MagicWandTool tool(doc, &stack);
            prepare(tool, c.tolerance, c.contiguous, /*antialias*/ false);
            check(tool.selectAt(seed), c.what);
            wandBounds = doc->selection().bounds();
            delete doc;
        }

        check(bucketBounds == wandBounds, c.what);
        if (bucketBounds != wandBounds) {
            std::printf("       油漆桶 (%d,%d %d×%d) vs 魔术棒 (%d,%d %d×%d)\n",
                        bucketBounds.x(), bucketBounds.y(),
                        bucketBounds.width(), bucketBounds.height(),
                        wandBounds.x(), wandBounds.y(),
                        wandBounds.width(), wandBounds.height());
        }
    }
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 魔术棒 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testIdentity();
    testCriterionSource();
    testTolerance();
    testContiguous();
    testBooleanOps();
    testDegenerate();
    testUndoGranularity();
    testModifierOverride();
    testNoWriteCheck();
    testAntialias();
    testFeatherOrder();
    testSameKernelAsBucket();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
