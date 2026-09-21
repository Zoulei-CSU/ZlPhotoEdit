// ============================================================================
//  shape_tool_test.cpp —— 直线 / 矩形 / 椭圆 的离线自检
// ----------------------------------------------------------------------------
//  本组盯的是两类东西：
//
//    A. **光栅化的形状对不对**（engine/Shape）
//       蒙版盖住的是不是那条线、那个矩形、那个椭圆。
//       这一类错误在编译期、运行期都没有任何提示，症状只有一个：
//       "画出来的东西长得不对，但看起来又像那么回事"。
//       最容易漏的一条是"椭圆其实被画成了矩形"—— 用"填充后角上有没有
//       被覆盖"就能一眼分开两者（椭圆的角必然不覆盖，矩形的角必然覆盖）。
//
//    B. **拖动过程中那些"看不见的状态"**（tools/ShapeTool）
//       ① 旧预览有没有被擦掉
//          形状是"每次几何变化整体重画"，不是在旧结果上追加。
//          如果重算区域只取了本帧包围盒，上一个位置的形状会**留在画面上**，
//          用户拖动时会看到一串残影 —— 而所有"形状画对了没"的断言
//          都会通过（因为最新那一帧确实画对了）。
//          本组用"拖出去 → 拖回来"把这一点钉死。
//
//       ② 一次拖动 = 一条撤销记录
//          DrawCommand 的描边编号若撞号，QUndoStack 会静默 mergeWith，
//          两次操作被合并成一步（阶段六首批真踩过，见 DrawCommand.h）。
//          形状工具一次拖动里会调很多次"重算"，但**只能在松手时压一条**。
//          断言方式：走完整事件流后看 count() 的增量，而不是写死编号值。
//
//       ③ 取消（Esc）必须把像素还回去
//          拖动期间写的是**真实像素**，所以取消不能只是"丢掉那个矩形"，
//          必须把整段触及区域从落笔前快照恢复。漏了这一步的症状是
//          "按了 Esc，画的东西还在，而且撤不掉"。
//
//  ★ 关于测试用的配置 ★
//    一律把抗锯齿**关掉**、颜色用不透明色，描边给足宽度（≥3）。
//    这样"在线上/不在线上"是精确判定，不必迁就像素中心的边界情形。
//    抗锯齿另有专门的断言（斜线边缘必须出现 1~254 的中间值 ——
//    那是"抗锯齿真的生效了"的唯一直接证据）。
//
//  ★ 关于坐标 ★
//    rasterizeXxx 返回的蒙版是**局部坐标**的（尺寸 == bounds.size()）。
//    下面所有对蒙版的取样都写成 `图像坐标 - bounds.topLeft()`，
//    而不是预先算好的常数 —— 这样万一包围盒的取整规则变了，
//    断言表达的"这条线该覆盖哪里"依然成立。
//
//  ★ 运行方式 ★
//    见 run_tests.sh 的 shape 分支。需要 QApplication（ToolOptions.cpp
//    与工具都链 QtWidgets），用 offscreen 插件在无显示器环境里跑。
// ============================================================================

#include "tools/ShapeTool.h"
#include "tools/ToolOptions.h"
#include "tools/ToolEvent.h"
#include "engine/Shape.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "commands/CommandStack.h"

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
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

// 覆盖度落在 [lo, hi] 区间内。
//
// ★ 为什么不写 "== 255" ★
//   "被覆盖 / 没被覆盖"才是本组要表达的意思，具体灰度值（255 还是 252）
//   取决于光栅化细节。区间断言既表达了意图，又不会因为无关的取整而误报。
static void checkCoverage(const QImage &mask, int x, int y, int lo, int hi,
                          const char *what)
{
    if (mask.isNull() || mask.format() != QImage::Format_Grayscale8) {
        ++g_failed;
        std::printf("  [失败] %s  蒙版为空或格式不是 Grayscale8\n", what);
        return;
    }
    if (x < 0 || y < 0 || x >= mask.width() || y >= mask.height()) {
        ++g_failed;
        std::printf("  [失败] %s  取样点 (%d,%d) 在蒙版 %dx%d 之外"
                    "（蒙版是局部坐标，不能直接拿图像坐标来量）\n",
                    what, x, y, mask.width(), mask.height());
        return;
    }
    const int v = mask.constScanLine(y)[x];
    if (v < lo || v > hi) {
        ++g_failed;
        std::printf("  [失败] %s  期望覆盖度 %d..%d，实际 %d\n", what, lo, hi, v);
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  像素读写（与 tool_paint_test 同一套理由：一律走扫描线）
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

static QRgb layerPixel(const Document *doc, int layerIndex, int x, int y)
{
    const Layer *l = doc->layers().layer(layerIndex);
    if (!l || l->image().isNull())
        return 0;
    return rawPixel(l->image(), x, y);
}

static const QRgb kWhite = qRgba(255, 255, 255, 255);
static const QRgb kRed   = qRgba(255, 0, 0, 255);
static const QRgb kBlue  = qRgba(0, 0, 255, 255);
static const QRgb kClear = qRgba(0, 0, 0, 0);

static const int kW = 64;
static const int kH = 48;

// 单图层文档，活动图层整层写 fill（**直接写预乘字节**，理由见 tool_paint_test）
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

// ---------------------------------------------------------------------------
//  事件构造
// ---------------------------------------------------------------------------
//  ★ Release 事件的 button 必须是左键、buttons 必须是空 ★
//   真实 Qt 事件里：button() = "造成这次事件的那个键"，buttons() = "此刻仍按着的"。
//   写成 button=NoButton 会让工具的 `if (e.button != Qt::LeftButton) return;`
//   直接早退，状态机永远停不下来（症状："松手后还在继续画"）。
//   tool_paint_test 的注释里有这个坑的完整记录。
static ToolEvent mouseEvent(ToolEventType type, int x, int y, bool shift = false)
{
    ToolEvent e;
    e.type = type;
    e.button  = Qt::LeftButton;
    e.buttons = (type == ToolEventType::Release) ? Qt::NoButton : Qt::LeftButton;
    e.imagePos = QPointF(x, y);
    e.widgetPos = QPointF(x, y);
    e.lastImagePos = QPointF(x, y);
    e.viewZoom = 1.0;
    e.modifiers.shift = shift;
    return e;
}

// ---------------------------------------------------------------------------
//  工具配置辅助
// ---------------------------------------------------------------------------
// 把直线调成"判定最干脆"的一档：不透明色、无抗锯齿
static LineOptions *prepareLine(ShapeTool *tool, const QColor &color, int width = 1)
{
    auto *opts = qobject_cast<LineOptions *>(tool->options());
    if (!opts)
        return nullptr;
    opts->setColor(color);
    opts->setWidth(width);
    opts->setStyle(Qt::SolidLine);
    opts->setAntialias(false);
    return opts;
}

static ShapeOptions *prepareShape(ShapeTool *tool, ShapeMode mode,
                                  const QColor &stroke, const QColor &fill,
                                  int width = 3, int corner = 0)
{
    auto *opts = qobject_cast<ShapeOptions *>(tool->options());
    if (!opts)
        return nullptr;
    opts->setMode(mode);
    opts->setStrokeColor(stroke);
    opts->setFillColor(fill);
    opts->setWidth(width);
    opts->setCornerRadius(corner);
    opts->setAntialias(false);
    return opts;
}

// ===========================================================================
//  [1] 身份证
// ===========================================================================
static void testIdentity()
{
    beginGroup("三个 Kind 的身份证");

    Document *doc = makeDoc(kWhite);
    CommandStack stack(doc);

    ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
    check(line.id() == QStringLiteral("line"), "直线 id 是 line（QSettings 键）");
    check(line.shortcutKey() == Qt::Key_L, "直线快捷键 L");
    check(qobject_cast<LineOptions *>(line.options()) != nullptr,
          "直线拿到的参数对象是 LineOptions");
    check(qobject_cast<ShapeOptions *>(line.options()) == nullptr,
          "直线没有 ShapeOptions（两者不该同时存在）");

    ShapeTool rect(ShapeTool::Kind::Rectangle, doc, &stack);
    check(rect.id() == QStringLiteral("rect"),
          "矩形 id 是 rect（与 iconForTool 的映射键一致）");
    check(qobject_cast<ShapeOptions *>(rect.options()) != nullptr,
          "矩形拿到的参数对象是 ShapeOptions");

    ShapeTool ellipse(ShapeTool::Kind::Ellipse, doc, &stack);
    check(ellipse.id() == QStringLiteral("ellipse"),
          "椭圆 id 是 ellipse（与 iconForTool 的映射键一致）");

    // ★ 矩形与椭圆**故意**共用 U 键 ★
    //   轮换逻辑在 MainWindow::activateToolByShortcut。
    //   如果哪天有人"顺手"把椭圆改成别的键，这条会失败 —— 那正是
    //   需要一个失败的地方，因为轮换会静默失效（U 仍然能用，只是不轮换了）。
    checkEqInt(rect.shortcutKey(), Qt::Key_U, "矩形快捷键 U");
    checkEqInt(ellipse.shortcutKey(), Qt::Key_U,
               "椭圆也绑 U（一个键两个工具，轮换在 MainWindow 里做）");
    check(line.shortcutKey() != rect.shortcutKey(), "直线与形状不同键");

    check(!line.displayName().isEmpty() && !rect.displayName().isEmpty()
              && !ellipse.displayName().isEmpty(),
          "三个工具都有显示名");
    check(line.displayName() != rect.displayName()
              && rect.displayName() != ellipse.displayName(),
          "三个显示名互不相同（否则工具箱里分不出来）");

    delete doc;
}

// ===========================================================================
//  [2] 直线光栅化
// ===========================================================================
static void testLineRaster()
{
    beginGroup("直线：蒙版盖住的确实是那条线");

    const QSize canvas(kW, kH);

    // ── 水平线 (10,10) → (50,10)，宽 1 ──
    ShapeMask h = rasterizeLine(canvas, QPointF(10, 10), QPointF(50, 10),
                                1, Qt::SolidLine, false);
    check(!h.isEmpty(), "水平线产生了蒙版");
    check(h.strokeMask.width() == h.bounds.width()
              && h.strokeMask.height() == h.bounds.height(),
          "蒙版尺寸等于包围盒尺寸（局部坐标，不是整幅画布）");

    const int hx = h.bounds.x();
    const int hy = h.bounds.y();
    checkCoverage(h.strokeMask, 30 - hx, 10 - hy, 200, 255, "线中点被覆盖");
    checkCoverage(h.strokeMask, 30 - hx, 7 - hy, 0, 0,
                  "线的上方 3 像素处没被覆盖（线宽只有 1）");
    checkCoverage(h.strokeMask, 30 - hx, 12 - hy, 0, 0,
                  "线的下方 2 像素处没被覆盖");
    checkCoverage(h.strokeMask, 52 - hx, 10 - hy, 0, 0,
                  "端点之外 2 像素处没有像素（RoundCap 只到端点为止）");

    // 包围盒必须**包住**线，又不能夸张地大 —— 局部光栅化的意义就在这里
    check(h.bounds.width() >= 40 && h.bounds.width() <= 48,
          "包围盒宽度约等于线长（略大一点是端点与取整余量）");
    check(h.bounds.height() <= 6, "线宽 1 时包围盒高度很小");

    // ── 垂直线 ──
    ShapeMask v = rasterizeLine(canvas, QPointF(20, 5), QPointF(20, 40),
                                1, Qt::SolidLine, false);
    checkCoverage(v.strokeMask, 20 - v.bounds.x(), 20 - v.bounds.y(),
                  200, 255, "垂直线中点被覆盖");
    checkCoverage(v.strokeMask, 22 - v.bounds.x(), 20 - v.bounds.y(),
                  0, 0, "垂直线右侧 2 像素处没被覆盖");

    // ── 线宽 5：法线方向上 ±2 也应该被覆盖 ──
    ShapeMask thick = rasterizeLine(canvas, QPointF(10, 20), QPointF(50, 20),
                                    5, Qt::SolidLine, false);
    checkCoverage(thick.strokeMask, 30 - thick.bounds.x(), 21 - thick.bounds.y(),
                  200, 255, "线宽 5 时，中心下方 1 像素仍被覆盖");
    checkCoverage(thick.strokeMask, 30 - thick.bounds.x(), 24 - thick.bounds.y(),
                  0, 0, "线宽 5 时，下方 4 像素（超出半宽 2.5）没被覆盖");

    // ── 完全在画布外 ──
    ShapeMask outside = rasterizeLine(canvas, QPointF(-100, -100), QPointF(-50, -50),
                                      3, Qt::SolidLine, false);
    check(outside.isEmpty(), "整条线在画布外时返回空结果");
    check(outside.strokeMask.isNull(), "空结果的蒙版也是空的");

    // ── 零长度（用户点了一下没拖）：与矩形/椭圆的"零面积不画"同一条规则 ──
    //
    // ★ 这条守的是"误点不留垃圾"★
    //   圆头笔在两点重合时会画出一个实实在在的小圆点 —— 用户点了画布一下，
    //   就得到一个还得撤销的痕迹。阈值 0.5 像素见 rasterizeLine 的注释。
    ShapeMask dot = rasterizeLine(canvas, QPointF(30, 30), QPointF(30, 30),
                                  5, Qt::SolidLine, false);
    check(dot.isEmpty(), "两点重合的直线不产生蒙版（点一下不该留下痕迹）");

    // ── 抗锯齿：斜线边缘必须出现中间值 ──
    //
    // ★ 这是"抗锯齿真的生效了"的唯一直接证据 ★
    //   关掉时边缘只有 0 与 255；打开后必然出现 1~254。
    auto countMid = [](const QImage &m) {
        int n = 0;
        for (int y = 0; y < m.height(); ++y) {
            const uchar *line = m.constScanLine(y);
            for (int x = 0; x < m.width(); ++x)
                if (line[x] > 0 && line[x] < 255)
                    ++n;
        }
        return n;
    };

    ShapeMask aa = rasterizeLine(canvas, QPointF(10, 10), QPointF(50, 40),
                                 1, Qt::SolidLine, true);
    ShapeMask noAa = rasterizeLine(canvas, QPointF(10, 10), QPointF(50, 40),
                                   1, Qt::SolidLine, false);
    const int midAa   = countMid(aa.strokeMask);
    const int midNoAa = countMid(noAa.strokeMask);
    check(midAa > 5, "斜线开抗锯齿后出现中间灰度（边缘过渡带存在）");
    check(midNoAa < midAa, "关掉抗锯齿后中间灰度显著减少");
}

// ===========================================================================
//  [3] 矩形光栅化
// ===========================================================================
static void testRectRaster()
{
    beginGroup("矩形：描边 / 填充 / 圆角");

    const QSize canvas(kW, kH);
    const QRectF r(10, 10, 30, 20);      // x 10..40，y 10..30

    // ── 只描边 ──
    ShapeMask stroke = rasterizeShape(canvas, r, ShapeKind::Rectangle,
                                      ShapeMode::Stroke, 1, 0, false);
    check(!stroke.strokeMask.isNull(), "描边模式有描边蒙版");
    check(stroke.fillMask.isNull(), "描边模式**没有**填充蒙版");
    checkCoverage(stroke.strokeMask, 10 - stroke.bounds.x(), 10 - stroke.bounds.y(),
                  200, 255, "描边：左上角被覆盖");
    checkCoverage(stroke.strokeMask, 25 - stroke.bounds.x(), 20 - stroke.bounds.y(),
                  0, 0, "描边：内部是空的");

    // ── 只填充 ──
    ShapeMask fill = rasterizeShape(canvas, r, ShapeKind::Rectangle,
                                    ShapeMode::Fill, 1, 0, false);
    check(!fill.fillMask.isNull(), "填充模式有填充蒙版");
    check(fill.strokeMask.isNull(), "填充模式**没有**描边蒙版");
    checkCoverage(fill.fillMask, 25 - fill.bounds.x(), 20 - fill.bounds.y(),
                  200, 255, "填充：内部被覆盖");
    checkCoverage(fill.fillMask, 8 - fill.bounds.x(), 20 - fill.bounds.y(),
                  0, 0, "填充：矩形左侧外面没被覆盖");

    // ── 两者都要：两张蒙版都得在 ──
    ShapeMask both = rasterizeShape(canvas, r, ShapeKind::Rectangle,
                                    ShapeMode::Both, 3, 0, false);
    check(!both.strokeMask.isNull() && !both.fillMask.isNull(),
          "Both 模式同时产出描边与填充两张蒙版（分两次合成，才能有两个颜色）");
    check(both.bounds.contains(fill.bounds) && both.bounds.contains(stroke.bounds),
          "Both 的包围盒包含两张蒙版各自的包围盒（否则会截掉一部分）");

    // ── 圆角 ──
    //
    // ★ 断言"角上不再被覆盖"★
    //   圆角半径 8 时，左上角必须是空的 —— 这正是圆角的意义。
    //   如果实现里把半径丢了（例如忘了传进 addRoundedRect），这一条会立刻
    //   失败；而画面上的差别（一个小直角）肉眼几乎看不出来。
    ShapeMask round = rasterizeShape(canvas, r, ShapeKind::Rectangle,
                                     ShapeMode::Fill, 1, 8, false);
    checkCoverage(round.fillMask, 11 - round.bounds.x(), 11 - round.bounds.y(),
                  0, 0, "圆角矩形：左上角（半径内）不被覆盖");
    checkCoverage(round.fillMask, 25 - round.bounds.x(), 20 - round.bounds.y(),
                  200, 255, "圆角矩形：中心照常被覆盖");

    // ── 零面积（用户点了一下没拖）：什么都不该画 ──
    ShapeMask degenerate = rasterizeShape(canvas, QRectF(20, 20, 0, 0),
                                          ShapeKind::Rectangle,
                                          ShapeMode::Both, 3, 0, false);
    check(degenerate.isEmpty(), "零面积矩形不产生任何蒙版");
}

// ===========================================================================
//  [4] 椭圆光栅化（★ 与矩形的分水岭 ★）
// ===========================================================================
static void testEllipseRaster()
{
    beginGroup("椭圆：必须是椭圆，不是矩形");

    const QSize canvas(kW, kH);
    const QRectF r(10, 10, 30, 30);      // 外接正方形 → 正圆

    ShapeMask e = rasterizeShape(canvas, r, ShapeKind::Ellipse,
                                 ShapeMode::Fill, 1, 0, false);
    check(!e.fillMask.isNull(), "椭圆产生了填充蒙版");

    // ★ 四个角是"椭圆 vs 矩形"的唯一分水岭 ★
    //   内切椭圆的四个角必然不被覆盖；同一条外接矩形的四个角必然被覆盖。
    //   两者其余部分（中心、边中点）完全一样，只有这里能分开。
    checkCoverage(e.fillMask, 12 - e.bounds.x(), 12 - e.bounds.y(),
                  0, 0, "椭圆：左上角不被覆盖（与矩形的分界）");
    checkCoverage(e.fillMask, 37 - e.bounds.x(), 12 - e.bounds.y(),
                  0, 0, "椭圆：右上角不被覆盖");
    checkCoverage(e.fillMask, 25 - e.bounds.x(), 25 - e.bounds.y(),
                  200, 255, "椭圆：中心被覆盖");
    checkCoverage(e.fillMask, 25 - e.bounds.x(), 11 - e.bounds.y(),
                  200, 255, "椭圆：正上方 1 像素处被覆盖（顶部附近的宽度约 ±5）");

    // 对照组：同一条外接矩形，换成 Rectangle 类型 → 角必须被覆盖
    ShapeMask rect = rasterizeShape(canvas, r, ShapeKind::Rectangle,
                                    ShapeMode::Fill, 1, 0, false);
    checkCoverage(rect.fillMask, 12 - rect.bounds.x(), 12 - rect.bounds.y(),
                  200, 255, "同一外接矩形的 Rectangle 类型：左上角被覆盖");

    // 圆角半径对椭圆不生效（否则用户会以为选项条坏了）
    ShapeMask eRound = rasterizeShape(canvas, r, ShapeKind::Ellipse,
                                      ShapeMode::Fill, 1, 8, false);
    check(eRound.fillMask.size() == e.fillMask.size(),
          "圆角半径不影响椭圆（Ellipse 忽略 cornerRadius）");
}

// ===========================================================================
//  [5] embedMask：把局部蒙版搬进更大的区域
// ===========================================================================
static void testEmbedMask()
{
    beginGroup("embedMask：位置搬运");

    QImage local(2, 2, QImage::Format_Grayscale8);
    local.fill(0);
    local.scanLine(0)[0] = 255;
    local.scanLine(1)[1] = 200;

    const QRect maskBounds(2, 1, 2, 2);
    const QRect region(0, 0, 6, 6);

    QImage out = embedMask(local, maskBounds, region);
    check(!out.isNull(), "embedMask 产出了区域大小的蒙版");
    check(out.size() == QSize(6, 6), "产出尺寸等于 region 尺寸");
    checkEqInt(int(out.constScanLine(1)[2]), 255, "蒙版 (0,0) 落在区域的 (2,1)");
    checkEqInt(int(out.constScanLine(2)[3]), 200, "蒙版 (1,1) 落在区域的 (3,2)");
    checkEqInt(int(out.constScanLine(0)[0]), 0, "区域的其他位置是 0（没被形状碰到）");
    checkEqInt(int(out.constScanLine(5)[5]), 0, "区域右下角也是 0");

    // region 装不下蒙版 → 返回空（宁可什么都不画，也不能放错位置）
    QImage bad = embedMask(local, maskBounds, QRect(3, 0, 6, 6));
    check(bad.isNull(), "region 不包含蒙版包围盒时返回空（不硬拷）");

    check(embedMask(QImage(), maskBounds, region).isNull(), "空蒙版返回空");
}

// ===========================================================================
//  [6] Shift 约束的纯几何
// ===========================================================================
static void testConstraints()
{
    beginGroup("Shift 约束：8 方向 + 正方形");

    const QPointF start(10, 10);

    // 近水平 → y 锁到起点（x 保留用户拖出的值）
    QPointF h = constrainTo8Directions(start, QPointF(50, 20));
    check(h == QPointF(50, 10), "近水平拖动被锁成水平（y 不变，x 保留）");

    // 近垂直 → x 锁到起点
    QPointF v = constrainTo8Directions(start, QPointF(20, 50));
    check(v == QPointF(10, 50), "近垂直拖动被锁成垂直（x 不变，y 保留）");

    // 对角线 → 两轴分量相等（取 |dx| 与 |dy| 的平均，保留长度感）
    QPointF d = constrainTo8Directions(start, QPointF(40, 36));
    check(d.x() - start.x() == d.y() - start.y(),
          "对角线拖动：两个方向的分量相等（真的是 45°）");
    check(qAbs((d.x() - start.x()) - 28.0) < 0.001,
          "对角线长度取 |dx| 与 |dy| 的平均（30 与 26 → 28）");

    // 反方向（往左、近水平）符号不能丢
    QPointF neg = constrainTo8Directions(start, QPointF(0, 12));
    check(neg == QPointF(0, 10), "往左拖得到向左的水平线（符号没丢）");

    // 两点重合 → 原样返回（不能跳走、不能算出 NaN）
    check(constrainTo8Directions(start, start) == start, "两点重合时原样返回");

    // ── 矩形 ──
    QRectF r1 = rectFromDrag(QPointF(10, 10), QPointF(30, 25), false);
    check(r1 == QRectF(10, 10, 20, 15), "自由拖动：宽高各自独立");

    QRectF r2 = rectFromDrag(QPointF(10, 10), QPointF(30, 25), true);
    check(r2 == QRectF(10, 10, 20, 20), "Shift：约束成正方形（取较长的一边）");

    // ★ 反向拖动必须往反方向展开 ★
    //   统一取正号的话，正方形会突然翻到相反象限 —— 用户会觉得
    //   "按住 Shift 之后图形跑到鼠标另一边去了"。
    QRectF r3 = rectFromDrag(QPointF(30, 30), QPointF(10, 10), true);
    check(r3 == QRectF(10, 10, 20, 20), "Shift：往左上拖也得到左上方的正方形");

    QRectF r4 = rectFromDrag(QPointF(30, 30), QPointF(10, 20), false);
    check(r4 == QRectF(10, 20, 20, 10), "反向自由拖动被 normalized 成正矩形");
}

// ===========================================================================
//  [7] 工具：真的把像素写进了活动图层
// ===========================================================================
static void testDrawIntoLayer()
{
    beginGroup("工具：写进活动图层");

    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
    prepareLine(&line, QColor(255, 0, 0));

    const bool changed = line.drawShape(QPointF(10, 10), QPointF(50, 10));
    check(changed, "画直线报告产生了改动");
    checkRgb(layerPixel(doc, 0, 30, 10), kRed, "线中点被写成描边色");
    checkRgb(layerPixel(doc, 0, 30, 20), kClear, "线以外的像素原样保留");
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "一次绘制压了**一条**撤销记录");

    delete doc;

    // ── 只改活动图层 ──
    //
    // 两层不同颜色，画完之后逐像素确认另一层一模一样。
    Document *doc2 = makeDoc(kClear);
    CommandStack stack2(doc2);
    doc2->addLayer(QStringLiteral("上层"));

    Layer *top = doc2->activeLayer();
    Layer *other = nullptr;
    int activeIdx = doc2->activeLayerIndex();
    int otherIdx = (activeIdx == 0) ? 1 : 0;
    other = doc2->layers().layer(otherIdx);

    if (!top || !other) {
        check(false, "两层文档的前置条件没建立起来");
    } else {
        QImage topImg(kW, kH, QImage::Format_ARGB32_Premultiplied);
        topImg.fill(Qt::transparent);
        rawFillRect(topImg, QRect(0, 0, kW, kH), kBlue);
        top->setImage(topImg);
        doc2->markDirty();

        // 操作前把"另一层"整份拷下来
        const QImage otherBefore = other->image().copy();

        ShapeTool box(ShapeTool::Kind::Rectangle, doc2, &stack2);
        prepareShape(&box, ShapeMode::Fill, QColor(0, 0, 0), QColor(255, 0, 0));
        box.drawShape(QPointF(5, 5), QPointF(25, 25));

        checkRgb(layerPixel(doc2, activeIdx, 15, 15), kRed, "填充画在活动图层上");
        check(layerPixel(doc2, otherIdx, 15, 15) == rawPixel(otherBefore, 15, 15),
              "另一个图层纹丝不动（形状只动活动图层）");
    }

    delete doc2;
}

// ===========================================================================
//  [8] 颜色语义：描边色 / 填充色 / 半透明
// ===========================================================================
static void testColorSemantics()
{
    beginGroup("颜色：描边与填充是两个颜色");

    // ── Both 模式：边是描边色、内部是填充色 ──
    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    ShapeTool box(ShapeTool::Kind::Rectangle, doc, &stack);
    prepareShape(&box, ShapeMode::Both, QColor(255, 0, 0), QColor(0, 0, 255), 3, 0);

    box.drawShape(QPointF(10, 10), QPointF(40, 30));

    checkRgb(layerPixel(doc, 0, 10, 20), kRed, "左边界是描边色");
    checkRgb(layerPixel(doc, 0, 25, 20), kBlue, "内部是填充色");
    checkRgb(layerPixel(doc, 0, 5, 20), kClear, "形状外面没被碰到");
    delete doc;

    // ── 半透明描边：alpha 直接体现在预乘像素上 ──
    //
    // ★ 这条守的是"颜色自带的 alpha 就是透明度"这个语义 ★
    //   本工具的选项里没有单独的不透明度滑块。若哪天有人在合成前
    //   又乘一次 alpha（或反过来把它丢掉），这一条会失败。
    Document *doc2 = makeDoc(kClear);
    CommandStack stack2(doc2);

    ShapeTool line(ShapeTool::Kind::Line, doc2, &stack2);
    prepareLine(&line, QColor(255, 0, 0, 128), 3);
    line.drawShape(QPointF(10, 20), QPointF(50, 20));

    const QRgb px = layerPixel(doc2, 0, 30, 20);
    // 预乘空间里半透明红就是 (128,0,0,128)
    check(qAbs(qAlpha(px) - 128) <= 2,
          "半透明直线：alpha 约等于 128（不被丢掉、也不被叠加两次）");
    check(qAbs(qRed(px) - 128) <= 2,
          "半透明直线：预乘后的红分量也约等于 128");
    check(qGreen(px) == 0 && qBlue(px) == 0,
          "半透明直线：另外两个通道保持 0");
    delete doc2;
}

// ===========================================================================
//  [9] 选区限制
// ===========================================================================
static void testSelectionClip()
{
    beginGroup("选区：形状被裁在选区内");

    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    // 左半边（x < 32）是选区
    doc->selection().prepareCanvas(doc->canvasSize());
    doc->selection().selectRect(QRect(0, 0, 32, kH), Selection::Op::Replace, false);
    check(doc->selection().isActive(), "选区已生效（前置条件，否则本组测不到东西）");

    ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
    prepareLine(&line, QColor(255, 0, 0), 1);

    // 横穿整幅画布的一条线
    line.drawShape(QPointF(5, 20), QPointF(60, 20));

    checkRgb(layerPixel(doc, 0, 20, 20), kRed, "选区内的线段被画上");
    checkRgb(layerPixel(doc, 0, 45, 20), kClear,
             "选区外的线段没有落笔（选区裁剪生效）");

    delete doc;
}

// ===========================================================================
//  [10] 拖动中的旧预览必须被擦掉（★ 本组最重要的一条 ★）
// ===========================================================================
static void testPreviewErase()
{
    beginGroup("拖动：旧预览被擦掉（不留残影）");

    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
    prepareLine(&line, QColor(255, 0, 0), 1);

    // 完整事件流：按下 → 拖到右方 → 拖到下方 → 再拖回右方 → 松手
    //
    // 最终几何是水平线 (10,10)-(50,10)：
    //   · 水平线上的点必须是线色；
    //   · 中线 (10,30) 必须回到**底色** —— 它在中间那一帧被画过。
    //     如果重算区域只取了本帧包围盒，这里会残留一个红点，
    //     而其余所有断言照样通过。
    line.mousePress(mouseEvent(ToolEventType::Press, 10, 10));
    line.mouseMove(mouseEvent(ToolEventType::Move, 50, 10));
    line.mouseMove(mouseEvent(ToolEventType::Move, 10, 50));
    line.mouseMove(mouseEvent(ToolEventType::Move, 50, 10));
    line.mouseRelease(mouseEvent(ToolEventType::Release, 50, 10));

    checkRgb(layerPixel(doc, 0, 30, 10), kRed, "最终几何（水平线）在位");
    checkRgb(layerPixel(doc, 0, 10, 30), kClear,
             "中间那一帧的垂直线已被擦掉（不留残影）");
    checkRgb(layerPixel(doc, 0, 10, 45), kClear, "垂直线更靠下的部分也没残留");
    checkRgb(layerPixel(doc, 0, 30, 45), kClear, "中间帧之外的区域没被波及");

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "整段拖动（含 3 次 move）只压了**一条**撤销记录");

    delete doc;
}

// ===========================================================================
//  [11] 撤销粒度与还原
// ===========================================================================
static void testUndoGranularity()
{
    beginGroup("撤销：两次绘制 = 两条记录，逐条可还原");

    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    ShapeTool box(ShapeTool::Kind::Rectangle, doc, &stack);
    prepareShape(&box, ShapeMode::Fill, QColor(0, 0, 0), QColor(255, 0, 0));

    box.drawShape(QPointF(2, 2), QPointF(12, 12));      // 第一块
    box.drawShape(QPointF(40, 30), QPointF(55, 42));    // 第二块

    checkEqInt(static_cast<int>(stack.undoStack()->count()), 2,
               "两次绘制是两条撤销记录（撞号会被静默合并成一条）");
    checkRgb(layerPixel(doc, 0, 7, 7), kRed, "第一块在位");
    checkRgb(layerPixel(doc, 0, 47, 36), kRed, "第二块在位");

    stack.undo();
    checkRgb(layerPixel(doc, 0, 47, 36), kClear, "撤销一次：第二块消失");
    checkRgb(layerPixel(doc, 0, 7, 7), kRed, "撤销一次：第一块仍在（粒度正确）");

    stack.undo();
    checkRgb(layerPixel(doc, 0, 7, 7), kClear, "再撤销一次：第一块也消失");

    stack.redo();
    checkRgb(layerPixel(doc, 0, 7, 7), kRed, "重做：第一块回来");

    delete doc;
}

// ===========================================================================
//  [12] 取消（Esc）与提交（切换工具）
// ===========================================================================
static void testCancelAndCommit()
{
    beginGroup("取消 / 提交");

    // ── Esc：像素还回去，撤销栈里不留痕 ──
    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
    prepareLine(&line, QColor(255, 0, 0), 1);

    line.mousePress(mouseEvent(ToolEventType::Press, 10, 10));
    line.mouseMove(mouseEvent(ToolEventType::Move, 50, 10));
    checkRgb(layerPixel(doc, 0, 30, 10), kRed,
             "拖动中确实已经写上了像素（前置条件）");
    check(line.isBusy(), "拖动中处于进行中状态");

    line.cancel();
    checkRgb(layerPixel(doc, 0, 30, 10), kClear,
             "取消之后整段触及区域都回到落笔前的样子");
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
               "取消不产生撤销记录");
    check(!line.isBusy(), "取消之后不再是进行中状态");

    // ── 切换工具（commit）：留下并压栈 ──
    line.mousePress(mouseEvent(ToolEventType::Press, 10, 20));
    line.mouseMove(mouseEvent(ToolEventType::Move, 50, 20));
    line.commit();
    checkRgb(layerPixel(doc, 0, 30, 20), kRed, "提交之后像素留下");
    checkEqInt(static_cast<int>(stack.undoStack()->count()), 1,
               "提交把这一笔压进撤销栈");
    check(!line.isBusy(), "提交之后不再是进行中状态");

    delete doc;
}

// ===========================================================================
//  [13] 拒绝路径
// ===========================================================================
static void testRefusals()
{
    beginGroup("拒绝路径（必须是拒绝，不能是画了一半）");

    // ── 点一下不拖动 ──
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
        prepareLine(&line, QColor(255, 0, 0));

        const bool changed = line.drawShape(QPointF(20, 20), QPointF(20, 20));
        check(!changed, "点一下不拖动：不产生改动");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "点一下不拖动：不压命令（压了会让 Ctrl+Z 看起来没反应）");
        checkRgb(layerPixel(doc, 0, 20, 20), kClear, "画布上什么都没留下");
        delete doc;
    }

    // ── 形状整体在画布外 ──
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
        prepareLine(&line, QColor(255, 0, 0));

        const bool changed = line.drawShape(QPointF(-100, -100), QPointF(-50, -50));
        check(!changed, "整个形状在画布外：不产生改动");
        checkEqInt(static_cast<int>(stack.undoStack()->count()), 0,
                   "整个形状在画布外：不压命令");
        delete doc;
    }

    // ── 没有撤销栈 ──
    {
        Document *doc = makeDoc(kClear);
        ShapeTool line(ShapeTool::Kind::Line, doc, nullptr);
        prepareLine(&line, QColor(255, 0, 0));

        check(!line.drawShape(QPointF(10, 10), QPointF(50, 10)),
              "没有撤销栈时拒绝绘制（避免留下撤不掉的痕迹）");
        checkRgb(layerPixel(doc, 0, 30, 10), kClear, "拒绝之后画布干净");
        delete doc;
    }

    // ── 图层锁定 / 隐藏 ──
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
        prepareLine(&line, QColor(255, 0, 0));

        doc->activeLayer()->setLocked(true);
        check(!line.drawShape(QPointF(10, 10), QPointF(50, 10)), "锁定图层拒绝绘制");
        checkRgb(layerPixel(doc, 0, 30, 10), kClear, "锁定图层上什么都没画");

        doc->activeLayer()->setLocked(false);
        doc->activeLayer()->setVisible(false);
        check(!line.drawShape(QPointF(10, 10), QPointF(50, 10)), "隐藏图层拒绝绘制");
        checkRgb(layerPixel(doc, 0, 30, 10), kClear, "隐藏图层上什么都没画");
        delete doc;
    }

    // ── 没有文档（程序启动早期 / 文档全部关闭）──
    {
        ShapeTool line(ShapeTool::Kind::Line, nullptr, nullptr);
        prepareLine(&line, QColor(255, 0, 0));
        check(!line.drawShape(QPointF(10, 10), QPointF(50, 10)),
              "没有文档时安全拒绝（不崩）");
        check(!line.isBusy(), "没有文档时也不该进入进行中状态");
    }

    // ── 右键 / 无按键移动不该被工具吃掉（那是画布自己的手势）──
    {
        Document *doc = makeDoc(kClear);
        CommandStack stack(doc);
        ShapeTool line(ShapeTool::Kind::Line, doc, &stack);
        prepareLine(&line, QColor(255, 0, 0));

        ToolEvent e = mouseEvent(ToolEventType::Press, 10, 10);
        e.button = Qt::RightButton;
        check(!line.mousePress(e).handled, "右键按下不由形状工具处理");
        check(!line.isBusy(), "右键没有开始任何形状");

        ToolEvent e2 = mouseEvent(ToolEventType::Move, 40, 10);
        e2.buttons = Qt::NoButton;
        check(!line.mouseMove(e2).handled, "没有按键的移动不算拖动");
        delete doc;
    }
}

// ===========================================================================
//  [14] 幂等：同一几何反复重算不会越画越深
// ===========================================================================
static void testIdempotent()
{
    beginGroup("幂等：反复重算同一几何不叠加");

    // ★ 这条测的正是"每帧从落笔前快照重算"这条纪律 ★
    //   若在上一帧结果上继续叠加，第二次画到同一位置时颜色会更深
    //   （半透明色尤其明显）—— 而"形状画对了没"的断言照样全绿。
    Document *doc = makeDoc(kClear);
    CommandStack stack(doc);

    ShapeTool box(ShapeTool::Kind::Rectangle, doc, &stack);
    // 半透明填充：叠加式的实现会在这里暴露（重叠处会更深）
    prepareShape(&box, ShapeMode::Fill, QColor(0, 0, 0), QColor(255, 0, 0, 128));

    box.mousePress(mouseEvent(ToolEventType::Press, 10, 10));
    box.mouseMove(mouseEvent(ToolEventType::Move, 40, 30));
    const QRgb once = layerPixel(doc, 0, 25, 20);
    check(qAbs(qAlpha(once) - 128) <= 2,
          "第一次落到该几何：alpha 约等于 128（前置条件）");

    box.mouseMove(mouseEvent(ToolEventType::Move, 10, 10));   // 缩成一个点（什么都不画）
    box.mouseMove(mouseEvent(ToolEventType::Move, 40, 30));   // 再展开到同一几何
    const QRgb twice = layerPixel(doc, 0, 25, 20);
    box.mouseRelease(mouseEvent(ToolEventType::Release, 40, 30));

    checkRgb(twice, once, "回到同一几何时像素与第一次完全相同（幂等，不叠加）");

    delete doc;
}

// ===========================================================================
//  main
// ===========================================================================
int main(int argc, char **argv)
{
    // offscreen：无显示器环境也能跑（工具与选项条都链了 QtWidgets）
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    testIdentity();
    testLineRaster();
    testRectRaster();
    testEllipseRaster();
    testEmbedMask();
    testConstraints();
    testDrawIntoLayer();
    testColorSemantics();
    testSelectionClip();
    testPreviewErase();
    testUndoGranularity();
    testCancelAndCommit();
    testRefusals();
    testIdempotent();

    // ★ 这一行的格式与其它测试保持一致（"通过 N 项，失败 M 项"）★
    //   植入验证脚本按这个模式解析结果 —— 改了它，所有植入验证都会
    //   因为"解析不出来"而集体失效（表现为 (-1,-1)，看着像脚本坏了）。
    std::printf("\n================================================\n");
    std::printf("  shape_tool_test：通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
