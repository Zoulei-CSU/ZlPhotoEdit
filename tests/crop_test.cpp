// ============================================================================
//  crop_test.cpp —— 裁剪工具的离线自检
// ----------------------------------------------------------------------------
//  为什么裁剪工具需要单独一组测试：
//
//    它的错误全是"编译通过、程序不崩、功能看起来还在"的类型，
//    而错的是用户最在意的两件事：**框在哪** 与 **裁出来多大**。
//
//      ① **拖出的矩形与鼠标位置不符**（比例约束、象限符号、钳制方向写反）
//         症状：拖出来的框跑到鼠标的另一个方向去了，或者贴着边时突然变形。
//         这类错误在任何缩放级别下都只能靠眼睛发现，代码里没有任何东西会报错。
//
//      ② **浮点框 → 像素矩形差一个像素**（最隐蔽）
//         标签上写"800 × 600"，裁出来是 799 × 600。用户无法理解，
//         而 100% 之外的任何缩放级别都会产生非整数坐标（33% 下拖 1200 个
//         屏幕像素 = 3636.4 个图像像素），所以这是**每次缩放后拖框**都会
//         碰到的问题，不是理论边角料。
//         本测试用一条不变量钉它：**宽为整数 w 的框必须给出 w 个像素**，
//         与框落在哪个小数坐标无关。
//
//      ③ **遮罩画反了**（框内被压暗、框外亮着）
//         这是"数学全对、画面全错"的典型。本测试**量它画出来的像素**：
//         把 paintOverlay 渲到一张离屏图上，逐点检查框外变暗、框内不变暗。
//
//  ★ 关于画布尺寸与采样区 ★
//    画布用 200 × 160，是为了给尺寸标签留出空间：标签画在框的左上角上方，
//    若框贴着画布顶边就会被挤进框内，那样"框内必须亮"的断言就没法干净地
//    划出一块采样区。这是判据的代价，不是工具的问题。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 crop 分支。需要 QApplication（要跑 QPainter 与字体），
//    用 offscreen 平台插件在无显示器环境里跑。
// ============================================================================

#include "tools/CropTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include <cmath>
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

static void checkRect(const QRectF &actual, const QRectF &expected, const char *what)
{
    const qreal d = qAbs(actual.x() - expected.x())
                  + qAbs(actual.y() - expected.y())
                  + qAbs(actual.width()  - expected.width())
                  + qAbs(actual.height() - expected.height());
    if (d > 1e-6) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 (%.2f, %.2f, %.2f, %.2f)，"
                    "实际 (%.2f, %.2f, %.2f, %.2f)\n", what,
                    expected.x(), expected.y(), expected.width(), expected.height(),
                    actual.x(), actual.y(), actual.width(), actual.height());
    } else {
        ++g_passed;
    }
}

static void checkRectI(const QRect &actual, const QRect &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 (%d, %d, %d × %d)，"
                    "实际 (%d, %d, %d × %d)\n", what,
                    expected.x(), expected.y(), expected.width(), expected.height(),
                    actual.x(), actual.y(), actual.width(), actual.height());
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  构造辅助
// ---------------------------------------------------------------------------

static const int kCanvasW = 200;
static const int kCanvasH = 160;

static Document *makeDoc(int w = kCanvasW, int h = kCanvasH)
{
    auto *doc = new Document(QSize(w, h), Qt::transparent);
    Layer *layer = doc->activeLayer();
    if (layer) {
        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::white);
        layer->setImage(img);
        doc->markDirty();
    }
    return doc;
}

// 造一个"有活动选区"的文档。选区必须先 prepareCanvas 才能建立
// （见 Selection::prepareCanvas 的注释：没准备过的选区会静默地什么都不做）。
static void setSelection(Document *doc, const QRect &r)
{
    doc->selection().prepareCanvas(doc->canvasSize());
    doc->selection().selectRect(r);
}

static ToolEvent makeEvent(ToolEventType type, qreal x, qreal y, qreal zoom = 1.0)
{
    ToolEvent e;
    e.type     = type;
    e.button   = (type == ToolEventType::Release) ? Qt::NoButton : Qt::LeftButton;
    e.buttons  = (type == ToolEventType::Release) ? Qt::NoButton : Qt::LeftButton;
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    e.viewZoom = zoom;
    return e;
}

// 走一遍完整的"按下 → 拖动 → 松开"。这是工具真实的输入路径，
// 不绕过状态机的任何一个分支。
static void dragTool(CropTool *tool, qreal x0, qreal y0, qreal x1, qreal y1,
                     bool shift = false, qreal zoom = 1.0)
{
    ToolEvent press = makeEvent(ToolEventType::Press, x0, y0, zoom);
    press.modifiers.shift = shift;
    tool->mousePress(press);

    ToolEvent move = makeEvent(ToolEventType::Move, x1, y1, zoom);
    // 拖动过程中的修饰键才是"几何约束"的依据（与选区工具同一套约定）
    move.modifiers.shift = shift;
    tool->mouseMove(move);

    tool->mouseRelease(makeEvent(ToolEventType::Release, x1, y1, zoom));
}

// ===========================================================================
//  [1] 身份与参数
// ===========================================================================

static void testIdentity()
{
    beginGroup("裁剪工具的身份证");

    Document *doc = makeDoc();
    CropTool tool(doc, nullptr);

    check(tool.id() == QStringLiteral("crop"), "id 是 crop（QSettings 键名，定了就不能改）");
    check(tool.shortcutKey() == Qt::Key_C, "快捷键是 C（对齐 Photoshop）");
    check(!tool.displayName().isEmpty(), "有显示名");
    check(tool.options() != nullptr, "有参数对象（选项条要用）");

    // 选项条上的比例与网格必须真的能读到 —— 否则工具会退化成"永远自由比例"。
    auto *opts = qobject_cast<CropOptions *>(tool.options());
    check(opts != nullptr, "参数对象就是 CropOptions");

    delete doc;
}

// ===========================================================================
//  [2] 手柄命中
// ===========================================================================
//  两条契约：
//    · 8 个手柄各自命中自己（角柄优先于边柄 —— 它们的可点区域会重叠）
//    · 可点区域是**屏幕**尺寸恒定的：4 个屏幕像素的偏差在 100% 下算命中，
//      在 800% 下就不该命中（否则放大后整个框内都成了手柄，拖不动）

static void testHitTest()
{
    beginGroup("手柄命中");

    Document *doc = makeDoc(100, 80);
    CropTool tool(doc, nullptr);
    tool.cancel();   // 触发一次播种（无选区 → 整幅画布）

    // 框 = (0,0,100,80)，所以 right() == 100、bottom() == 80
    check(tool.hitTest(QPointF(0, 0), 1.0)     == CropTool::Handle::TopLeft,     "左上角柄");
    check(tool.hitTest(QPointF(100, 0), 1.0)   == CropTool::Handle::TopRight,    "右上角柄");
    check(tool.hitTest(QPointF(0, 80), 1.0)    == CropTool::Handle::BottomLeft,  "左下角柄");
    check(tool.hitTest(QPointF(100, 80), 1.0)  == CropTool::Handle::BottomRight, "右下角柄");
    check(tool.hitTest(QPointF(50, 0), 1.0)    == CropTool::Handle::Top,         "上边柄");
    check(tool.hitTest(QPointF(50, 80), 1.0)   == CropTool::Handle::Bottom,      "下边柄");
    check(tool.hitTest(QPointF(0, 40), 1.0)    == CropTool::Handle::Left,        "左边柄");
    check(tool.hitTest(QPointF(100, 40), 1.0)  == CropTool::Handle::Right,       "右边柄");
    check(tool.hitTest(QPointF(50, 40), 1.0)   == CropTool::Handle::Inside,
          "框中心落在框内，不是手柄");

    // ★ 可点区域随缩放变化 ★
    //   同一个图像坐标点 (4,4)：100% 下离左上角柄只有 4 个屏幕像素（算命中），
    //   800% 下等于 32 个屏幕像素（早出了手柄，落回框内）。
    check(tool.hitTest(QPointF(4, 4), 1.0) == CropTool::Handle::TopLeft,
          "100%：离角柄 4 像素仍算命中（可点区域够大）");
    check(tool.hitTest(QPointF(4, 4), 8.0) == CropTool::Handle::Inside,
          "800%：同样的图像坐标已经出了手柄，落回框内");

    delete doc;
}

// ===========================================================================
//  [3] 拖出新框（dragNewRect）
// ===========================================================================

static void testDragNewRect()
{
    beginGroup("拖出新框");

    const QRectF bounds(0, 0, 100, 80);

    // ── 自由比例：四个方向拖出的框必须完全一致 ──
    //   这是"象限符号写反"的守门人。写反的典型症状是拖右下角却框到了左上角。
    const QRectF expect(20, 20, 40, 30);
    checkRect(CropTool::dragNewRect(bounds, QPointF(20, 20), QPointF(60, 50), 0.0, false),
              expect, "向右下拖");
    checkRect(CropTool::dragNewRect(bounds, QPointF(60, 50), QPointF(20, 20), 0.0, false),
              expect, "向左上拖（与上一行结果必须相同）");
    checkRect(CropTool::dragNewRect(bounds, QPointF(60, 20), QPointF(20, 50), 0.0, false),
              expect, "向左下拖");
    checkRect(CropTool::dragNewRect(bounds, QPointF(20, 50), QPointF(60, 20), 0.0, false),
              expect, "向右上拖");

    // ── 起点在画布外：先被钳进画布，再拖 ──
    checkRect(CropTool::dragNewRect(bounds, QPointF(-30, -30), QPointF(40, 40), 0.0, false),
              QRectF(0, 0, 40, 40), "起点在画布外时先钳进画布");

    // ── 终点在画布外：钳到边界 ──
    checkRect(CropTool::dragNewRect(bounds, QPointF(20, 20), QPointF(400, 400), 0.0, false),
              QRectF(20, 20, 80, 60), "终点超出画布时钳到边界");

    // ── 正方形（Shift）──
    checkRect(CropTool::dragNewRect(bounds, QPointF(10, 10), QPointF(60, 30), 0.0, true),
              QRectF(10, 10, 50, 50), "Shift 拖出正方形（以较长的那一边为准）");

    // ── 比例预设 ──
    //   1:1 且按住 Shift 时结果一致，说明两条路径没有分叉
    checkRect(CropTool::dragNewRect(bounds, QPointF(10, 10), QPointF(60, 30), 1.0, false),
              QRectF(10, 10, 50, 50), "比例 1:1 与 Shift 结果一致");

    // ★ 以"较大的方向"为准 ★
    //   鼠标横向只走了 30、纵向走了 50 时，框必须按 50 那一维算 ——
    //   否则用户会觉得"我明明往下拖了，框却没长"。
    checkRect(CropTool::dragNewRect(bounds, QPointF(0, 0), QPointF(30, 50), 1.0, false),
              QRectF(0, 0, 50, 50), "1:1 时以较大的那一边为准（横向 30 < 纵向 50）");

    // ★ 越界时等比收缩，而不是截断 ★
    //   截断会把比例改掉，用户看到的是"框突然变形了"。
    //   锚点 (80,70)，拖到 (95,75)：1:1 会算出 15×15，但向下只剩 10 像素，
    //   于是整体收缩成 10×10 —— 宽高仍然相等，且完全落在画布内。
    const QRectF shrunk = CropTool::dragNewRect(bounds, QPointF(80, 70),
                                                QPointF(95, 75), 1.0, false);
    check(qAbs(shrunk.width() - shrunk.height()) < 1e-6,
          "越界收缩后宽高比仍是 1:1（不是截断）");
    checkEqInt(int(qRound(shrunk.width())), 10, "收缩到向下装得下的 10 像素");
    check(bounds.contains(shrunk), "收缩后仍然完全落在画布内");

    // 16:9 —— 用一个不像 1:1 的比例确认真的按 ratio 走
    const qreal r169 = 16.0 / 9.0;
    const QRectF wide = CropTool::dragNewRect(bounds, QPointF(0, 0), QPointF(32, 18),
                                              r169, false);
    check(qAbs(wide.width() / wide.height() - r169) < 1e-6, "16:9 的宽高比正确");
    checkEqInt(int(qRound(wide.width())), 32, "16:9：宽跟手（32）");
}

// ===========================================================================
//  [4] 拖手柄调整（resizeRect）
// ===========================================================================

static void testResizeRect()
{
    beginGroup("拖手柄调整");

    const QRectF bounds(0, 0, 100, 80);
    const QRectF start(20, 20, 40, 30);   // left 20, top 20, right 60, bottom 50

    // ── 自由比例：只有被拖的那条边动 ──
    QRectF r = CropTool::resizeRect(bounds, start, CropTool::Handle::Right,
                                    QPointF(80, 999), 0.0, false);
    checkRect(r, QRectF(20, 20, 60, 30), "拖右柄只改右边（纵向的越界坐标不影响上下边）");

    r = CropTool::resizeRect(bounds, start, CropTool::Handle::TopLeft,
                             QPointF(10, 10), 0.0, false);
    checkRect(r, QRectF(10, 10, 50, 40), "拖左上角柄时右下角固定不动");

    r = CropTool::resizeRect(bounds, start, CropTool::Handle::Bottom,
                             QPointF(999, 70), 0.0, false);
    checkRect(r, QRectF(20, 20, 40, 50), "拖下边柄时左右边不动");

    // ── 最小尺寸：拖过头不能让框塌成 0 ──
    r = CropTool::resizeRect(bounds, start, CropTool::Handle::Right,
                             QPointF(21, 40), 0.0, false);
    checkEqInt(int(qRound(r.width())), 1, "右柄拖过头时至少留 1 像素宽");

    // ★ 不能翻面 ★
    //   把左柄一路拖到右边之外：结果必须停在"宽 = 1"处，
    //   而不是变成一个宽为负、方向反过来的框。
    r = CropTool::resizeRect(bounds, start, CropTool::Handle::Left,
                             QPointF(200, 40), 0.0, false);
    check(r.width() >= 0.0, "左柄拖过头时框不会翻面（宽度非负）");
    checkEqInt(int(qRound(r.left())), 59, "左柄停在紧贴右边的前一个像素");

    // ── 不是手柄时原样返回 ──
    checkRect(CropTool::resizeRect(bounds, start, CropTool::Handle::Inside,
                                   QPointF(80, 70), 0.0, false),
              start, "Handle::Inside 不是手柄，原样返回");

    // ── 比例约束：角柄固定对角 ──
    r = CropTool::resizeRect(bounds, start, CropTool::Handle::BottomRight,
                             QPointF(80, 70), 1.0, false);
    checkRect(r, QRectF(20, 20, 60, 60), "1:1 拖右下角柄：左上角固定、宽高相等");

    // ★ 比例约束：边柄在垂直方向上居中扩张 ★
    //   拖右柄时框会变高，但**中心线不动** —— 与 Photoshop 一致。
    //   若实现成"下端固定"，用户会看到框一边长一边往下爬。
    r = CropTool::resizeRect(bounds, start, CropTool::Handle::Right,
                             QPointF(70, 999), 1.0, false);
    checkEqInt(int(qRound(r.width())), 50, "1:1 拖右柄：宽跟手");
    check(qAbs(r.center().y() - start.center().y()) < 1e-6,
          "★ 1:1 拖右柄：纵向中心线保持不动（居中扩张）");

    // ── 比例约束下也不能越界 ──
    //   右下角柄拖到画布角落：锚点 (20,20)，可用空间是 (80, 60)，
    //   1:1 只能取 60 —— 若忘了收缩就会出现一个 80×80 的框伸出画布。
    r = CropTool::resizeRect(bounds, start, CropTool::Handle::BottomRight,
                             QPointF(999, 999), 1.0, false);
    check(bounds.contains(r), "比例约束下拖到画布角落也不会越界");
    check(qAbs(r.width() - r.height()) < 1e-6, "越界收缩后比例仍然正确");
}

// ===========================================================================
//  [5] 整框平移（moveRect）
// ===========================================================================

static void testMoveRect()
{
    beginGroup("整框平移");

    const QRectF bounds(0, 0, 100, 80);
    const QRectF start(20, 20, 40, 30);

    checkRect(CropTool::moveRect(bounds, start, QPointF(10, 10)),
              QRectF(30, 30, 40, 30), "平移 10 像素");

    // ★ 整框钳制 ★
    //   框一旦被推出画布，用户就再也抓不到它了（框外按下是"新建"，
    //   而新建需要一次完整的拖动）。所以宁可让它贴着边走。
    checkRect(CropTool::moveRect(bounds, start, QPointF(-100, 0)),
              QRectF(0, 20, 40, 30), "向左拖出画布：贴着左边停下");
    checkRect(CropTool::moveRect(bounds, start, QPointF(0, -100)),
              QRectF(20, 0, 40, 30), "向上拖出画布：贴着上边停下");
    checkRect(CropTool::moveRect(bounds, start, QPointF(1000, 1000)),
              QRectF(60, 50, 40, 30), "向右下拖出画布：贴着右下停下");

    // 平移不改变尺寸 —— 这条最容易被"顺手钳制"的写法破坏
    const QRectF moved = CropTool::moveRect(bounds, start, QPointF(-1000, -1000));
    check(qAbs(moved.width() - start.width()) < 1e-6 &&
          qAbs(moved.height() - start.height()) < 1e-6,
          "钳制之后尺寸一个像素都没变");
    checkRect(moved, QRectF(0, 0, 40, 30), "向左上拖到底：停在左上角");
}

// ===========================================================================
//  [6] 播种：框从哪里来
// ===========================================================================

static void testSeeding()
{
    beginGroup("初始框的播种");

    // ── 没有选区 → 整幅画布 ──
    {
        Document *doc = makeDoc(100, 80);
        CropTool tool(doc, nullptr);

        check(!tool.isSeeded(), "刚造出来时还没有播种（第一次需要画框才播种）");
        tool.cancel();
        check(tool.isSeeded(), "cancel() 之后已播种");
        checkRectI(tool.currentRect(), QRect(0, 0, 100, 80), "无选区时播种为整幅画布");
        delete doc;
    }

    // ── 有选区 → 选区包围盒 ──
    //   用户已经用选框表达过"我要这一块"，再让他拿裁剪工具重框一遍是重复劳动。
    {
        Document *doc = makeDoc(100, 80);
        setSelection(doc, QRect(10, 12, 20, 25));

        CropTool tool(doc, nullptr);
        tool.cancel();
        checkRectI(tool.currentRect(), QRect(10, 12, 20, 25),
                   "有选区时播种为选区包围盒");
        delete doc;
    }

    // ── 画布尺寸变化 → 重新播种 ──
    //   上一次裁剪刚生效时就是这种情况：旧框已经不可能还在画布内。
    {
        Document *doc = makeDoc(100, 80);
        CropTool tool(doc, nullptr);
        dragTool(&tool, 20, 20, 60, 50);
        checkRectI(tool.currentRect(), QRect(20, 20, 40, 30), "先拖出一个框");

        doc->crop(QRect(20, 20, 40, 30));   // 模拟"上一次裁剪生效"
        // 下一次输入会触发重新播种
        tool.mousePress(makeEvent(ToolEventType::Press, 5, 5));
        tool.mouseRelease(makeEvent(ToolEventType::Release, 5, 5));
        checkRectI(tool.currentRect(), QRect(0, 0, 40, 30),
                   "画布变成 40×30 之后，框自动重新播种为新的整幅画布");
        delete doc;
    }
}

// ===========================================================================
//  [7] 交互：新建 / 平移 / 调整 / 点一下不算数
// ===========================================================================

static void testInteraction()
{
    beginGroup("画布上的交互");

    Document *doc = makeDoc();
    CropTool tool(doc, nullptr);

    // ── 框 = 整幅画布时，框内按下即拉新框 ──
    //
    // ★ 这条规则很重要 ★
    //   默认（没有选区）时框就是整幅画布，画布上**没有"框外"可以按**。
    //   若此时判定成"平移整框"，用户第一次在画布上拖动会毫无反应 ——
    //   因为整幅画布已经被钳制住，往哪儿都推不动。看起来就像工具坏了。
    dragTool(&tool, 20, 20, 60, 50);
    checkRectI(tool.currentRect(), QRect(20, 20, 40, 30),
               "整幅画布上拖动 → 拉出新框");

    // ── 框内拖动 = 平移 ──
    dragTool(&tool, 30, 30, 40, 40);
    checkRectI(tool.currentRect(), QRect(30, 30, 40, 30), "框内拖动 → 整框平移");

    // ── 拖手柄 = 调整 ──
    //   当前框 (30,30,40,30) 的右下角是 (70,60)
    dragTool(&tool, 70, 60, 90, 80);
    checkRectI(tool.currentRect(), QRect(30, 30, 60, 50), "拖右下角柄 → 左上角固定");

    // ── 点一下不拖动 → 还原 ──
    //   若不复原，一次误点就把用户调好的框塌成零尺寸，遮罩还会盖满整幅画布。
    const QRect before = tool.currentRect();
    tool.mousePress(makeEvent(ToolEventType::Press, 5, 5));   // 框外
    tool.mouseRelease(makeEvent(ToolEventType::Release, 5, 5));
    checkRectI(tool.currentRect(), before, "框外点一下（没拖动）→ 原框还原，不塌陷");

    // ── 切换工具 → 未确认的框被丢弃 ──
    //   commit() 绝不能顺手裁一刀：用户当时什么都没确认。
    tool.commit();
    check(!tool.isSeeded(), "commit() 之后回到未播种状态（未确认的框丢弃）");

    delete doc;
}

// ===========================================================================
//  [8] 提交与复位
// ===========================================================================

static void testCommitAndCancel()
{
    beginGroup("Enter 提交 / Esc 复位");

    // ── 提交：矩形必须与框完全一致 ──
    {
        Document *doc = makeDoc();
        CropTool tool(doc, nullptr);

        QRect got;
        int emitted = 0;
        QObject::connect(&tool, &CropTool::cropRequested, &tool,
                         [&](const QRect &r) { got = r; ++emitted; });

        dragTool(&tool, 40, 30, 140, 110);

        ToolEvent enter = makeEvent(ToolEventType::KeyPress, 0, 0);
        enter.key = Qt::Key_Return;
        const ToolResult res = tool.keyPress(enter);

        checkEqInt(emitted, 1, "Enter 发出了 cropRequested");
        check(res.handled, "Enter 被工具消费（不再传给画布）");
        checkRectI(got, QRect(40, 30, 100, 80), "提交的矩形与框一致");

        // ── 小键盘 Enter 同样有效 ──
        emitted = 0;
        ToolEvent enterNum = makeEvent(ToolEventType::KeyPress, 0, 0);
        enterNum.key = Qt::Key_Enter;
        tool.keyPress(enterNum);
        checkEqInt(emitted, 1, "小键盘 Enter 同样提交");

        delete doc;
    }

    // ── 框 = 整幅画布时不提交 ──
    //   那种情况下 Document::crop() 会返回 false，若还是压一条命令，
    //   历史面板里就会多出一条"什么都没改"的记录。
    {
        Document *doc = makeDoc(100, 80);
        CropTool tool(doc, nullptr);

        int emitted = 0;
        QObject::connect(&tool, &CropTool::cropRequested, &tool,
                         [&](const QRect &) { ++emitted; });

        // 先播种成整幅画布（无选区）
        ToolEvent enter = makeEvent(ToolEventType::KeyPress, 0, 0);
        enter.key = Qt::Key_Return;
        tool.keyPress(enter);
        checkEqInt(emitted, 0, "框就是整幅画布时不提交（等于没裁）");

        delete doc;
    }

    // ── Esc（走 cancel()）→ 复位 ──
    {
        Document *doc = makeDoc(100, 80);
        CropTool tool(doc, nullptr);

        dragTool(&tool, 20, 20, 60, 50);
        checkRectI(tool.currentRect(), QRect(20, 20, 40, 30), "先拖出一个框");

        tool.cancel();
        checkRectI(tool.currentRect(), QRect(0, 0, 100, 80), "cancel() 复位为整幅画布");
        check(!tool.isBusy(), "cancel() 清掉了忙标志");

        delete doc;
    }

    // ── 有选区时 cancel() 复位到选区 ──
    {
        Document *doc = makeDoc(100, 80);
        setSelection(doc, QRect(5, 5, 30, 30));
        CropTool tool(doc, nullptr);

        dragTool(&tool, 60, 60, 90, 70);
        tool.cancel();
        checkRectI(tool.currentRect(), QRect(5, 5, 30, 30),
                   "有选区时 cancel() 复位到选区包围盒（与播种同源）");

        delete doc;
    }

    // ── 方向键微调 ──
    {
        Document *doc = makeDoc(100, 80);
        CropTool tool(doc, nullptr);
        dragTool(&tool, 20, 20, 60, 50);

        ToolEvent right = makeEvent(ToolEventType::KeyPress, 0, 0);
        right.key = Qt::Key_Right;
        tool.keyPress(right);
        checkRectI(tool.currentRect(), QRect(21, 20, 40, 30), "方向键平移 1 像素");

        right.modifiers.shift = true;
        tool.keyPress(right);
        checkRectI(tool.currentRect(), QRect(31, 20, 40, 30), "Shift + 方向键平移 10 像素");

        delete doc;
    }
}

// ===========================================================================
//  [9] 浮点框 → 像素矩形
// ===========================================================================
//  ★ 本组守的是"标签上写的尺寸 == 实际裁出来的尺寸" ★
//
//  判据是**像素中心落在框内**（见 CropTool::currentRect 的注释）。
//  用"相交"判据或 qRound 都会在非整数坐标下多算/少算一列 ——
//  而 100% 之外的每一档缩放都会拖出非整数坐标。

static void testPixelRect()
{
    beginGroup("浮点框 → 像素矩形");

    Document *doc = makeDoc();
    CropTool tool(doc, nullptr);

    // 整数坐标：宽 100 的框必须给出 100 个像素
    dragTool(&tool, 40, 30, 140, 110);
    checkRectI(tool.currentRect(), QRect(40, 30, 100, 80), "整数坐标：尺寸逐个对上");

    // ★ 小数坐标：宽度不变，像素个数也不能变 ★
    //   这才是真的判据 —— "宽 40 的框给出 40 列像素"，与它落在 20.0 还是
    //   20.4 无关。差一列的实现在这里必然暴露。
    dragTool(&tool, 20.4, 20.6, 60.4, 50.6);
    const QRect r = tool.currentRect();
    checkEqInt(r.width(),  40, "★ 小数坐标：宽 40.0 的框给出 40 列像素");
    checkEqInt(r.height(), 30, "★ 小数坐标：高 30.0 的框给出 30 行像素");
    checkEqInt(r.left(), 20, "左边界取距框最近的像素中心（20.4 → 20）");
    checkEqInt(r.top(),  21, "上边界同上（20.6 → 21）");

    // 另一半：向右偏 0.6，左上角应当落到下一个像素
    //
    // ★ 必须先 cancel() 复位，否则这一拖根本不是"拉新框" ★
    //   上一个框还停在 (20.4, 20.6, 40, 30)，而这次要按的点 (20.6, 20.6)
    //   距它的左上角只有 0.2 像素 —— 落在角柄的命中半径（4.5 + slack）里，
    //   于是命中测试返回 TopLeft，走的是"调手柄"：
    //   右下角被钉死在 (60.4, 50.6)，左上角却被拖到 (60.6, 50.6)，
    //   框被压成最小尺寸再钳制 —— 实测左边界跑出 59，与"拉新框"毫无关系。
    //
    //   这不是工具的 bug，而是"连续两次拖动会互相影响状态"的必然结果：
    //   工具的命中测试**应当**在角柄附近优先当调柄（Photoshop 也是这么做的）。
    //   所以想验证"拉新框"，就得先把框复位到整幅画布 ——
    //   复位后框覆盖整个画布，框内按下会走"拉新框"那条特例（见 mousePress）。
    tool.cancel();
    dragTool(&tool, 20.6, 20.6, 60.6, 50.6);
    checkEqInt(tool.currentRect().left(), 21, "起点 20.6 → 左边界是 21");

    // 顶到画布边缘时不能多算一列
    dragTool(&tool, 0.5, 0.5, 400, 400);   // 终点被钳到画布右下角
    const QRect full = tool.currentRect();
    checkEqInt(full.right(),  199, "右边界停在画布最后一列（不越出画布）");
    checkEqInt(full.bottom(), 159, "下边界停在画布最后一行（不越出画布）");
    checkEqInt(full.width(),  200, "贴着画布边缘时正好取满 200 列");

    delete doc;
}

// ===========================================================================
//  [10] 比例预设变化立刻作用到已有的框
// ===========================================================================
//  用户在选项条里选了 16:9，框却纹丝不动 —— 他会以为那个下拉框是坏的。
//  这条走的是 CropOptions::changed → applyRatioToCurrentRect() 的真实链路。

static void testRatioOption()
{
    beginGroup("比例预设作用于已有的框");

    Document *doc = makeDoc();
    CropTool tool(doc, nullptr);

    dragTool(&tool, 20, 20, 60, 50);   // 40 × 30
    checkRectI(tool.currentRect(), QRect(20, 20, 40, 30), "先拖出一个 40×30 的框");

    auto *opts = qobject_cast<CropOptions *>(tool.options());
    if (!opts) {
        check(false, "拿不到 CropOptions，本组无法继续");
        delete doc;
        return;
    }

    // 1:1 → 内切（只缩不放），中心不动
    opts->setPreset(AspectPreset::Square);
    QRect r = tool.currentRect();
    check(qAbs(r.width() - r.height()) <= 1, "选 1:1 之后宽高相等");
    checkEqInt(r.width(), 30, "内切到 1:1：取较短的一边（30）");
    checkRectI(r, QRect(25, 20, 30, 30), "内切时中心不动");

    // 切回"自由"：保留当前形状，不强行掰回去
    opts->setPreset(AspectPreset::Free);
    checkRectI(tool.currentRect(), QRect(25, 20, 30, 30), "切回自由：当前框原样保留");

    // 16:9 在一个很扁的框上：应当"收窄"（内切），而不是把高度撑上去
    dragTool(&tool, 5, 5, 120, 40);   // 115 × 35
    checkRectI(tool.currentRect(), QRect(5, 5, 115, 35), "拖出一个很扁的框");
    opts->setPreset(AspectPreset::SixteenNine);
    r = tool.currentRect();
    check(qAbs(r.width() / qreal(r.height()) - 16.0 / 9.0) < 0.06, "16:9 的比例正确");
    check(r.width() <= 115 && r.height() <= 35, "内切只缩不放");

    delete doc;
}

// ===========================================================================
//  [11] 渲染：量它画出来的像素
// ===========================================================================
//
//  ★ 为什么必须有这一组 ★
//    "遮罩画反了"是纯几何断言永远抓不到的错误：rect 算得完全正确，
//    但画出来框内是暗的、框外是亮的。本项目已经在两条自绘管线上各踩过一次
//    同类问题（drawComposite 的视图变换双施加、CanvasPreviewWidget 的
//    漏坐标映射），所以每写一个自绘控件就要有一条"量像素"的断言。
//
//    ★ 断言必须能抓住 bug —— 本组在提交前做过植入验证 ★
//      实测三次（每次都先跑红、再改回）：
//        ⓐ **遮罩画反**：删掉 `mask.addRect(bounds)`，只填框内。
//           → 报"框外未被压暗的像素 18385 个，首个在 (0,0)"
//             与"框内被压暗的像素 3311 个"，9 条断言变红。
//        ⓑ **框贴边不算点**：去掉 mousePress 里 rectCoversCanvas 那条特例。
//           → 首拖无反应，整条链路的框都停在整幅画布，20 条断言变红。
//        ⓒ **像素判据退化成"相交"**：currentRect 改用 floor/ceil。
//           → 报"宽 40.0 的框给出 40 列像素 期望 40，实际 41"，4 条变红。
//
//      ★ 一条反面教训 ★
//        最初这里写的是"把遮罩两个子路径对调"。那是**错的**：
//        奇偶填充只看穿过的次数，两个子路径谁先谁后结果完全相同 ——
//        照那个说法去做植入，会看到测试**依然全绿**，然后误判成
//        "断言抓不住 bug"。植入必须真的是能改变渲染的那种改动。

namespace {

// 扫描一块区域，统计不满足"亮度"条件的像素个数并累加进 scanned。
// firstBad 由调用方初始化，只在第一次违规时写入 —— 一块区域几百个像素
// 若逐个断言，通过数会虚高（自检总数里躺着几百条"每个像素都对"毫无信息量）。
int scanRegion(const QImage &img, int x0, int y0, int x1, int y1,
               bool wantBright, QPoint *firstBad, int *scanned)
{
    int bad = 0;
    for (int y = qMax(0, y0); y <= qMin(img.height() - 1, y1); ++y) {
        for (int x = qMax(0, x0); x <= qMin(img.width() - 1, x1); ++x) {
            ++(*scanned);
            const int red = qRed(img.pixel(x, y));
            const bool ok = wantBright ? (red >= 250) : (red < 200);
            if (!ok) {
                if (bad == 0 && firstBad->x() < 0)
                    *firstBad = QPoint(x, y);
                ++bad;
            }
        }
    }
    return bad;
}

// 渲一遍工具的叠加层（走与 CanvasView::drawToolOverlay 相同的调用方式：
// 抗锯齿打开、视图变换已经设好）
QImage renderOverlay(CropTool *tool, const QSize &size, qreal zoom)
{
    QImage canvas(size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::white);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(zoom, zoom);
    tool->paintOverlay(p, nullptr);
    p.end();
    return canvas;
}

} // namespace

static void testRendering()
{
    beginGroup("叠加层的真实像素");

    Document *doc = makeDoc();
    CropTool tool(doc, nullptr);

    // 框放在 (40,70)-(140,130)：上方留出足够空间给尺寸标签
    // （标签画在框顶再往上 20 来像素），于是"框外必须暗"的采样带可以
    // 干干净净地划出来，不会把标签里的白字算进去。
    dragTool(&tool, 40, 70, 140, 130);
    checkRectI(tool.currentRect(), QRect(40, 70, 100, 60), "框落在 (40,70) 100×60");

    const QImage img = renderOverlay(&tool, QSize(kCanvasW, kCanvasH), 1.0);

    QPoint bad(-1, -1);
    int scanned = 0;

    // ── 框外必须被压暗 ──
    //   四条带，都避开三样"本来就该亮"的东西：
    //     · 边框 1 像素（wx 在 40 / 140，wy 在 70 / 130）
    //     · 尺寸标签所在的横条（y ≈ 42..64，x ≥ 40）
    //     · ★ 角柄 ★ —— 直径 9 屏幕像素、以框角为中心，所以有 4.5 像素
    //       伸到框**外**。这是遮罩之外唯一允许亮的区域。
    //       实测踩过：不排除它，本断言会报"框外未被压暗的像素 45 个，
    //       首个在 (36, 133)" —— 那正是左下角柄伸出框外的那一块。
    //       角柄伸出去是对的（它要能被点到），错的是采样带没把它让开。
    //     上带 y 0..38      —— 标签从 y≈42 开始，角柄从 y≈65.5 开始
    //     下带 y 136..159   —— 角柄到 y≈134.5 + 抗锯齿
    //     左带 x 0..34      —— 角柄从 x≈35.5 开始
    //     右带 x 146..199   —— 角柄到 x≈144.5 + 抗锯齿
    int n = 0;
    n += scanRegion(img, 0, 0, kCanvasW - 1, 38, false, &bad, &scanned);
    n += scanRegion(img, 0, 136, kCanvasW - 1, kCanvasH - 1, false, &bad, &scanned);
    n += scanRegion(img, 0, 68, 34, 132, false, &bad, &scanned);
    n += scanRegion(img, 146, 68, kCanvasW - 1, 132, false, &bad, &scanned);

    check(scanned > 8000, "框外采样区够大（判据不是空转）");
    if (n != 0) {
        std::printf("         框外未被压暗的像素 %d 个，首个在 (%d, %d)\n",
                    n, bad.x(), bad.y());
    }
    checkEqInt(n, 0, "★ 框外的每一个像素都被遮罩压暗");

    // ── 框内（采样区）必须一点都没被压暗 ──
    //   采样区 x 50..130、y 78..122，并跳过三分法辅助线所在的行列
    //   （竖线 x≈73/107，横线 y≈90/110）与尺寸标签所在的横条（y ≤ 70）。
    //   四个角柄半径 4.5，够不到这里。
    int innerBad = 0;
    int innerTotal = 0;
    QPoint innerFirst(-1, -1);
    for (int y = 78; y <= 122; ++y) {
        if (y == 90 || y == 110) continue;                          // 三分法横线
        for (int x = 50; x <= 130; ++x) {
            if (x == 73 || x == 74 || x == 106 || x == 107) continue;  // 三分法竖线
            ++innerTotal;
            if (qRed(img.pixel(x, y)) < 250) {
                if (innerBad == 0)
                    innerFirst = QPoint(x, y);
                ++innerBad;
            }
        }
    }
    check(innerTotal > 2000, "框内采样区够大（判据不是空转）");
    if (innerBad != 0) {
        std::printf("         框内被压暗的像素 %d 个，首个在 (%d, %d)\n",
                    innerBad, innerFirst.x(), innerFirst.y());
    }
    checkEqInt(innerBad, 0, "★ 框内的像素一个都没有被压暗");

    // ── 角柄是白的 ──
    //
    // ★ 采样点为什么往框内让 2 像素 ★
    //   角柄的中心就在框的角上，而**边界那一列像素属于框外**：
    //   框的右边界是 rw.right() = 140，像素 (140, y) 横跨 [140, 141)，
    //   落在遮罩里；那个像素又同时吃到边框的描边（覆盖 [139.5, 140.5]）
    //   与半透明的角柄填充（alpha 235），三层叠完是 250 而不是 255。
    //   实测踩过：直接取 (140, 70) 与 (90, 130) 得到 250。
    //   往里让 2 像素既仍在 9 像素宽的角柄内、又完全落在框内（背景纯白），
    //   于是"角柄是白的"这条断言才是在测角柄，而不是在测遮罩的边界。
    checkEqInt(qRed(img.pixel(40, 70)),  255, "左上角柄是白的");
    checkEqInt(qRed(img.pixel(138, 70)), 255, "右上角柄是白的（往框内让 2px）");
    checkEqInt(qRed(img.pixel(90, 128)), 255, "下边柄是白的（往框内让 2px）");

    // ── 换一档缩放：遮罩跟着图像走，角柄不跟着放大 ──
    //   按 0.5 渲染：框落在屏幕 (20,35)-(70,65) 处，角柄仍然只有
    //   9 个屏幕像素宽 —— 若角柄被画进了图像坐标系，它会缩成 4.5 像素，
    //   下面这两条立刻失败。
    const QImage half = renderOverlay(&tool, QSize(kCanvasW / 2, kCanvasH / 2), 0.5);
    checkEqInt(qRed(half.pixel(20, 35)), 255, "50% 下左上角柄落在框的角上");
    checkEqInt(qRed(half.pixel(21, 36)), 255, "50% 下角柄仍是整块白（没被缩小成一条边）");
    checkEqInt(qRed(half.pixel(45, 50)), 255, "50% 下框中心仍然是亮的（遮罩没错位）");
    check(qRed(half.pixel(10, 50)) < 200, "50% 下框外仍然是暗的");

    delete doc;
}

// ===========================================================================

int main(int argc, char **argv)
{
    // ★ 必须在 QApplication 构造前设置 ★
    //   offscreen 插件让 Qt 在没有显示器的环境下也能用 QPainter、
    //   取到字体度量（尺寸标签要算 boundingRect）。
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 裁剪工具 离线自检\n");
    std::printf("  （平台插件：offscreen）\n");
    std::printf("================================================\n");

    testIdentity();
    testHitTest();
    testDragNewRect();
    testResizeRect();
    testMoveRect();
    testSeeding();
    testInteraction();
    testCommitAndCancel();
    testPixelRect();
    testRatioOption();
    testRendering();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
