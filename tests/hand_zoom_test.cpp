// ============================================================================
//  hand_zoom_test.cpp —— 抓手 / 缩放 的离线自检
// ----------------------------------------------------------------------------
//  这两个工具是**唯一两个"不碰文档"的工具**：不改一个像素、不压一条命令。
//  它们唯一的产出是三个视图请求信号：
//
//      requestPan(deltaWidget)              —— 抓手
//      requestZoom(factor, anchorWidget)    —— 缩放（点击）
//      requestZoomToRect(imageRect)         —— 缩放（框选）
//
//  所以本组的形状与前面几组完全不同：**不去量图层像素，而是量信号的实参**。
//  信号参数错了，工具"看起来"完全正常 —— 画面会平移/缩放，只是位置或倍数
//  不对，而且没有任何报错。这正是需要逐参数量化的地方。
//
//  ────────────────────────────────────────────────────────────────────────
//  本组盯的四类东西：
//
//    A. **位移的坐标系与增量口径**（抓手）
//       ① 位移必须是"控件坐标"。
//          用图像坐标算的话，800% 缩放下拖 8 个屏幕像素画面只挪 1 个图像像素
//          —— 手感是"怎么拖都不动"，而代码看起来完全合理。
//          断言方式：同一串控件位移，在 viewZoom=1 与 viewZoom=8 下
//          发出的 delta 必须**逐项相同**。
//       ② 增量必须取自"上一次位置"，不是"按下位置"。
//          取按下位置的话每次 move 都会把整段位移重复加一遍，
//          拖动以平方级加速冲出屏幕（"一开始正常、越拖越快"）。
//          断言方式：连续两次 move，第二次的 delta 必须是**那一步**的差，
//          而不是"从按下点算起"的差。这两个值在第二步开始就不相等，
//          所以一条断言就能把两种实现分开。
//
//    B. **点击缩放的锚点与方向**（缩放）
//       ③ 锚点必须是**按下点**，不是松手点。
//          用松手点的话，手抖几个像素就让画面往旁边偏，连续点击会累积漂移。
//       ④ Alt 必须**对称地**反转方向（放大→缩小、缩小→放大）。
//          只反转一边的实现看起来"能用"，但用户在缩小模式下按 Alt 会得到
//          缩小 —— 而选项条上明明白白写着"可临时切换到相反方向"。
//
//    C. **框选与点击的分界线**（缩放）
//       ⑤ 阈值必须存在，且判定要用**控件坐标**。
//          没有阈值 → 想点一下放大却每次都框住一个 2×3 像素的区域，
//          画面随机跳到某个无意义的局部。
//          阈值用图像坐标 → 放大状态下永远触发不了框选（6 个控件像素
//          在 800% 下只有 0.75 个图像像素）。
//       ⑥ 退化的框选要**回落成单击**，不能什么都不做。
//       ⑦ 框选矩形必须归一化（反向拖也要得到同一个框）。
//
//    D. **叠加层的真实像素**（缩放）
//       ⑧ 框选时画的必须是一个**空心**框：框内一个像素都不能被覆盖。
//          画成实心块的话，"量出来的包围盒"照样是对的 ——
//          只有量"框内部"才能把两者分开。
//
//  ────────────────────────────────────────────────────────────────────────
//  ★ 为什么本组不构造真正的 CanvasView ★
//    那两个信号在 CanvasView 里被接到 panBy / setZoomAt / zoomToRect，
//    但"接得对不对"属于装配，归 mainwin_test 与 view_test 管。
//    本组只钉"工具发出的实参对不对" —— 那是两个不同的问题，
//    混在一起测的话，一处出错会同时弄红两组断言，反而更难定位。
//    所以本组**刻意不链 ui/**，跑得也快。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 handzoom 分支。需要 QApplication（ToolOptions.cpp
//    链了 QtWidgets），用 offscreen 插件在无显示器环境里跑。
// ============================================================================

#include "tools/HandTool.h"
#include "tools/ZoomTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolParams.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "commands/CommandStack.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRectF>
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

static void checkPoint(const QPointF &actual, const QPointF &expected, const char *what)
{
    // 位移都是"整数减整数"，浮点表示精确，可以直接比。
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 (%.3f, %.3f)，实际 (%.3f, %.3f)\n",
                    what, expected.x(), expected.y(), actual.x(), actual.y());
    } else {
        ++g_passed;
    }
}

static void checkClose(qreal actual, qreal expected, qreal eps, const char *what)
{
    if (!(qAbs(actual - expected) <= eps)) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %.6f，实际 %.6f\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkRect(const QRectF &actual, const QRectF &expected, const char *what)
{
    const bool same = qAbs(actual.x() - expected.x()) <= 0.001
                   && qAbs(actual.y() - expected.y()) <= 0.001
                   && qAbs(actual.width()  - expected.width())  <= 0.001
                   && qAbs(actual.height() - expected.height()) <= 0.001;
    if (!same) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 (%.2f, %.2f, %.2f×%.2f)，"
                    "实际 (%.2f, %.2f, %.2f×%.2f)\n", what,
                    expected.x(), expected.y(), expected.width(), expected.height(),
                    actual.x(), actual.y(), actual.width(), actual.height());
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  事件构造
// ---------------------------------------------------------------------------
//  ★ Release 事件的 button 必须是左键、move 事件的 buttons 必须是左键 ★
//    真实 Qt 事件里：button() = "造成这次事件的那个键"，
//    buttons() = "此刻仍按着的键"。
//    把 Release 的 button 写成 NoButton，工具的
//    `if (e.button != Qt::LeftButton) return;` 会直接早退，
//    状态机永远停不下来（症状："松手后还在拖动"）。
//    这个坑在 tool_paint_test / shape_tool_test 里都记录过。
// ---------------------------------------------------------------------------
static ToolEvent evPress(const QPointF &widgetPos, const QPointF &imagePos,
                         bool alt = false)
{
    ToolEvent e;
    e.type    = ToolEventType::Press;
    e.widgetPos = widgetPos;
    e.imagePos  = imagePos;
    e.lastImagePos = imagePos;
    e.button  = Qt::LeftButton;
    e.buttons = Qt::LeftButton;
    e.modifiers.alt = alt;
    return e;
}

static ToolEvent evMove(const QPointF &widgetPos, const QPointF &imagePos,
                        bool alt = false)
{
    ToolEvent e;
    e.type    = ToolEventType::Move;
    e.widgetPos = widgetPos;
    e.imagePos  = imagePos;
    e.lastImagePos = imagePos;
    e.button  = Qt::NoButton;      // 移动事件没有"造成它的键"
    e.buttons = Qt::LeftButton;    // 但左键是按着的
    e.modifiers.alt = alt;
    return e;
}

static ToolEvent evRelease(const QPointF &widgetPos, const QPointF &imagePos,
                           bool alt = false)
{
    ToolEvent e;
    e.type    = ToolEventType::Release;
    e.widgetPos = widgetPos;
    e.imagePos  = imagePos;
    e.lastImagePos = imagePos;
    e.button  = Qt::LeftButton;    // ★ 松手事件要带上是哪个键松的 ★
    e.buttons = Qt::NoButton;
    e.modifiers.alt = alt;
    return e;
}

// ---------------------------------------------------------------------------
//  信号记录器
// ---------------------------------------------------------------------------
//  ★ 为什么记录实参而不是只看"发生过" ★
//    本组全部价值都在实参上：位移错一个坐标系、锚点错成松手点、
//    倍率少了反转 —— 这三件事都"有信号发出"，只看有无一条都抓不住。
struct Recorder
{
    QVector<QPointF> pans;
    QVector<qreal>   zoomFactors;
    QVector<QPointF> zoomAnchors;
    QVector<QRectF>  zoomRects;

    void attach(AbstractTool *tool)
    {
        QObject::connect(tool, &AbstractTool::requestPan, tool,
                         [this](const QPointF &d) { pans.append(d); });
        QObject::connect(tool, &AbstractTool::requestZoom, tool,
                         [this](qreal f, const QPointF &a) {
                             zoomFactors.append(f);
                             zoomAnchors.append(a);
                         });
        QObject::connect(tool, &AbstractTool::requestZoomToRect, tool,
                         [this](const QRectF &r) { zoomRects.append(r); });
    }

    void clear()
    {
        pans.clear();
        zoomFactors.clear();
        zoomAnchors.clear();
        zoomRects.clear();
    }
};

// ===========================================================================
//  [1] 身份证与参数对象
// ===========================================================================
static void testIdentity()
{
    beginGroup("身份证与参数对象");

    HandTool hand(nullptr, nullptr, nullptr);
    ZoomTool zoom(nullptr, nullptr, nullptr);

    check(hand.id() == QLatin1String("hand"), "抓手的 id 是 hand");
    check(zoom.id() == QLatin1String("zoom"), "缩放的 id 是 zoom");

    // ★ 两个 id 不能相同 ★
    //   工具箱按钮用 QActionGroup 做互斥，两个工具共用一个 id 的话
    //   按钮的 objectName 会撞在一起，选中态跟着乱；
    //   而且 QSettings 的"上次使用的工具"永远只会命中其中一个。
    check(hand.id() != zoom.id(), "两个工具的 id 不同");

    check(!hand.displayName().isEmpty(), "抓手有中文显示名");
    check(!zoom.displayName().isEmpty(), "缩放有中文显示名");
    check(hand.displayName() != zoom.displayName(), "两个工具的显示名不同");

    checkEqInt(hand.shortcutKey(), Qt::Key_H, "抓手绑 H");
    checkEqInt(zoom.shortcutKey(), Qt::Key_Z, "缩放绑 Z");
    check(hand.shortcutKey() != zoom.shortcutKey(), "两个工具的快捷键不同");

    // ★ 抓手没有可选参数，缩放有（放大/缩小方向 + Alt 提示）★
    //   抓手返回 nullptr 是**有意义**的：ToolOptionsBar 据此不显示选项条。
    //   如果哪天给它随便塞一个空选项对象，选项条上会出现一条空白栏。
    check(hand.options() == nullptr, "抓手没有选项条（options() 为空）");
    check(zoom.options() != nullptr, "缩放有选项条");
    check(qobject_cast<ZoomOptions *>(zoom.options()) != nullptr,
          "缩放的选项对象确实是 ZoomOptions");
}

// ===========================================================================
//  [2] 抓手：光标的两态
// ===========================================================================
static void testHandCursor()
{
    beginGroup("抓手：松开的手 / 握紧的手");

    HandTool t(nullptr, nullptr, nullptr);

    checkEqInt(t.cursor().shape(), int(Qt::OpenHandCursor),
               "空闲时是「张开的手」");

    t.mousePress(evPress(QPointF(100, 100), QPointF(100, 100)));
    checkEqInt(t.cursor().shape(), int(Qt::ClosedHandCursor),
               "拖动中是「握紧的手」（用户唯一能确认「抓住了」的反馈）");

    t.mouseRelease(evRelease(QPointF(120, 130), QPointF(120, 130)));
    checkEqInt(t.cursor().shape(), int(Qt::OpenHandCursor),
               "松手后回到「张开的手」");

    // 拖动途中被切走（或关文档）时也要回到张开的手 ——
    // 不然切回来的第一眼看到"握紧的手"，像是还抓着什么。
    t.mousePress(evPress(QPointF(10, 10), QPointF(10, 10)));
    t.cancel();
    checkEqInt(t.cursor().shape(), int(Qt::OpenHandCursor),
               "cancel() 之后回到「张开的手」");
}

// ===========================================================================
//  [3] 抓手：增量取自"上一次位置"
// ===========================================================================
static void testHandDelta()
{
    beginGroup("抓手：增量取自上一次位置，不是按下位置");

    HandTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    const ToolEvent press = evPress(QPointF(100, 100), QPointF(400, 400));
    check(t.mousePress(press).handled, "左键按下被抓手消费");
    check(t.isDragging(), "按下之后进入拖动状态");
    checkEqInt(rec.pans.size(), 0, "按下本身不产生平移");

    t.mouseMove(evMove(QPointF(110, 120), QPointF(440, 480)));
    checkEqInt(rec.pans.size(), 1, "第一次移动发出一次平移");
    checkPoint(rec.pans.value(0), QPointF(10, 20), "第一次位移 = 本次 - 按下点");

    t.mouseMove(evMove(QPointF(112, 118), QPointF(448, 472)));
    checkEqInt(rec.pans.size(), 2, "第二次移动再发出一次平移");
    // ★ 这一条是全组的重点 ★
    //   若实现用的是"按下位置"，这里会得到 (12, 18)；
    //   用"上一次位置"才是 (2, -2)。两者从第二次移动起就不再相等，
    //   而后者才是"鼠标走多远画面跟多远"。
    checkPoint(rec.pans.value(1), QPointF(2, -2),
               "第二次位移 = 本次 - 上一次（不是 - 按下点）");

    t.mouseRelease(evRelease(QPointF(112, 118), QPointF(448, 472)));
    check(!t.isDragging(), "松手后退出拖动状态");

    t.mouseMove(evMove(QPointF(200, 200), QPointF(800, 800)));
    checkEqInt(rec.pans.size(), 2, "松手之后再移动不产生平移");
}

// ===========================================================================
//  [4] 抓手：位移与缩放比无关（必须是控件坐标）
// ===========================================================================
static void testHandIndependentOfZoom()
{
    beginGroup("抓手：位移是控件坐标，与缩放比无关");

    // 同一串控件位移，跑两遍：一遍 viewZoom=1，一遍 viewZoom=8。
    // ★ 若实现按图像坐标算位移（或把位移除以了 zoom），
    //   第二遍的结果会是第一遍的 1/8 —— 而代码看起来完全合理。 ★
    auto runWith = [](qreal viewZoom) {
        HandTool t(nullptr, nullptr, nullptr);
        Recorder rec;
        rec.attach(&t);
        ToolEvent p = evPress(QPointF(50, 60), QPointF(50 * viewZoom, 60 * viewZoom));
        p.viewZoom = viewZoom;
        t.mousePress(p);
        ToolEvent m1 = evMove(QPointF(70, 90), QPointF(70 * viewZoom, 90 * viewZoom));
        m1.viewZoom = viewZoom;
        t.mouseMove(m1);
        ToolEvent m2 = evMove(QPointF(60, 100), QPointF(60 * viewZoom, 100 * viewZoom));
        m2.viewZoom = viewZoom;
        t.mouseMove(m2);
        return rec.pans;
    };

    const QVector<QPointF> at1 = runWith(1.0);
    const QVector<QPointF> at8 = runWith(8.0);

    checkEqInt(at1.size(), 2, "viewZoom=1 时发出两次平移");
    checkEqInt(at8.size(), 2, "viewZoom=8 时发出两次平移");

    checkPoint(at1.value(0), QPointF(20, 30), "viewZoom=1 的第一段位移");
    // 8 倍缩放下的图像坐标差了 8 倍，但控件位移还是 (20,30) —— 必须一样。
    checkPoint(at8.value(0), QPointF(20, 30),
               "viewZoom=8 的同一串控件位移，结果完全相同（不是 1/8）");
    checkPoint(at1.value(1), at8.value(1),
               "第二段位移在两种缩放比下也完全相同");
}

// ===========================================================================
//  [5] 抓手：不该发请求的几种情形
// ===========================================================================
static void testHandRefusals()
{
    beginGroup("抓手：没有按下 / 非左键 / 原地不动");

    HandTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    // ① 没按下就划过画布
    const ToolResult r0 = t.mouseMove(evMove(QPointF(10, 10), QPointF(10, 10)));
    check(!r0.handled, "没有按下时抓手的 mouseMove 不消费事件");
    checkEqInt(rec.pans.size(), 0, "没有按下时不产生平移");

    // ② 中键 / 右键按下应当被拒（它们在 CanvasView 里就走不到工具，
    //    但工具自己也要守住这条边界，否则自检里就能造出"右键拖动平移"）
    ToolEvent mid = evPress(QPointF(10, 10), QPointF(10, 10));
    mid.button  = Qt::MiddleButton;
    mid.buttons = Qt::MiddleButton;
    check(!t.mousePress(mid).handled, "中键按下被抓手拒绝");
    check(!t.isDragging(), "中键被拒后没有进入拖动状态");

    ToolEvent right = evPress(QPointF(10, 10), QPointF(10, 10));
    right.button  = Qt::RightButton;
    right.buttons = Qt::RightButton;
    check(!t.mousePress(right).handled, "右键按下被抓手拒绝");
    check(!t.isDragging(), "右键被拒后没有进入拖动状态");

    // ③ 原地抖动（delta 为 0）不该发平移请求
    //    ★ 为什么这条重要 ★
    //      Qt 在"鼠标停着但窗口被重绘/按下修饰键"时也会发 mouseMove。
    //      每发一次就重建一次视图矩阵 + 重绘整幅画面，是纯浪费；
    //      而"画面在抖"这种症状极难归因到"跟着自己不该发的信号重绘了"。
    t.mousePress(evPress(QPointF(300, 300), QPointF(300, 300)));
    t.mouseMove(evMove(QPointF(300, 300), QPointF(300, 300)));
    checkEqInt(rec.pans.size(), 0, "原地移动（零位移）不产生平移请求");
    checkEqInt(t.panCount(), 0, "零位移不计入平移次数");

    // ④ 非左键松手不该结束拖动（真实的 button 就摆在那里）
    ToolEvent otherUp = evRelease(QPointF(320, 300), QPointF(320, 300));
    otherUp.button = Qt::RightButton;
    check(!t.mouseRelease(otherUp).handled, "右键松手不结束左键拖动");
    check(t.isDragging(), "拖动状态仍在（右键松手被忽略）");

    // ⑤ 一次真实移动 → 一次平移；松手本身不产生平移
    //   （③ 那次是零位移，所以这里才出现第一次有效平移。）
    t.mouseMove(evMove(QPointF(320, 300), QPointF(320, 300)));
    checkEqInt(rec.pans.size(), 1, "随后一次真实移动产生一次平移");
    checkPoint(rec.pans.value(0), QPointF(20, 0), "位移就是那一次移动的差");

    t.mouseRelease(evRelease(QPointF(320, 300), QPointF(320, 300)));
    check(!t.isDragging(), "左键松手才真正结束拖动");
    checkEqInt(rec.pans.size(), 1, "松手本身不产生平移");
}

// ===========================================================================
//  [6] 缩放：点击缩放的锚点与步长
// ===========================================================================
static void testZoomClick()
{
    beginGroup("缩放：单击一档，锚点是按下点");

    ZoomTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    // 按下与松手在同一个点 —— 最标准的"点一下"
    t.mousePress(evPress(QPointF(200, 150), QPointF(500, 400)));
    t.mouseRelease(evRelease(QPointF(200, 150), QPointF(500, 400)));

    checkEqInt(rec.zoomFactors.size(), 1, "点一下发出一次缩放请求");
    checkClose(rec.zoomFactors.value(0), kZoomStepFactor, 1e-9,
               "点击缩放的步长就是共享常量 kZoomStepFactor");
    // ★ 锚点必须是**控件坐标** ★
    //   图像坐标是 (500,400)，控件坐标是 (200,150)。断言等于后者，
    //   就同时证明了"没把图像坐标当成锚点传出去"。
    checkPoint(rec.zoomAnchors.value(0), QPointF(200, 150),
               "锚点是控件坐标（不是图像坐标）");
    checkEqInt(rec.zoomRects.size(), 0, "点击缩放不产生框选请求");
    checkEqInt(t.clickZoomCount(), 1, "点击缩放计数为 1");
    checkEqInt(t.marqueeZoomCount(), 0, "框选缩放计数为 0");

    // 松手点不同（但没超过阈值）→ 锚点仍然是**按下点**
    rec.clear();
    t.mousePress(evPress(QPointF(200, 150), QPointF(500, 400)));
    t.mouseMove(evMove(QPointF(203, 152), QPointF(507.5, 405)));
    t.mouseRelease(evRelease(QPointF(205, 155), QPointF(512.5, 412.5)));

    checkEqInt(rec.zoomFactors.size(), 1, "手抖范围内的移动仍然算点击");
    checkPoint(rec.zoomAnchors.value(0), QPointF(200, 150),
               "锚点是按下点，不是松手点（否则连续点击会累积漂移）");
    checkEqInt(rec.zoomRects.size(), 0, "手抖没有变成框选");
    check(!t.isMarqueeActive(), "手抖之后框选状态没有被打开");
}

// ===========================================================================
//  [7] 缩放：Alt 对称反转
// ===========================================================================
static void testZoomAlt()
{
    beginGroup("缩放：Alt 对称反转方向");

    ZoomTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    // ★ 先直接问 zoomsIn()，得到一张真值表 ★
    //   从点击结果反推只能覆盖"当前是哪种模式"，而两种模式 × 有无 Alt
    //   一共四种组合，直接问函数才能全部覆盖到。
    auto *opt = qobject_cast<ZoomOptions *>(t.options());
    check(opt != nullptr, "能取到 ZoomOptions");

    opt->setZoomIn(true);
    check(t.zoomsIn(false), "放大模式：不按 Alt → 放大");
    check(!t.zoomsIn(true),  "放大模式：按住 Alt → 缩小");

    opt->setZoomIn(false);
    check(!t.zoomsIn(false), "缩小模式：不按 Alt → 缩小");
    check(t.zoomsIn(true),   "缩小模式：按住 Alt → 放大（反转必须是**对称**的）");

    // 再看实际发出去的倍率
    rec.clear();
    opt->setZoomIn(true);
    t.mousePress(evPress(QPointF(10, 20), QPointF(10, 20)));
    t.mouseRelease(evRelease(QPointF(10, 20), QPointF(10, 20)));
    checkClose(rec.zoomFactors.value(0), kZoomStepFactor, 1e-9,
               "放大模式点一下 → ×1.25");

    rec.clear();
    t.mousePress(evPress(QPointF(10, 20), QPointF(10, 20)));
    t.mouseRelease(evRelease(QPointF(10, 20), QPointF(10, 20), /*alt=*/true));
    checkClose(rec.zoomFactors.value(0), 1.0 / kZoomStepFactor, 1e-9,
               "放大模式 + Alt → ×0.8（不是 ×1.25）");

    rec.clear();
    opt->setZoomIn(false);
    t.mousePress(evPress(QPointF(10, 20), QPointF(10, 20)));
    t.mouseRelease(evRelease(QPointF(10, 20), QPointF(10, 20)));
    checkClose(rec.zoomFactors.value(0), 1.0 / kZoomStepFactor, 1e-9,
               "缩小模式点一下 → ×0.8");

    rec.clear();
    t.mousePress(evPress(QPointF(10, 20), QPointF(10, 20)));
    t.mouseRelease(evRelease(QPointF(10, 20), QPointF(10, 20), /*alt=*/true));
    checkClose(rec.zoomFactors.value(0), kZoomStepFactor, 1e-9,
               "缩小模式 + Alt → ×1.25");

    // ★ 倍率必须与"按快捷键放大"一致 ★
    //   两边都从 tools/ToolParams.h 取 kZoomStepFactor，
    //   这条断言把这个约定写死在测试里（改了共享常量，本组与
    //   view_test 会同时红）。
    checkClose(kZoomStepFactor, 1.25, 1e-9,
               "共享步长常量是 1.25（与 CanvasView::zoomIn 同源）");
}

// ===========================================================================
//  [8] 缩放：阈值与"手抖不算框选"
// ===========================================================================
static void testZoomThreshold()
{
    beginGroup("缩放：拖动阈值（手抖不算框选）");

    ZoomTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    const qreal thr = ZoomTool::marqueeThreshold();

    // ① 刚好差一点点：必须仍然是点击
    //   ★ 阈值用控件坐标 ★ 所以 image 坐标这里故意放得很大，
    //     如果实现错用了图像坐标，这一步会被判成"框选"。
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(1000, 1000)));
    t.mouseMove(evMove(QPointF(100 + thr - 0.5, 100 + thr - 0.5),
                       QPointF(1220, 1220)));
    check(!t.isMarqueeActive(), "差 0.5 像素没到阈值 → 还没进入框选");
    t.mouseRelease(evRelease(QPointF(100 + thr - 0.5, 100 + thr - 0.5),
                             QPointF(1220, 1220)));
    checkEqInt(rec.zoomRects.size(), 0, "未到阈值不产生框选请求");
    checkEqInt(rec.zoomFactors.size(), 1, "未到阈值仍然按点击处理（发了一次缩放）");

    // ② 正好达到阈值：进入框选（边界是 >=，不是 >）
    //   ★ 图像矩形必须**非退化** ★
    //     若两个端点只在 y 上相差 0，换算出来的图像矩形高度就是 0，
    //     松手时会走"退化回落成单击"那条路（见 [10]）——
    //     那是在测另一件事，本组就不再是在测阈值了。
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(1000, 1000)));
    t.mouseMove(evMove(QPointF(100 + thr, 100), QPointF(1400, 1400)));
    check(t.isMarqueeActive(), "达到阈值 → 进入框选模式");
    t.mouseRelease(evRelease(QPointF(100 + thr, 100), QPointF(1400, 1400)));
    checkEqInt(rec.zoomRects.size(), 1, "达到阈值后松手发的是框选请求");
    checkEqInt(rec.zoomFactors.size(), 0, "进入框选之后不再发点击缩放");

    // ③ 竖直方向单独判定（用 |dx|/|dy| 的较大者，不是欧氏距离）
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(1000, 1000)));
    t.mouseMove(evMove(QPointF(100, 100 + thr), QPointF(1050, 1400)));
    check(t.isMarqueeActive(), "只有竖直方向拖够距离也会进入框选");

    // ④ 每次按下都会重置框选状态
    //   ★ 不清的话，上一次的框会留下来，这一次"点一下"的松手
    //     会被判成框选，而且框还是上一次的位置（错位且极难复现）。
    t.mouseRelease(evRelease(QPointF(100, 100 + thr), QPointF(1050, 1400)));
    rec.clear();
    t.mousePress(evPress(QPointF(300, 300), QPointF(300, 300)));
    check(!t.isMarqueeActive(), "重新按下时框选状态被清掉");
    check(t.marqueeImageRect().isEmpty(), "重新按下时框选矩形是空的");
    t.mouseRelease(evRelease(QPointF(300, 300), QPointF(300, 300)));
    checkEqInt(rec.zoomRects.size(), 0, "这一次是点击，不是框选");
    checkEqInt(rec.zoomFactors.size(), 1, "这一次走的是点击缩放");
}

// ===========================================================================
//  [9] 缩放：框选矩形（图像坐标 + 归一化 + 不受 Alt 影响）
// ===========================================================================
static void testZoomMarqueeRect()
{
    beginGroup("缩放：框选矩形的内容");

    ZoomTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    // ① 矩形取的是**图像坐标**
    //   ★ 构造时刻意让控件坐标与图像坐标差得很远（差 4 倍）★
    //     取错坐标系的实现会得到 (100,100,200×150)，与期望差一个数量级。
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(400, 400)));
    t.mouseMove(evMove(QPointF(300, 250), QPointF(1200, 1000)));
    check(t.isMarqueeActive(), "拖动已进入框选");
    checkRect(t.marqueeImageRect(), QRectF(400, 400, 800, 600),
              "框选矩形是图像坐标");
    checkRect(t.marqueeWidgetRect(), QRectF(100, 100, 200, 150),
              "框选的控件矩形（画虚线框用）");
    t.mouseRelease(evRelease(QPointF(300, 250), QPointF(1200, 1000)));
    checkEqInt(rec.zoomRects.size(), 1, "松手发出一次框选请求");
    checkRect(rec.zoomRects.value(0), QRectF(400, 400, 800, 600),
              "请求里的矩形与拖动一致");
    checkRect(t.lastZoomToRect(), QRectF(400, 400, 800, 600),
              "观测口记下了同一个矩形");
    checkEqInt(t.marqueeZoomCount(), 1, "框选缩放计数为 1");
    checkEqInt(t.clickZoomCount(), 0, "框选没有混进点击计数");

    // ② 反向拖（右下 → 左上）必须得到同一个矩形
    //   ★ 不归一化的话会得到负宽高，zoomToRect 直接判 isEmpty 走人 ——
    //     症状是"只有从左上往右下拖才有效"，用户会以为框选是碰运气。★
    rec.clear();
    t.mousePress(evPress(QPointF(300, 250), QPointF(1200, 1000)));
    t.mouseMove(evMove(QPointF(100, 100), QPointF(400, 400)));
    t.mouseRelease(evRelease(QPointF(100, 100), QPointF(400, 400)));
    checkEqInt(rec.zoomRects.size(), 1, "反向拖动也发出框选请求");
    checkRect(rec.zoomRects.value(0), QRectF(400, 400, 800, 600),
              "反向拖动得到**同一个**矩形（归一化生效）");

    // ③ 框选不受 Alt 影响
    //   "把这一块放大"没有反方向可言 —— 缩小到某一块是没有意义的操作。
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(400, 400)));
    t.mouseMove(evMove(QPointF(300, 250), QPointF(1200, 1000), /*alt=*/true));
    t.mouseRelease(evRelease(QPointF(300, 250), QPointF(1200, 1000), /*alt=*/true));
    checkEqInt(rec.zoomRects.size(), 1, "按住 Alt 仍然是框选缩放");
    checkRect(rec.zoomRects.value(0), QRectF(400, 400, 800, 600),
              "Alt 不改变框选矩形（框选没有反方向）");
}

// ===========================================================================
//  [10] 缩放：退化的框选回落成单击
// ===========================================================================
static void testZoomDegenerateMarquee()
{
    beginGroup("缩放：退化的框选回落成单击");

    ZoomTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    // 控件上拖够了阈值（否则压根进不了框选），但图像坐标只挪了半个像素。
    // 这在低缩放比下很常见：10% 缩放下拖 6 个屏幕像素 = 0.6 个图像像素。
    // ★ 交给 zoomToRect 会算出几千倍 ★ —— 必须挡在工具这一层。
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(1000.0, 1000.0)));
    t.mouseMove(evMove(QPointF(106, 100), QPointF(1000.5, 1000.0)));
    check(t.isMarqueeActive(), "控件距离够 → 已进入框选模式");
    check(t.marqueeImageRect().width() < 1.0,
          "但图像矩形宽不足 1 像素（退化了）");
    t.mouseRelease(evRelease(QPointF(106, 100), QPointF(1000.5, 1000.0)));

    checkEqInt(rec.zoomRects.size(), 0,
               "退化的框选**不**发出框选请求（否则会放大到几千倍）");
    // ★ 关键：不是"什么都不做" ★
    //   用户明明拖了一下，毫无反应会被当成卡死。
    checkEqInt(rec.zoomFactors.size(), 1, "退化的框选回落到单击缩放");
    checkEqInt(rec.zoomFactors.size() == 1 ? 1 : 0, 1,
               "回落之后确实产生了一次点击缩放");
    checkPoint(rec.zoomAnchors.value(0), QPointF(100, 100),
               "回落时锚点仍然用按下点");
    checkEqInt(t.marqueeZoomCount(), 0, "框选计数没有增加");
    checkEqInt(t.clickZoomCount(), 1, "点击计数加一");
    check(!t.isMarqueeActive(), "松手后框选状态已关掉");

    // 反面对照：图像矩形正好 1×1 像素时必须走框选
    rec.clear();
    t.mousePress(evPress(QPointF(100, 100), QPointF(1000.0, 1000.0)));
    t.mouseMove(evMove(QPointF(120, 120), QPointF(1001.0, 1001.0)));
    t.mouseRelease(evRelease(QPointF(120, 120), QPointF(1001.0, 1001.0)));
    checkEqInt(rec.zoomRects.size(), 1, "图像矩形 1×1 时走框选（边界是 >= 1）");
    checkEqInt(rec.zoomFactors.size(), 0, "此时不发点击缩放");
}

// ===========================================================================
//  [11] 缩放：叠加层的真实像素
// ===========================================================================
//  铁律：每条自绘管线都要有一条"量它画出来的像素"的断言。
//  ★ 这一组之所以能做，是因为 paintScreenOverlay 刻意不 gate 在 view 上 ★
//    如果那里写成 `if (!view) return;`（像 ColorPickerTool 那样），
//    本组就只能去构造一个真 CanvasView —— 那要链整个 ui/ 层。
static void testZoomOverlayPixels()
{
    beginGroup("缩放：框选叠加层的真实像素");

    ZoomTool t(nullptr, nullptr, nullptr);

    // ① 没在框选时，什么都不许画
    {
        QImage canvas(400, 300, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter p(&canvas);
        t.paintScreenOverlay(p, nullptr);
        p.end();

        int painted = 0;
        for (int y = 0; y < canvas.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(canvas.constScanLine(y));
            for (int x = 0; x < canvas.width(); ++x)
                if (qAlpha(row[x]) != 0)
                    ++painted;
        }
        checkEqInt(painted, 0, "没有框选时叠加层一个像素都不画");
    }

    // ② 框选中：量包围盒 + 量框内是否为空
    t.mousePress(evPress(QPointF(100, 80), QPointF(400, 320)));
    t.mouseMove(evMove(QPointF(300, 220), QPointF(1200, 880)));
    check(t.isMarqueeActive(), "已进入框选");

    const QRectF box = t.marqueeWidgetRect();

    QImage canvas(400, 300, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    {
        QPainter p(&canvas);
        // ★ 故意传 nullptr ★
        //   这一条同时断言了"叠加层不依赖 CanvasView 指针"这个刻意的设计，
        //   因为本组的所有像素断言都建立在它之上。
        t.paintScreenOverlay(p, nullptr);
        p.end();
    }

    int minX = canvas.width(), minY = canvas.height(), maxX = -1, maxY = -1, painted = 0;
    for (int y = 0; y < canvas.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(canvas.constScanLine(y));
        for (int x = 0; x < canvas.width(); ++x) {
            if (qAlpha(row[x]) == 0)
                continue;
            ++painted;
            minX = qMin(minX, x); maxX = qMax(maxX, x);
            minY = qMin(minY, y); maxY = qMax(maxY, y);
        }
    }

    check(painted > 0, "框选时叠加层确实画出了东西（传 nullptr 也能画）");
    check(maxX >= minX && maxY >= minY, "画出来的像素构成一个非空包围盒");

    // 包围盒必须贴着拖动框（黑白双线，允许抗锯齿外溢几个像素）
    check(minX <= int(box.left()) && minX >= int(box.left()) - 3,
          "包围盒左边界贴住框的左边");
    check(minY <= int(box.top()) && minY >= int(box.top()) - 3,
          "包围盒上边界贴住框的上边");
    check(maxX >= int(box.right()) && maxX <= int(box.right()) + 3,
          "包围盒右边界贴住框的右边");
    check(maxY >= int(box.bottom()) && maxY <= int(box.bottom()) + 3,
          "包围盒下边界贴住框的下边");

    // ★ 必须是**空心**的 ★
    //   画成实心块的话上面那四条包围盒断言照样通过 ——
    //   只有量"框内部"才能把实心与空心分开。
    const int cx = int(box.center().x());
    const int cy = int(box.center().y());
    const QRgb mid = reinterpret_cast<const QRgb *>(canvas.constScanLine(cy))[cx];
    checkEqInt(qAlpha(mid), 0, "框的内部是空的（画的是空心框，不是实心块）");

    // 框线上必须有不透明像素
    const int edgeY = int(box.top());
    const int edgeX = int(box.left()) + 5;
    const QRgb onEdge = reinterpret_cast<const QRgb *>(canvas.constScanLine(edgeY))[edgeX];
    check(qAlpha(onEdge) > 0, "框的上边线上有不透明像素");

    // ③ cancel 之后不许再画
    t.cancel();
    {
        QImage c2(400, 300, QImage::Format_ARGB32_Premultiplied);
        c2.fill(Qt::transparent);
        QPainter p(&c2);
        t.paintScreenOverlay(p, nullptr);
        p.end();

        int again = 0;
        for (int y = 0; y < c2.height(); ++y) {
            const QRgb *row = reinterpret_cast<const QRgb *>(c2.constScanLine(y));
            for (int x = 0; x < c2.width(); ++x)
                if (qAlpha(row[x]) != 0)
                    ++again;
        }
        checkEqInt(again, 0, "cancel() 之后叠加层不再画任何像素");
    }
}

// ===========================================================================
//  [12] 缩放：cancel 与状态复位
// ===========================================================================
static void testZoomCancel()
{
    beginGroup("缩放：cancel 复位");

    ZoomTool t(nullptr, nullptr, nullptr);
    Recorder rec;
    rec.attach(&t);

    t.mousePress(evPress(QPointF(100, 100), QPointF(400, 400)));
    t.mouseMove(evMove(QPointF(300, 250), QPointF(1200, 1000)));
    check(t.isMarqueeActive(), "框选进行中");
    check(t.isBusy(), "框选进行中 isBusy 为真");

    t.cancel();

    check(!t.isMarqueeActive(), "cancel 关掉框选状态");
    check(!t.isBusy(), "cancel 清掉 busy 标志");
    check(t.marqueeImageRect().isEmpty(), "cancel 之后框选矩形是空的");
    check(t.marqueeWidgetRect().isEmpty(), "cancel 之后控件矩形也是空的");

    // cancel 之后再松手，什么都不该发生
    //   （m_pressed 已经清了，所以 release 走的是"我没在处理"那条路）
    rec.clear();
    const ToolResult r = t.mouseRelease(evRelease(QPointF(300, 250), QPointF(1200, 1000)));
    check(!r.handled, "cancel 之后的松手不被消费");
    checkEqInt(rec.zoomRects.size(), 0, "cancel 之后松手不发框选请求");
    checkEqInt(rec.zoomFactors.size(), 0, "cancel 之后松手也不发点击缩放");

    // 取消之后重新开始必须干净
    rec.clear();
    t.mousePress(evPress(QPointF(10, 10), QPointF(10, 10)));
    t.mouseRelease(evRelease(QPointF(10, 10), QPointF(10, 10)));
    checkEqInt(rec.zoomFactors.size(), 1, "cancel 之后重新点击仍然正常工作");
}

// ===========================================================================
//  [13] 两个工具都不碰文档
// ===========================================================================
//  ★ 这是它们最重要的性质，也是唯一能用"看数据"来证明的一条 ★
//    前面所有断言都只看了信号；这一组看的是**文档本身有没有被动过**。
static void testDocumentUntouched()
{
    beginGroup("抓手 / 缩放：一个像素、一条命令都不产生");

    const QSize kSize(64, 48);
    Document doc(kSize, Qt::transparent);
    CommandStack stack(&doc);

    // 铺一点有辨识度的内容，便于逐字节比对
    QImage src(kSize, QImage::Format_ARGB32_Premultiplied);
    src.fill(Qt::transparent);
    for (int y = 0; y < src.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(src.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
            row[x] = qRgba(255, (x * 3) % 256, (y * 5) % 256, 255);
    }
    doc.activeLayer()->setImage(src);
    doc.markDirty();

    const QImage before = doc.activeLayer()->image().copy();
    const int beforeCount = static_cast<int>(stack.undoStack()->count());

    HandTool hand(&doc, &stack, nullptr);
    hand.mousePress(evPress(QPointF(10, 10), QPointF(10, 10)));
    hand.mouseMove(evMove(QPointF(40, 60), QPointF(40, 60)));
    hand.mouseMove(evMove(QPointF(0, 0), QPointF(0, 0)));
    hand.mouseRelease(evRelease(QPointF(0, 0), QPointF(0, 0)));

    ZoomTool zoom(&doc, &stack, nullptr);
    zoom.mousePress(evPress(QPointF(20, 20), QPointF(20, 20)));
    zoom.mouseRelease(evRelease(QPointF(20, 20), QPointF(20, 20)));
    zoom.mousePress(evPress(QPointF(5, 5), QPointF(5, 5)));
    zoom.mouseMove(evMove(QPointF(45, 35), QPointF(45, 35)));
    zoom.mouseRelease(evRelease(QPointF(45, 35), QPointF(45, 35)));

    checkEqInt(static_cast<int>(stack.undoStack()->count()), beforeCount,
               "跑完两个工具的完整事件流，撤销栈一条命令都没多");
    check(beforeCount == 0, "（基线也是 0，所以上面那条不是「本来就有」）");

    const QImage after = doc.activeLayer()->image();
    bool identical = (after.size() == before.size())
                  && (after.format() == before.format());
    if (identical) {
        for (int y = 0; y < before.height() && identical; ++y) {
            const QRgb *rb = reinterpret_cast<const QRgb *>(before.constScanLine(y));
            const QRgb *ra = reinterpret_cast<const QRgb *>(after.constScanLine(y));
            for (int x = 0; x < before.width(); ++x) {
                if (rb[x] != ra[x]) {
                    identical = false;
                    break;
                }
            }
        }
    }
    check(identical, "图层像素逐字节不变（两个工具都不写像素）");
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char **argv)
{
    // offscreen：无显示器环境也能跑（ToolOptions.cpp 链了 QtWidgets）
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testIdentity();
    testHandCursor();
    testHandDelta();
    testHandIndependentOfZoom();
    testHandRefusals();
    testZoomClick();
    testZoomAlt();
    testZoomThreshold();
    testZoomMarqueeRect();
    testZoomDegenerateMarquee();
    testZoomOverlayPixels();
    testZoomCancel();
    testDocumentUntouched();

    // ★ 这一行的格式与其它测试保持一致（"通过 N 项，失败 M 项"）★
    //   植入验证脚本按这个模式解析结果 —— 改了它，所有植入验证都会
    //   因为"解析不出来"而集体失效（表现为 (-1,-1)，看着像脚本坏了）。
    std::printf("\n================================================\n");
    std::printf("  hand_zoom_test：通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
