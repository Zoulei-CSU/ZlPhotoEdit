// ============================================================================
//  brush_tool_test.cpp —— 画笔 / 橡皮 的离线自检（tools 层）
// ----------------------------------------------------------------------------
//  为什么需要它（以及为什么 brush_test 不够）：
//
//    tests/brush_test.cpp 测的是 **engine/Brush.cpp** 那些纯函数 ——
//    "覆盖度蒙版对不对""预乘合成对不对"。它很扎实，但它看不见
//    BrushTool 这个类。而 BrushTool 恰恰是全工程**状态最复杂**的一个工具：
//
//      浅拷贝快照 → 累积蒙版 → 每帧从快照重算 → 松手打包一条命令
//                                      → 取消时把像素还回去
//
//    这条链上的每个环节错了，engine 层的断言都照样全绿。
//
//  ★★ 本组的头号目标：把"拖动期间到底重算了多少"钉死 ★★
//
//    画笔在 4K 图上拖动时每帧只应重算**笔尖扫过的那一小块**，
//    呈现层也只该重合成那一块。而"重算了多少"这件事：
//      · 从画面上**完全看不出来**（画得都对）；
//      · 从像素断言里**完全看不出来**（区域重算与整图重算结果一致）；
//      · 只有 Document 的遥测计数（fullCompositeCount / regionCompositeCount）
//        能回答。
//
//    实测踩过（批次一）：BrushTool::applyRegion() 里残留了一句
//    `document()->markDirty()`（全量失效）。像素级结果全对、取消也全对、
//    两个构建 0 错 0 警 —— 但呈现层每帧照样把整张画布重新合成一遍，
//    4K 画笔的卡顿（评估文档 §1.3）其实一点没改善。
//    这正是本组必须存在、且必须以**计数**为判据的原因。
//
//  组别：
//    [1] 落笔与幂等（引擎之外的"工具行为"）
//    [2] ★ 增量失效：拖动 N 帧 → 一次全图重算都不许有 ★
//    [3] 增量结果 == 全量结果（逐字节）—— 防止"只脏一小块"漏掉变化
//    [4] 撤销粒度：一整笔 = 一条命令；撤销后逐字节还原
//    [5] 越界不产生记录；取消（Esc）必须把像素还回去
//    [6] ★ 轮廓圆跟着鼠标走 —— **拖动过程中也必须跟** ★
//        （量的是 paintScreenOverlay 真正画出的像素，不是工具里的缓存）
//
//  ★ 关于配置 ★
//    硬化笔尖（hardness=100）、不透明色、抗锯齿由笔尖蒙版自身决定。
//    "中心必须完全上色""远处一个像素都不许动"是精确判定，
//    不必迁就像素中心的边界情形。
//
//  ★ 关于坐标 ★
//    事件里的 imagePos 直接就是图像坐标（ToolEvent 已把换算做完），
//    所以下面写的整数就是图像像素坐标。
//
//  编译： ./run_tests.sh brushtool
//  ★ 需要 Qt5Widgets ★ —— ToolOptions.cpp 要造控件（即使这里不调
//    createWidget，符号也会被引进来）。见 run_tests.sh 的说明。
// ============================================================================

#include "tools/BrushTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "commands/CommandStack.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include <cstdio>
#include <cstring>

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

static void checkRgb(QRgb actual, QRgb expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 a=%d r=%d g=%d b=%d，"
                    "实际 a=%d r=%d g=%d b=%d\n", what,
                    qAlpha(expected), qRed(expected), qGreen(expected), qBlue(expected),
                    qAlpha(actual),   qRed(actual),   qGreen(actual),   qBlue(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  像素读写（一律走扫描线，理由见 tool_paint_test）
// ---------------------------------------------------------------------------
static QRgb rawPixel(const QImage &img, int x, int y)
{
    return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
}

static void rawFillRect(QImage &img, const QRect &r, QRgb c)
{
    for (int y = r.top(); y <= r.bottom(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = r.left(); x <= r.right(); ++x)
            line[x] = c;
    }
}

// 两张同尺寸同格式的图是否逐字节相同。
static bool sameBytes(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull())
        return a.isNull() && b.isNull();
    if (a.size() != b.size() || a.format() != b.format())
        return false;

    const int bytes = a.width() * 4;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        size_t(bytes)) != 0)
            return false;
    }
    return true;
}

static const QRgb kWhite  = qRgba(255, 255, 255, 255);
static const QRgb kRed    = qRgba(255, 0, 0, 255);
static const QRgb kClear  = qRgba(0, 0, 0, 0);

// 画布尺寸：够大，让"一条 20 像素宽的笔画"在整图面前确实是**一小块**
// （增量失效的断言才有意义）；又够小，让整组跑得很快。
static const int kW = 256;
static const int kH = 192;

// 单图层文档，活动图层整层写 fill（**直接写预乘字节**，理由见 tool_paint_test：
// QImage::fill(QColor) 会按非预乘语义解释颜色，半透明色会写错）。
static Document *makeDoc(QRgb fill)
{
    auto *doc = new Document(QSize(kW, kH), Qt::transparent);
    Layer *l = doc->activeLayer();
    if (l) {
        QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        if (qAlpha(fill) != 0)
            rawFillRect(img, QRect(0, 0, kW, kH), fill);
        l->setImage(img);
        doc->markDirty();
    }
    return doc;
}

static const QImage &layerImage(const Document *doc, int index = 0)
{
    static const QImage kNull;
    const Layer *l = doc->layers().layer(index);
    return l ? l->image() : kNull;
}

// ---------------------------------------------------------------------------
//  事件构造
// ---------------------------------------------------------------------------
//  ★ Release 事件的 button 必须是左键、buttons 必须是空 ★
//   真实 Qt 事件里 button() 是"造成这次事件的键"、buttons() 是"此刻按着的"。
//   （完整记录见 tool_paint_test 的注释。）
static ToolEvent mouseEvent(ToolEventType type, int x, int y)
{
    ToolEvent e;
    e.type = type;
    e.button  = Qt::LeftButton;
    e.buttons = (type == ToolEventType::Release) ? Qt::NoButton : Qt::LeftButton;
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    return e;
}

// 悬停事件：button / buttons 都是空的。
// 真实 Qt 事件里"没按键的移动"就是这样 —— 画布据此把它分派给 hoverMove
// 而不是 mouseMove（见 CanvasView::mouseMoveEvent 的"区分悬停与拖动"）。
// 本组要断言"两条路都得让轮廓圆跟着走"，所以两种事件都得能造。
static ToolEvent hoverEvent(int x, int y)
{
    ToolEvent e;
    e.type = ToolEventType::Move;
    e.button  = Qt::NoButton;
    e.buttons = Qt::NoButton;
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    return e;
}

// ---------------------------------------------------------------------------
//  ★ 把屏幕叠加层画到一张内存图上，量出轮廓圆的外接盒中心 ★
// ---------------------------------------------------------------------------
//
//  ★ 为什么必须"量它画出来的像素"，而不是读工具里的成员 ★
//
//    轮廓圆是一条**自绘管线**：工具只是把 widgetPos 记下来，
//    真正决定"圆圈落在哪"的是 paintScreenOverlay 里那次 drawEllipse。
//    只断言"缓存里的坐标对了"，等于把渲染这一环整个让出去 ——
//    圆心算错、半径忘了乘缩放比、少了 resetTransform、
//    条件写成了恒假导致根本不画：全都照样全绿。
//    所以本组一律渲染后量像素（这也是本项目对自绘管线的统一纪律）。
//
//  返回 false 表示"一个像素都没画"（例如 m_hoverStale 导致早退，
//  或者圆被画出界只留下了半条边）。调用方必须单独断言这件事 ——
//  否则"圆心 = (0,0)"这种假通过会混进来。
static bool overlayCircleCenter(BrushTool *tool, const QSize &canvasSize,
                                QPointF *centerOut)
{
    QImage target(canvasSize, QImage::Format_ARGB32_Premultiplied);
    target.fill(Qt::transparent);

    {
        QPainter p(&target);
        // 与 CanvasView::paintEvent 的调用约定一致：叠加层在
        // resetTransform() 之后画，坐标系是**控件坐标**。
        p.resetTransform();
        // ★ 传空 view ★ —— 工具不该碰它（轮廓圆要的两个量都在事件里）。
        //   这条同时是一个契约断言：将来谁在 paintScreenOverlay 里
        //   解引用 view，这里会立刻崩，而不是静默画错。
        tool->paintScreenOverlay(p, nullptr);
    }

    int minX = canvasSize.width(),  minY = canvasSize.height();
    int maxX = -1,                   maxY = -1;

    for (int y = 0; y < target.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(target.constScanLine(y));
        for (int x = 0; x < target.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }

    // 没画出东西，或者只画出一条线（不成其为圆）
    if (maxX < 0 || minX == maxX || minY == maxY)
        return false;

    *centerOut = QPointF((minX + maxX) / 2.0, (minY + maxY) / 2.0);
    return true;
}

// 造一个画笔（或橡皮），硬化笔尖、不透明、指定颜色。
// 用完由调用方 delete（这里刻意不用智能指针，与其余工具测试保持一致）。
static BrushTool *makeTool(Document *doc, CommandStack *stack,
                           BrushTool::Kind kind, const QColor &color, int size)
{
    auto *tool = new BrushTool(kind, doc, stack);
    auto *opt = static_cast<BrushOptions *>(tool->options());
    if (opt) {
        opt->setSize(size);
        opt->setHardness(100);
        opt->setOpacity(100);
        opt->setFlow(100);
        opt->setColor(color);
    }
    return tool;
}

// ===========================================================================
//  [1] 落笔与幂等
// ===========================================================================
static void testStampAndIdempotence()
{
    beginGroup("落笔与幂等");

    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);
    BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush, Qt::red, 20);

    // ── 1.1 落笔即画一个点 ──
    //
    //   用户"单击一下"必须留下痕迹，而不是什么都没发生。
    //   实现上靠"press 时也走一次 from == to 的补点"。
    tool->mousePress(mouseEvent(ToolEventType::Press, 128, 96));
    checkRgb(rawPixel(layerImage(doc), 128, 96), kRed,
             "单击一下 → 笔尖中心完全上色");
    checkRgb(rawPixel(layerImage(doc), 5, 5), kClear,
             "远离笔尖的像素一个都不许动");

    // ── 1.2 ★ 幂等：同一个位置反复 move，结果不许变 ★ ──
    //
    //   ★ 为什么这条必须钉 ★
    //     按住鼠标不动时 Qt 仍会持续派发 move 事件（手抖）。
    //     如果实现是"在上一帧结果上继续叠加"，抖动就会让线条局部变深 ——
    //     用户看到的是"画着画着自己变黑了"，而所有"画对了吗"的断言全绿。
    //     本工程的做法是"每帧从落笔前快照重算"（见 BrushTool.h），
    //     这条断言就是那个约定的执行者。
    const QImage afterFirst = layerImage(doc).copy();
    for (int i = 0; i < 6; ++i)
        tool->mouseMove(mouseEvent(ToolEventType::Move, 128, 96));

    check(sameBytes(layerImage(doc), afterFirst),
          "★★ 同一位置反复 move 6 次 → 图层逐字节不变（幂等，不会越画越深）");

    tool->mouseRelease(mouseEvent(ToolEventType::Release, 128, 96));

    // ── 1.3 橡皮：把 alpha 擦掉 ──
    //
    //   ★ 这里只验"擦掉了"（alpha → 0），不验"擦到多干净" ★
    //     边缘的残留量与笔尖蒙版的形状有关，那属于 engine/brush_test 的范围。
    {
        Document *doc2 = makeDoc(kRed);
        CommandStack stack2(doc2);
        BrushTool *eraser = makeTool(doc2, &stack2, BrushTool::Kind::Eraser,
                                     Qt::black, 20);
        eraser->mousePress(mouseEvent(ToolEventType::Press, 100, 80));
        eraser->mouseRelease(mouseEvent(ToolEventType::Release, 100, 80));

        checkEqInt(qAlpha(rawPixel(layerImage(doc2), 100, 80)), 0,
                   "橡皮 → 笔尖中心 alpha 归零");
        checkEqInt(qAlpha(rawPixel(layerImage(doc2), 5, 5)), 255,
                   "橡皮 → 远处像素仍是原来的不透明红");
        delete eraser;
        delete doc2;
    }

    delete tool;
    delete doc;
}

// ===========================================================================
//  [2] ★ 增量失效：拖动 N 帧，一次全图重算都不许有 ★
// ===========================================================================
static void testIncrementalInvalidation()
{
    beginGroup("增量失效（遥测计数）");

    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);
    BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush, Qt::red, 20);

    // 先建一次合成缓存（真实路径上它由首次呈现建立）。
    doc->composite();
    const quint64 fullBefore   = doc->fullCompositeCount();
    const quint64 regionBefore = doc->regionCompositeCount();
    std::printf("     起手：全量 %llu 次，区域 %llu 次（画布 %dx%d）\n",
                static_cast<unsigned long long>(fullBefore),
                static_cast<unsigned long long>(regionBefore), kW, kH);

    // ── 2.1 拖动 6 帧，每帧都像真实呈现那样合成一次 ──
    tool->mousePress(mouseEvent(ToolEventType::Press, 30, 96));
    doc->composite();

    for (int x = 60; x <= 210; x += 30) {
        tool->mouseMove(mouseEvent(ToolEventType::Move, x, 96));
        doc->composite();
    }

    const quint64 fullAfter   = doc->fullCompositeCount();
    const quint64 regionAfter = doc->regionCompositeCount();
    std::printf("     拖完：全量 %llu 次（+%llu），区域 %llu 次（+%llu）\n",
                static_cast<unsigned long long>(fullAfter),
                static_cast<unsigned long long>(fullAfter - fullBefore),
                static_cast<unsigned long long>(regionAfter),
                static_cast<unsigned long long>(regionAfter - regionBefore));

    // ★ 这一条是本组存在的全部理由 ★
    checkEqInt(int(fullAfter - fullBefore), 0,
               "★★ 拖动期间一次全图重算都不许有（BrushTool 不再 markDirty 整图）");
    check(regionAfter - regionBefore >= 7,
          "★★ 每一帧都走了区域补丁快路径（全量+区域 = 帧数）");

    tool->mouseRelease(mouseEvent(ToolEventType::Release, 210, 96));

    // ── 2.2 松手压栈**也不算**全图失效 ──
    //
    //   DrawCommand 实现了 IRegionAware，CommandStack 会按它报的矩形失效。
    //   若哪天这条链断了（命令不再报区域），这里会立刻变红。
    const quint64 fullBeforeRelease = doc->fullCompositeCount();
    doc->composite();
    checkEqInt(int(doc->fullCompositeCount() - fullBeforeRelease), 0,
               "★ 松手压命令后的重合成也是区域的，不是全图");

    delete tool;
    delete doc;
}

// ===========================================================================
//  [3] 增量结果 == 全量结果（逐字节）
// ===========================================================================
//
//  ★ 这一条是"能省"与"省错了"的分界线 ★
//    前面所有断言都只说明"我们走了快路径"。快路径若贴错了区域
//    （比如少贴了笔尖边缘那一圈），画面会停在旧内容上 ——
//    不崩、不报错，要放大到特定比例才看得见。
//    判据必须是"同一份图层数据，走增量与走全量得到同一张图"。
// ===========================================================================
static void testRegionEqualsFull()
{
    beginGroup("增量结果 == 全量结果（逐字节）");

    Document *doc = makeDoc(kWhite);
    CommandStack stack(doc);
    BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush, Qt::blue, 24);

    doc->composite();   // 建缓存

    tool->mousePress(mouseEvent(ToolEventType::Press, 40, 60));
    doc->composite();
    for (int i = 1; i <= 4; ++i) {
        tool->mouseMove(mouseEvent(ToolEventType::Move, 40 + i * 35, 60 + i * 12));
        doc->composite();
    }

    // 此时 doc 的合成缓存是**区域补丁**拼出来的 —— 拷一份留证。
    const QImage incremental = doc->composite().copy();

    // 强制走一次全量重算（对同一份图层数据），作为参照。
    doc->markDirty();
    const QImage full = doc->composite().copy();

    check(sameBytes(incremental, full),
          "★★★ 增量补丁结果与全量重算结果逐字节相同（没有漏贴/错贴区域）");

    // 顺带确认这一笔确实被画上去了（不然两张空图也"相同"）。
    const QImage &li = layerImage(doc);
    bool anyPainted = false;
    for (int y = 0; y < li.height() && !anyPainted; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(li.constScanLine(y));
        for (int x = 0; x < li.width(); ++x) {
            if (line[x] != kWhite) { anyPainted = true; break; }
        }
    }
    check(anyPainted, "这一笔确实改变了图层（否则上面的'相同'没有意义）");

    tool->mouseRelease(mouseEvent(ToolEventType::Release, 180, 108));

    delete tool;
    delete doc;
}

// ===========================================================================
//  [4] 撤销粒度与还原
// ===========================================================================
static void testUndoGranularity()
{
    beginGroup("撤销粒度");

    Document *doc = makeDoc(kWhite);
    CommandStack stack(doc);
    BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush, Qt::black, 16);

    const QImage before = layerImage(doc).copy();

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
               "落笔前撤销栈是空的");

    // 一整笔：press + 4 次 move + release
    tool->mousePress(mouseEvent(ToolEventType::Press, 40, 40));
    for (int i = 1; i <= 4; ++i)
        tool->mouseMove(mouseEvent(ToolEventType::Move, 40 + i * 20, 40 + i * 15));
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
               "★ 拖动**期间**一次都不压栈（整笔只在松手时压一条）");
    tool->mouseRelease(mouseEvent(ToolEventType::Release, 120, 100));

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "★★ 一整笔 = 恰好 1 条撤销记录（拖了 5 帧也只压 1 条）");

    const QImage after = layerImage(doc).copy();
    check(!sameBytes(before, after), "这一笔确实改了像素（否则撤销断言没有意义）");

    // ── 撤销 → 逐字节还原 ──
    stack.undo();
    check(sameBytes(layerImage(doc), before),
          "★ 撤销一次 → 图层逐字节回到落笔前（不残留淡影）");

    // ── 重做 → 回到画完的样子 ──
    stack.redo();
    check(sameBytes(layerImage(doc), after),
          "★ 重做 → 图层逐字节回到画完的样子");

    // ── 再画一笔 → 必须是**第二条**记录，不许 mergeWith 合并 ──
    //
    //   DrawCommand::standalone() 会取新描边编号；若两条撞号，
    //   QUndoStack 会**静默** mergeWith，把两笔并成一步 ——
    //   用户画了两笔，Ctrl+Z 一次两笔全没（阶段六首批真踩过）。
    tool->mousePress(mouseEvent(ToolEventType::Press, 200, 150));
    tool->mouseRelease(mouseEvent(ToolEventType::Release, 200, 150));
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 2,
               "★ 再画一笔 → 是第 2 条记录，没有被静默合并");

    stack.undo();
    check(sameBytes(layerImage(doc), after),
          "★ 撤销第 2 笔 → 回到第 1 笔之后的状态（第 1 笔还在）");

    delete tool;
    delete doc;
}

// ===========================================================================
//  [5] 越界与取消
// ===========================================================================
static void testOutOfCanvasAndCancel()
{
    beginGroup("越界与取消");

    // ── 5.1 笔尖完全在画布外 → 不产生撤销记录 ──
    //
    //   用户点在画布外面（或画布边缘之外的一角）时什么都没改，
    //   这时压一条空命令会污染撤销栈 —— Ctrl+Z 按一下什么都没发生。
    {
        Document *doc = makeDoc(kWhite);
        CommandStack stack(doc);
        BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush,
                                   Qt::black, 20);

        tool->mousePress(mouseEvent(ToolEventType::Press, -100, -100));
        tool->mouseRelease(mouseEvent(ToolEventType::Release, -100, -100));

        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "笔尖完全落在画布外 → 不产生撤销记录");

        delete tool;
        delete doc;
    }

    // ── 5.2 取消（Esc）必须把像素还回去 ──
    //
    //   ★ 拖动期间写的是**真实像素**（不是预览图层），
    //     所以取消不能只是"丢掉那个矩形"，必须从落笔前快照恢复。
    //     漏了这一步的症状是"按了 Esc，画的东西还在，而且撤不掉"。
    {
        Document *doc = makeDoc(kWhite);
        CommandStack stack(doc);
        BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush,
                                   Qt::black, 20);

        const QImage before = layerImage(doc).copy();

        tool->mousePress(mouseEvent(ToolEventType::Press, 60, 60));
        tool->mouseMove(mouseEvent(ToolEventType::Move, 120, 90));
        tool->mouseMove(mouseEvent(ToolEventType::Move, 180, 120));
        check(!sameBytes(layerImage(doc), before),
              "拖动期间像素真的被改了（否则下面的还原断言没有意义）");

        tool->cancel();

        check(sameBytes(layerImage(doc), before),
              "★★ Esc 取消 → 图层逐字节回到落笔前");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "★ 取消不产生撤销记录");
        check(!tool->isBusy(), "★ 取消之后工具回到空闲态");

        delete tool;
        delete doc;
    }
}

// ===========================================================================
//  [6] ★ 轮廓圆跟着鼠标走 —— 拖动过程中也必须跟 ★
// ===========================================================================
//
//  症状（用户报的）：用画笔或橡皮**按住**鼠标在画布上拖动时，
//  表示落笔范围的小圆圈停在落笔点不动；**不按住**时才跟着鼠标走。
//
//  根因：圆圈画在 m_hoverWidgetPos 上，而这个缓存原先只在
//  beginStroke()（落笔瞬间）与 hoverMove()（不按键的悬停）里更新。
//  画布是**按"此刻有没有按键"分派**的（无键 → hoverMove，
//  有键 → mouseMove），所以"按住拖动"这条路一次都到不了 hoverMove，
//  悬停缓存自然永远停在落笔点。
//
//  ★ 为什么这条必须量像素 ★
//    这是自绘管线，"缓存里的坐标对了"证明不了画面对。下面把叠加层
//    真的渲染到内存图上，量轮廓外接盒的中心 —— 画没画出来、画在哪，
//    一次全量出来。
static void testOverlayFollowsDrag()
{
    beginGroup("轮廓圆跟随鼠标（拖动中）");

    // 叠加层渲染用的"画布"（控件坐标）。圆圈只是画个圈，
    // 尺寸够装下所有圆心 + 半径即可。
    const QSize kOverlay(240, 180);

    QPointF c;

    // ── 6.1 落笔瞬间：圆心 = 落笔点 ──
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush,
                                   Qt::red, 20);

        tool->mousePress(mouseEvent(ToolEventType::Press, 100, 90));

        check(overlayCircleCenter(tool, kOverlay, &c),
              "落笔后确实画出了一个轮廓圆（不是空的）");
        check(qAbs(c.x() - 100.0) <= 1.0 && qAbs(c.y() - 90.0) <= 1.0,
              "落笔瞬间圆心就在落笔点");

        // ── 6.2 ★ 按住不放，拖到别处 —— 用户报的就是这一步 ★ ──
        //
        //   ★ 顺带钉住"工具必须请求重绘" ★
        //     圆心挪对了但没人重绘，画面上的圆圈照样是钉住的 ——
        //     所以这条得单独断言（needsRepaint 是 ToolResult 的公开字段，
        //     不必去猜 Qt 的事件循环）。
        const ToolResult moveResult =
            tool->mouseMove(mouseEvent(ToolEventType::Move, 170, 50));

        check(moveResult.needsRepaint,
              "★ 拖动中工具请求了重绘（否则圆挪了也没人重画）");
        check(overlayCircleCenter(tool, kOverlay, &c),
              "拖动中轮廓圆还在（没被状态错乱弄没了）");
        check(qAbs(c.x() - 170.0) <= 1.0 && qAbs(c.y() - 50.0) <= 1.0,
              "★★ 拖动中圆心跟着鼠标走（修复前它钉在落笔点 100,90）");

        // ── 6.3 再拖一帧：确认是"持续跟随"，不是"只跟了第一次" ──
        tool->mouseMove(mouseEvent(ToolEventType::Move, 60, 140));

        check(overlayCircleCenter(tool, kOverlay, &c),
              "第三帧仍能画出轮廓");
        check(qAbs(c.x() - 60.0) <= 1.0 && qAbs(c.y() - 140.0) <= 1.0,
              "★ 连续拖动每一帧都跟随");

        // ── 6.4 松手之后走 hoverMove，也必须跟（回退守卫） ──
        //
        //   ★ 为什么要补这一条 ★
        //     本次修复的方向是"让 mouseMove 也更新悬停缓存"，
        //     很容易顺手把 hoverMove 里那一份删掉或改坏 ——
        //     那样"松开后跟随"就会坏掉，而 6.2 / 6.3 照样全绿。
        tool->mouseRelease(mouseEvent(ToolEventType::Release, 60, 140));
        tool->hoverMove(hoverEvent(40, 30));

        check(overlayCircleCenter(tool, kOverlay, &c),
              "松手后的悬停也能画出轮廓");
        check(qAbs(c.x() - 40.0) <= 1.0 && qAbs(c.y() - 30.0) <= 1.0,
              "★ 松开鼠标后圆心跟着鼠标走（hoverMove 那条路没被改坏）");

        delete tool;
        delete doc;
    }

    // ── 6.5 ★ 本帧一个像素都没落（笔尖整个在画布外）时也必须跟 ★ ──
    //
    //   ★ 这是"悬停缓存挪到最前面 + 无条件请求重绘"那个决定唯一能被证伪的地方 ★
    //
    //     笔尖出了画布 → paintSegment 与画布求交后返回空 → 这一帧一行像素都不写。
    //     若这时按老写法 `return ToolResult::accepted()`（不请求重绘），
    //     用户把笔刷拖出画布再拖回来，圆圈会**卡在画布边缘**不动 ——
    //     和用户报的那个 bug 是同一类，只是触发条件更隐蔽。
    //
    //   ★ 为什么要刻意把 imagePos 与 widgetPos 分开构造 ★
    //     上面几条里两个坐标是相同的（图未缩放也未平移），
    //     分不出"用的到底是哪一个"。真实场景下两者并不相等：
    //     图像坐标可以远在画布外，而控件坐标仍在屏幕可见区内。
    //     这条就是要钉住"圆圈跟的是**控件坐标**"。
    //
    //   ★★ 落笔点也必须在画布外 —— 这是第一版写错的地方 ★★
    //
    //     第一版把落笔点放在画布内 (120,100)，只把拖动点甩到
    //     (-900,-900)。看起来"笔尖已经出画布了"，可 paintSegment 是
    //     **按上一落笔点到本帧位置补点**的：那条从 (120,100) 到
    //     (-900,-900) 的线段**斜穿画布**，并集包围盒与画布求交后
    //     **并不为空** → dirty 非空 → 根本没走到"本帧没落笔"那条分支。
    //     于是植入 P（把 dirty 为空时的 repaint 改回 accepted()）
    //     在自检里 **37 项全绿、一条都没红** —— 一条完全没被走到的断言。
    //
    //     所以现在：落笔点与拖动点**都在画布外**，整条线段一个像素
    //     都不碰画布；并且下面**显式断言"这一帧确实没改像素"** ——
    //     把前提本身也钉住，免得下次又写出"看起来在测这条路径、
    //     其实没走到"的假断言。
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        BrushTool *tool = makeTool(doc, &stack, BrushTool::Kind::Brush,
                                   Qt::red, 20);

        // 落笔：图像坐标在画布外（灰色画板区），控件坐标在屏幕可见区内。
        ToolEvent down = mouseEvent(ToolEventType::Press, 0, 0);
        down.imagePos  = QPointF(-300.0, -300.0);
        down.widgetPos = QPointF(50.0, 50.0);
        tool->mousePress(down);

        const QImage before = layerImage(doc).copy();

        ToolEvent off = mouseEvent(ToolEventType::Move, 0, 0);
        off.imagePos  = QPointF(-420.0, -360.0);   // 整条补点线段都在画布外
        off.widgetPos = QPointF(80.0, 40.0);

        const ToolResult r = tool->mouseMove(off);

        // ★ 前提断言：没这一条，上面那段教训会原样复发 ★
        check(sameBytes(layerImage(doc), before),
              "★ 前提：这一帧确实一个像素都没落（否则下面测的不是这条路径）");
        check(r.needsRepaint,
              "★★ 本帧没落笔也要请求重绘（否则拖出画布再拖回时圆圈卡住）");
        check(overlayCircleCenter(tool, kOverlay, &c),
              "本帧没落笔时轮廓圆仍能画出");
        check(qAbs(c.x() - 80.0) <= 1.0 && qAbs(c.y() - 40.0) <= 1.0,
              "★★ 本帧没落笔，圆心照样跟着鼠标走（跟的是控件坐标）");

        tool->cancel();     // 笔在画布外什么都没改，取消一次不会有副作用

        delete tool;
        delete doc;
    }

    // ── 6.6 橡皮：同一个类，同样要跟 ──
    //
    //   用户报的是"画笔或者橡皮"，而两者共用 BrushTool，
    //   所以这里再钉一次 —— 万一将来真的被拆成两个类，这条会立刻红。
    {
        Document *doc = makeDoc(kRed);
        CommandStack stack(doc);
        BrushTool *eraser = makeTool(doc, &stack, BrushTool::Kind::Eraser,
                                     Qt::black, 30);

        eraser->mousePress(mouseEvent(ToolEventType::Press, 70, 60));
        eraser->mouseMove(mouseEvent(ToolEventType::Move, 160, 120));

        check(overlayCircleCenter(eraser, kOverlay, &c),
              "橡皮拖动中也能画出轮廓圆");
        check(qAbs(c.x() - 160.0) <= 1.0 && qAbs(c.y() - 120.0) <= 1.0,
              "★★ 橡皮的轮廓圆同样跟随鼠标（与画笔共用同一个类）");

        delete eraser;
        delete doc;
    }
}

// ===========================================================================

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 画笔工具 离线测试\n");
    std::printf("  （增量失效 / 幂等 / 撤销 / 轮廓跟随）\n");
    std::printf("================================================\n");

    testStampAndIdempotence();
    testIncrementalInvalidation();
    testRegionEqualsFull();
    testUndoGranularity();
    testOutOfCanvasAndCancel();
    testOverlayFollowsDrag();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
