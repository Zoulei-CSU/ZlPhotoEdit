// ============================================================================
//  move_tool_test.cpp —— 移动工具 的离线自检（增量失效 / 残影 / 撤销）
// ----------------------------------------------------------------------------
//  为什么需要它：
//
//    移动工具是**唯一一个"每个鼠标事件都整体重写一整层位图"**的工具
//    （平移 = 生成一张新的整层图，见 transform::translate）。
//    在 4K 图上，这会是一个"每帧改了几百万像素"的操作。
//
//    所以它也是**最容易把脏矩形用错**的地方，而错了有两种截然不同的下场：
//
//      · 区域**算小了** → 画面残留旧内容（"残影"）。
//        最典型的是"只脏本帧所在的那块"：旧位置没人去重合成，
//        合成缓存里就留着上一次的影像。
//        ★ 它的隐蔽之处：只在拖动过程中可见，松手后（或者你换个缩放）就没了。
//          不崩、不报错、所有像素级断言照样全绿。
//
//      · 区域**算大了**（退回整图）→ 正确但白算。
//        ★ 这一种**看不出**。只有 Document 的遥测计数能发现。
//
//    本组就是针对这两条各钉一组断言。判据分别是：
//      · 残影  → 从合成缓存里取样："旧位置那块必须已经变透明"
//      · 白算  → fullCompositeCount() 在拖动期间不许增长
//
//  组别：
//    [1] ★ 拖动期间"旧位置 ∪ 新位置"都必须被重合成（不留残影）
//    [2] ★ 遥测：拖动期间一次全图重算都不许有
//    [3] 撤销粒度：一次拖动 = 一条命令；撤销/重做逐字节还原
//    [4] 没真正移动（只点了一下）→ 不产生撤销记录，且像素回到原样
//
//  ★ 关于取样点 ★
//    刻意选三个互不重叠的位置：
//      · 原始位置独有的一点
//      · **中间某一帧**位置独有的一点（这一帧的旧位置也要被擦掉）
//      · 最终位置独有的一点
//    只验"最终位置画上了"是不够的 —— 那正是"只脏本帧"也能通过的断言。
//
//  编译： ./run_tests.sh movetool
//  ★ 需要 Qt5Widgets ★ —— ToolOptions.cpp 要造控件（即使这里不调
//    createWidget，符号也会被引进来）。见 run_tests.sh 的说明。
// ============================================================================

#include "tools/MoveTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "commands/CommandStack.h"

#include <QApplication>
#include <QColor>
#include <QImage>
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

static void checkClear(QRgb actual, const char *what)
{
    if (qAlpha(actual) != 0) {
        ++g_failed;
        std::printf("  [失败] %s  alpha 期望 0，实际 %d（a=%d r=%d g=%d b=%d）\n",
                    what, qAlpha(actual),
                    qAlpha(actual), qRed(actual), qGreen(actual), qBlue(actual));
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  像素读写（一律走扫描线）
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

static const QRgb kRed   = qRgba(255, 0, 0, 255);
static const QRgb kClear = qRgba(0, 0, 0, 0);

static const int kW = 256;
static const int kH = 192;

// 内容方块的几何：原点 (10,10)、边长 40。
// 三个取样点都由它推出来，改一个数不用改三处。
static const QRect kContentRect(10, 10, 40, 40);
static const QPoint kPressInContent(30, 30);      // 落在方块内部（必须在内容上按下）

// 单图层文档：内容方块不透明红，其余全透明。
static Document *makeDoc()
{
    auto *doc = new Document(QSize(kW, kH), Qt::transparent);
    Layer *l = doc->activeLayer();
    if (l) {
        QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        rawFillRect(img, kContentRect, kRed);
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

static ToolEvent moveEvent(int x, int y)
{
    ToolEvent e;
    e.type = ToolEventType::Move;
    e.button  = Qt::LeftButton;
    e.buttons = Qt::LeftButton;      // 拖动中：左键仍按着
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    return e;
}

static ToolEvent pressEvent(int x, int y)
{
    ToolEvent e = moveEvent(x, y);
    e.type = ToolEventType::Press;
    return e;
}

static ToolEvent releaseEvent(int x, int y)
{
    ToolEvent e = moveEvent(x, y);
    e.type = ToolEventType::Release;
    e.buttons = Qt::NoButton;        // 松手：此刻没有键按着
    return e;
}

// ===========================================================================
//  [1] ★ 拖动期间"旧位置 ∪ 新位置"都必须被重合成（不留残影）
// ===========================================================================
//
//  拖动两帧：内容从原位 (10,10) 先挪到 (50,30)，再挪到 (90,50)。
//
//    帧① 的脏区域应当是 (10,10,40,40) ∪ (50,30,40,40)
//    帧② 的脏区域应当是 (50,30,40,40) ∪ (90,50,40,40)
//
//  ★ 只脏"本帧新位置"的实现会在帧①漏掉 (10,10) 那一块，
//    于是合成缓存里永远留着最初那枚红方块 —— 就是"残影"。
// ===========================================================================
static void testNoGhostDuringDrag()
{
    beginGroup("拖动不留残影（旧位置 ∪ 新位置）");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    MoveTool tool(doc, &stack);

    // 建缓存：此刻缓存里只有原位那一枚方块
    {
        const QImage comp = doc->composite();
        if (qAlpha(rawPixel(comp, kContentRect.center().x(),
                            kContentRect.center().y())) == 0) {
            check(false, "前置条件：内容方块应当已经在合成缓存里（否则本组无意义）");
        }
    }

    tool.mousePress(pressEvent(kPressInContent.x(), kPressInContent.y()));

    // ── 帧①：位移 (40,20) ──
    tool.mouseMove(moveEvent(kPressInContent.x() + 40, kPressInContent.y() + 20));
    doc->composite();

    // ── 帧②：位移 (80,40) ──
    tool.mouseMove(moveEvent(kPressInContent.x() + 80, kPressInContent.y() + 40));
    const QImage comp = doc->composite();

    // 三个互不重叠的取样点
    const QPoint oldOnly(15, 15);           // 只在原位（10..49, 10..49）
    const QPoint midOnly(55, 35);           // 只在帧①位置（50..89, 30..69）
    const QPoint newOnly(100, 60);          // 只在帧②位置（90..129, 50..89）

    checkClear(rawPixel(comp, oldOnly.x(), oldOnly.y()),
               "★★ 原始位置已变透明（只脏本帧的实现在这里会留下一枚残影）");
    checkClear(rawPixel(comp, midOnly.x(), midOnly.y()),
               "★★ 中间帧的位置也已变透明（\"上帧 ∪ 本帧\"里的\"上帧\"）");
    check(qAlpha(rawPixel(comp, newOnly.x(), newOnly.y())) == 255,
          "★ 最终位置已经画上了内容");

    // 附带：图层位图本身也必须是"只在最终位置"
    const QImage &li = layerImage(doc);
    checkClear(rawPixel(li, oldOnly.x(), oldOnly.y()),
               "图层位图：原位置为空");
    checkClear(rawPixel(li, midOnly.x(), midOnly.y()),
               "图层位图：中间帧位置为空");
    check(qAlpha(rawPixel(li, newOnly.x(), newOnly.y())) == 255,
          "图层位图：最终位置有不透明内容");

    tool.mouseRelease(releaseEvent(kPressInContent.x() + 80,
                                  kPressInContent.y() + 40));
}

// ===========================================================================
//  [2] ★ 遥测：拖动期间一次全图重算都不许有
// ===========================================================================
static void testIncrementalInvalidation()
{
    beginGroup("增量失效（遥测计数）");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    MoveTool tool(doc, &stack);

    doc->composite();
    const quint64 fullBefore   = doc->fullCompositeCount();
    const quint64 regionBefore = doc->regionCompositeCount();

    tool.mousePress(pressEvent(kPressInContent.x(), kPressInContent.y()));
    for (int i = 1; i <= 5; ++i) {
        tool.mouseMove(moveEvent(kPressInContent.x() + i * 12,
                                 kPressInContent.y() + i * 6));
        doc->composite();
    }

    const quint64 fullAfter   = doc->fullCompositeCount();
    const quint64 regionAfter = doc->regionCompositeCount();
    std::printf("     5 帧拖动：全量 +%llu，区域 +%llu\n",
                static_cast<unsigned long long>(fullAfter - fullBefore),
                static_cast<unsigned long long>(regionAfter - regionBefore));

    checkEqInt(int(fullAfter - fullBefore), 0,
               "★★ 拖动期间一次全图重算都不许有（每帧只重算两块）");
    check(regionAfter - regionBefore >= 5,
          "★ 每一帧都走了区域补丁快路径");

    // ── 松手那一下也不许退回全图 ──
    //
    //   ImageCommand 必须带上 movedRegion；不带的话 push() 会按全图失效，
    //   "整段拖动都流畅，唯独松手卡一下"（4K 约 431ms）。
    tool.mouseRelease(releaseEvent(kPressInContent.x() + 5 * 12,
                                  kPressInContent.y() + 5 * 6));
    const quint64 fullBeforeRelease = doc->fullCompositeCount();
    doc->composite();
    checkEqInt(int(doc->fullCompositeCount() - fullBeforeRelease), 0,
               "★ 松手压命令时也是区域失效，不是全图");
}

// ===========================================================================
//  [3] 撤销粒度：一次拖动 = 一条命令
// ===========================================================================
static void testUndoGranularity()
{
    beginGroup("撤销粒度");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    MoveTool tool(doc, &stack);

    const QImage before = layerImage(doc).copy();
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
               "拖动前撤销栈是空的");

    tool.mousePress(pressEvent(kPressInContent.x(), kPressInContent.y()));
    for (int i = 1; i <= 4; ++i)
        tool.mouseMove(moveEvent(kPressInContent.x() + i * 15,
                                 kPressInContent.y() + i * 10));
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
               "★ 拖动**期间**一次都不压栈");
    tool.mouseRelease(releaseEvent(kPressInContent.x() + 4 * 15,
                                  kPressInContent.y() + 4 * 10));

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "★★ 一次拖动 = 恰好 1 条撤销记录");

    const QImage after = layerImage(doc).copy();
    check(!sameBytes(before, after), "这次拖动确实改了像素");

    stack.undo();
    check(sameBytes(layerImage(doc), before),
          "★ 撤销一次 → 图层逐字节回到原位");

    stack.redo();
    check(sameBytes(layerImage(doc), after),
          "★ 重做 → 图层逐字节回到移动后的位置");
}

// ===========================================================================
//  [4] 只点一下（没真正移动）→ 不产生记录，像素回到原样
// ===========================================================================
static void testClickWithoutMove()
{
    beginGroup("点击未移动");

    Document *doc = makeDoc();
    CommandStack stack(doc);
    MoveTool tool(doc, &stack);

    const QImage before = layerImage(doc).copy();

    tool.mousePress(pressEvent(kPressInContent.x(), kPressInContent.y()));
    tool.mouseRelease(releaseEvent(kPressInContent.x(), kPressInContent.y()));

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
               "★ 没移动 → 不产生撤销记录（Ctrl+Z 不该撤出一个空操作）");
    check(sameBytes(layerImage(doc), before),
          "★ 没移动 → 图层逐字节不变");
}

// ===========================================================================

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 移动工具 离线测试（残影 / 增量失效 / 撤销）\n");
    std::printf("================================================\n");

    testNoGhostDuringDrag();
    testIncrementalInvalidation();
    testUndoGranularity();
    testClickWithoutMove();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
