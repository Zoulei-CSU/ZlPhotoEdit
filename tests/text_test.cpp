// ============================================================================
//  text_test.cpp —— engine/Text 的离线自检（文字排版 + 光栅化）
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    文字排版与别的引擎不一样 —— 它**没有"数学上唯一正确"的期望值**。
//    同一个字符串在不同字体下的宽度、高度、墨迹位置全都不同，
//    所以"量正确值"的断言在这里几乎没法写（难道要把 Arial 的字宽抄进来？）。
//
//    于是本测试全部押在**不变量**上（铁律 §4.7）。这些不变量都与字体无关：
//
//      ① 非空 bounds ⟺ 确实有墨        ← 挡住"整段空格也当成功"
//      ② 墨迹绝不贴到蒙版最外一圈       ← 挡住"包围盒算小了、字形被切边"
//      ③ bounds 恒 ⊆ 画布               ← 挡住"画到画布外"
//      ④ 三种对齐下 width/height/top 完全相同，只有水平位移不同
//                                        ← 挡住"对齐顺手把内容改了"
//      ⑤ measureTextBlock 的宽度 == QFontMetricsF 的 advance
//                                        ← 挡住"引擎自己另算一套字宽"
//      ⑥ '\r\n' 与 '\n' 逐像素相同      ← 挡住"看不见的 \r 污染排版"
//      ⑦ 同一输入调用两次逐像素相同     ← 挡住状态泄漏 / 随机性
//      ⑧ 墨迹里既有 255 也有中间值      ← 挡住"抗锯齿静默失效"
//
//    最后两条特别重要：
//      · 若 QPainter 在 Grayscale8 上其实什么都没画出来，①②会一起变红；
//      · 若某个"优化"把文字渲染成二值（无中间灰度），⑧会红 ——
//        而这一点在界面上的表现只是"边缘有点硬"，肉眼几乎看不出来。
//
//  ★ 本测试**刻意只链接 Qt5Core + Qt5Gui** ★
//    engine 层不允许依赖 QtWidgets。TextOptions（用户参数对象）在
//    tools/ToolOptions.cpp 里、拖着整个 QtWidgets，所以它**不在这里测** ——
//    "参数对象 → 字体 → 排版"那条链归 text_tool_test。
//    如果哪天有人往 engine/Text.* 里塞进一个 QWidget 依赖，
//    这里会立刻编译失败，而不是等到某天有人抱怨"engine 层变慢了"。
//
//  ★ 为什么本测试要建 QGuiApplication ★
//    别的引擎测试都不建（QImage 逐像素访问不需要 GUI）。但**排版需要字体**，
//    而字体的加载要 QGuiApplication（更准确地说：要 QFontDatabase）。
//    用 QGuiApplication 而不是 QApplication —— 后者会拖进 QtWidgets，
//    那样"engine 不依赖 Widgets"这条守门就白设了。
//
//    同时顺手设 QT_QPA_FONTDIR：Qt 5.15 起不再自带字体，offscreen 平台
//    默认找不到任何字体目录，一个字都画不出来（症状是"全是空蒙版"，
//    而不是报错）。把字体目录显式指到系统字体目录上，
//    本测试在任何机器上都自给自足（不需要外部先设环境变量）。
// ============================================================================

#include "engine/Text.h"

#include <QGuiApplication>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QtGlobal>

#include <cmath>
#include <cstdio>

using namespace zl;

// ---------------------------------------------------------------------------
//  断言辅助
// ---------------------------------------------------------------------------
static int  g_failed = 0;
static int  g_passed = 0;
static bool g_fontOk = true;

static void check(bool ok, const char *what, const char *detail = "")
{
    if (ok) {
        ++g_passed;
    } else {
        ++g_failed;
        std::printf("  [失败] %s  %s\n", what, detail);
    }
}

static void checkEq(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkNear(qreal actual, qreal expected, qreal tol, const char *what)
{
    if (std::fabs(actual - expected) > tol) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %.2f（容差 %.2f），实际 %.2f\n",
                    what, expected, tol, actual);
    } else {
        ++g_passed;
    }
}

// 矩形**逐值相等**（不给容差）。
//   用在"同一输入两次/两种等价输入"这类必须逐位一致的场景 ——
//   那时任何差异都是 bug，给容差反而会把 bug 放过去。
static void checkRectEq(const QRect &got, const QRect &want, const char *what)
{
    if (got != want) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 (x=%d,y=%d,w=%d,h=%d)，"
                    "实际 (x=%d,y=%d,w=%d,h=%d)\n",
                    what, want.left(), want.top(), want.width(), want.height(),
                    got.left(), got.top(), got.width(), got.height());
    } else {
        ++g_passed;
    }
}

// 矩形近似相等（每条边分别给容差）—— 排版里的取整误差是 1~2 像素量级，
// 写死相等会变成"换个字体就红"，而这条断言想抓的是"数量级不对"。
static void checkRectNear(const QRect &got, const QRect &want, int tol, const char *what)
{
    const bool ok = std::abs(got.left()   - want.left())   <= tol
                 && std::abs(got.top()    - want.top())    <= tol
                 && std::abs(got.width()  - want.width())  <= tol
                 && std::abs(got.height() - want.height()) <= tol;
    if (!ok) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 (x=%d,y=%d,w=%d,h=%d)，"
                    "实际 (x=%d,y=%d,w=%d,h=%d)  容差 %d\n",
                    what, want.left(), want.top(), want.width(), want.height(),
                    got.left(), got.top(), got.width(), got.height(), tol);
    } else {
        ++g_passed;
    }
}

static void checkRectNearF(const QRectF &got, const QRectF &want, qreal tol,
                           const char *what)
{
    const bool ok = std::fabs(got.left()   - want.left())   <= tol
                 && std::fabs(got.top()    - want.top())    <= tol
                 && std::fabs(got.width()  - want.width())  <= tol
                 && std::fabs(got.height() - want.height()) <= tol;
    if (!ok) {
        ++g_failed;
        std::printf("  [失败] %s\n         期望 (%.1f,%.1f,%.1fx%.1f)，"
                    "实际 (%.1f,%.1f,%.1fx%.1f)  容差 %.1f\n",
                    what, want.left(), want.top(), want.width(), want.height(),
                    got.left(), got.top(), got.width(), got.height(), tol);
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------

// 固定用 Arial：Windows 上一定有，而且是无衬线、字形宽度可预测。
// 字号取 48 而不是 12：小字号下抗锯齿的中间灰度像素可能一个都没有，
// 会让"抗锯齿生效"那条断言变成环境敏感（那属于测试自身不稳，不是产品问题）。
static QFont testFont(int pixelSize = 48)
{
    QFont f(QStringLiteral("Arial"));
    f.setPixelSize(pixelSize);
    return f;
}

// 蒙版里"有墨"的像素的包围盒（**蒙版局部坐标**）。
// 返回空矩形表示一个墨点都没有。
static QRect inkBounds(const QImage &mask)
{
    if (mask.isNull() || mask.format() != QImage::Format_Grayscale8)
        return QRect();

    int minX = mask.width(), minY = mask.height(), maxX = -1, maxY = -1;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (line[x] != 0) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    if (maxX < 0)
        return QRect();
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

static int inkMaxValue(const QImage &mask)
{
    int m = 0;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            m = qMax(m, int(line[x]));
    }
    return m;
}

// 中间灰度（抗锯齿过渡）像素个数。排除 250~255（字干）与 1~4（噪声）。
static int inkIntermediateCount(const QImage &mask)
{
    int n = 0;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            const int v = int(line[x]);
            if (v >= 5 && v <= 249)
                ++n;
        }
    }
    return n;
}

static bool sameImage(const QImage &a, const QImage &b)
{
    if (a.isNull() || b.isNull())
        return a.isNull() && b.isNull();
    if (a.size() != b.size() || a.format() != b.format())
        return false;
    for (int y = 0; y < a.height(); ++y) {
        if (std::memcmp(a.constScanLine(y), b.constScanLine(y),
                        size_t(a.width())) != 0)
            return false;
    }
    return true;
}

// 某一行的墨迹水平范围（起点 -1 表示该行没有墨）
static void rowInkRange(const QImage &mask, int y, int &a, int &b)
{
    a = -1;
    b = -1;
    if (y < 0 || y >= mask.height())
        return;
    const uchar *line = mask.constScanLine(y);
    for (int x = 0; x < mask.width(); ++x) {
        if (line[x] != 0) {
            if (a < 0)
                a = x;
            b = x;
        }
    }
}

// 墨迹在**图像坐标**里的包围盒（局部坐标 + bounds 偏移）
static QRect inkBoundsInImage(const TextMask &tm)
{
    const QRect local = inkBounds(tm.mask);
    if (local.isEmpty())
        return QRect();
    return local.translated(tm.bounds.topLeft());
}

// ---------------------------------------------------------------------------
//  0. 前置条件：字体必须可用
// ---------------------------------------------------------------------------
//  ★ 为什么不"没有字体就跳过断言" ★
//    那会让整套测试在没有字体的机器上**显示全绿而什么都没测**。
//    自检宁可红，也不要给假的好消息（见 run_tests.sh 里防假绿那段）。
//    所以这里把"字体可用"本身当成一条断言，并且让它显式地红。
static void preconditions()
{
    std::printf("[0] 前置条件：字体与 Grayscale8 绘制\n");

    const QFont f = testFont();
    const QFontMetricsF fm(f);
    check(fm.horizontalAdvance(QStringLiteral("X")) > 0.0,
          "Arial 可用（字宽 > 0）",
          "★ 拿不到字体 → 下面全部断言都会失去意义");

    const QSize canvas(400, 200);
    const TextMask tm = rasterizeText(QStringLiteral("X"), f, TextAlign::Left,
                                      1.2, QPointF(10, 10), canvas);

    // 这一条同时证明三件事：字体有字形、Grayscale8 可以作画、覆盖度取值正确。
    // 任何一条不成立都会在这里集中暴露，而不是让后面几十条断言各自"失败得莫名其妙"。
    const bool painted = !tm.isEmpty();
    check(painted, "QPainter 能在 Grayscale8 上画出字形",
          "★ 若这里红：engine/Text.cpp 的绘制目标格式需要换（见文件头）");

    if (!painted)
        g_fontOk = false;
}

// ---------------------------------------------------------------------------
//  1. 空文本 / 空白文本 / 退化输入
// ---------------------------------------------------------------------------
static void testEmptyAndBlank()
{
    const QSize canvas(400, 200);
    const QFont f = testFont();

    check(rasterizeText(QString(), f, TextAlign::Left, 1.2,
                        QPointF(10, 10), canvas).isEmpty(),
          "空字符串 → 空结果");

    check(rasterizeText(QStringLiteral("\n\n"), f, TextAlign::Left, 1.2,
                        QPointF(10, 10), canvas).isEmpty(),
          "只有换行 → 空结果（有行、无墨）");

    // ★ 这一条是本组最重要的 ★
    //   空格有 advance（所以 measureTextBlock 会给出非零宽度），
    //   但一个字形都没有。若引擎只按"包围盒非空"判定成功，
    //   调用方就会压一条"按了 Ctrl+Z 什么也没发生"的空撤销命令。
    check(rasterizeText(QStringLiteral("   "), f, TextAlign::Left, 1.2,
                        QPointF(10, 10), canvas).isEmpty(),
          "只有空格 → 空结果（有宽度、无墨）★");

    check(rasterizeText(QStringLiteral("\t\t"), f, TextAlign::Left, 1.2,
                        QPointF(10, 10), canvas).isEmpty(),
          "只有制表符 → 空结果");

    // 排版尺寸与"有没有墨"是两件事：
    //   空格有宽度 → measure 非空；但它没有墨 → rasterize 为空。
    //   这两条同时断言，是为了把这个差异**写死**，避免将来有人
    //   "顺手统一一下"，把它改成其中一边。
    check(!measureTextBlock(QStringLiteral(" "), f, TextAlign::Left, 1.2,
                            QPointF(10, 10)).isEmpty(),
          "排版尺寸：空格仍占宽度（与上面那条并不矛盾）");

    check(measureTextBlock(QString(), f, TextAlign::Left, 1.2,
                           QPointF(10, 10)).isEmpty(),
          "排版尺寸：空字符串 → 空矩形");

    check(rasterizeText(QStringLiteral("X"), f, TextAlign::Left, 1.2,
                        QPointF(10, 10), QSize()).isEmpty(),
          "画布尺寸为空 → 空结果（防除零 / 防越界）");

    check(measureTextBlock(QStringLiteral("X"), f, TextAlign::Left, 1.2,
                           QPointF(10, 10)).width() > 0.0,
          "排版尺寸与画布无关（没有画布也能算出块宽）");
}

// ---------------------------------------------------------------------------
//  2. 单行排版几何
// ---------------------------------------------------------------------------
static void testMeasureSingleLine()
{
    const QFont f = testFont();
    const QFontMetricsF fm(f);
    const QString text = QStringLiteral("Hg");

    const QRectF r = measureTextBlock(text, f, TextAlign::Left, 1.0,
                                      QPointF(100, 50));

    // ★ 同源不变量：引擎的块宽必须**就是** QFontMetricsF 给的 advance ★
    //   这一条挡住的是"引擎自己另算一套字宽"（例如按字形数 × 平均宽估）。
    //   那种实现看起来"差不多对"，但在等宽/衬线/等距字体上会明显偏，
    //   而且换了字体就再也对不上 —— 是最难查的一类。
    checkNear(r.width(), fm.horizontalAdvance(text), 0.01,
              "单行宽度 == QFontMetricsF::horizontalAdvance（同源）★");

    checkNear(r.height(), fm.height(), 0.01,
              "单行高度 == QFontMetricsF::height()");

    checkNear(r.left(), 100.0, 0.01, "左对齐：块左边缘 == 锚点 x");
    checkNear(r.top(), 50.0, 0.01, "锚点 y 就是块顶（三对齐共用）");

    // 行距 1.0 时高度不该等于 2 行（"把行数算多一行"是常见笔误）
    checkNear(r.height(), fm.height(), 0.01,
              "单行高度不是 2 行（行数没多算）");
}

// ---------------------------------------------------------------------------
//  3. 多行排版与行距
// ---------------------------------------------------------------------------
static void testMeasureMultiLineAndLineHeight()
{
    const QFont f = testFont();
    const QFontMetricsF fm(f);
    const QString text = QStringLiteral("one\ntwo\nthree");
    const QPointF anchor(10, 10);

    const QRectF r1 = measureTextBlock(text, f, TextAlign::Left, 1.0, anchor);
    // ★ 高度公式：(n-1) × 行距 + 最后一行自身的高度 ★
    //   写成 n × 行距的话，块底会凭空多出一整行 —— 于是 DrawCommand 的
    //   脏矩形跟着变大（只是慢一点），而"文字框"的视觉范围也偏大。
    checkNear(r1.height(), 2.0 * fm.height() + fm.height(), 0.01,
              "3 行 @行距1.0：高 == 2×行高 + 1×行高");

    const QRectF r2 = measureTextBlock(text, f, TextAlign::Left, 2.0, anchor);
    checkNear(r2.height(), 2.0 * (2.0 * fm.height()) + fm.height(), 0.01,
              "3 行 @行距2.0：步长变成 2 倍行高");

    checkNear(r2.height() - r1.height(), 2.0 * fm.height(), 0.01,
              "行距增大 1 倍 → 多出 (n-1)×行高");

    // 行距不该影响宽度
    checkNear(r2.width(), r1.width(), 0.01, "行距不影响块宽");

    // ── 行距的退化钳制 ──
    //   0 或负值会让所有行重叠；不钳的话块高会算成 0 或负数，
    //   QRectF 变成"空矩形"，一个字形都画不出来 —— 用户以为工具坏了。
    const QRectF rZero = measureTextBlock(text, f, TextAlign::Left, 0.0, anchor);
    const QRectF rMin  = measureTextBlock(text, f, TextAlign::Left, 0.1, anchor);
    checkNear(rZero.height(), rMin.height(), 0.01,
              "行距 0 被钳到下限 0.1（与直接给 0.1 同结果）");

    const QRectF rNeg = measureTextBlock(text, f, TextAlign::Left, -5.0, anchor);
    checkNear(rNeg.height(), rMin.height(), 0.01, "负行距同样被钳到下限");

    check(rMin.height() > 0.0, "退化行距下块高仍为正（不会退化成空矩形）");

    // 三行文本必须真的比两行高
    const QRectF two = measureTextBlock(QStringLiteral("one\ntwo"), f,
                                        TextAlign::Left, 1.0, anchor);
    check(r1.height() > two.height(), "三行块比两行块高");
}

// ---------------------------------------------------------------------------
//  4. 对齐（排版几何）
// ---------------------------------------------------------------------------
static void testAlignGeometry()
{
    const QFont f = testFont();
    const QString text = QStringLiteral("aligned");
    const QPointF anchor(200, 80);

    const QRectF left   = measureTextBlock(text, f, TextAlign::Left,   1.2, anchor);
    const QRectF center = measureTextBlock(text, f, TextAlign::Center, 1.2, anchor);
    const QRectF right  = measureTextBlock(text, f, TextAlign::Right,  1.2, anchor);

    checkNear(left.left(), 200.0, 0.01, "左对齐：左边缘 == 锚点 x");
    checkNear(center.left() + center.width() / 2.0, 200.0, 0.01,
              "居中：块中心 == 锚点 x");
    checkNear(right.left() + right.width(), 200.0, 0.01,
              "右对齐：右边缘 == 锚点 x");

    // ★ 不变量：对齐只改水平位置，不改尺寸、不改纵向位置 ★
    //   若有人把"对齐"实现成"整体平移后重新排版"，这些会一起红。
    checkNear(left.width(), center.width(), 0.01, "对齐不改块宽（居中）");
    checkNear(left.width(), right.width(), 0.01, "对齐不改块宽（右对齐）");
    checkNear(left.height(), center.height(), 0.01, "对齐不改块高（居中）");
    checkNear(left.height(), right.height(), 0.01, "对齐不改块高（右对齐）");
    checkNear(left.top(), center.top(), 0.01, "对齐不改块顶（居中）");
    checkNear(left.top(), right.top(), 0.01, "对齐不改块顶（右对齐）");

    checkNear(center.left(), 200.0 - left.width() / 2.0, 0.01,
              "居中位移恰好是半个块宽");
    checkNear(right.left(), 200.0 - left.width(), 0.01,
              "右对齐位移恰好是一个块宽");
}

// ---------------------------------------------------------------------------
//  5. 蒙版包围盒与墨迹（核心不变量）
// ---------------------------------------------------------------------------
static void testMaskBoundsAndInk()
{
    const QSize canvas(600, 300);
    const QFont f = testFont();
    const QString text = QStringLiteral("Hg");
    const QPointF anchor(120, 90);

    const TextMask tm = rasterizeText(text, f, TextAlign::Left, 1.2, anchor, canvas);

    check(!tm.isEmpty(), "普通文本产生非空结果");
    checkEq(int(tm.mask.format()), int(QImage::Format_Grayscale8),
            "蒙版格式是 Grayscale8（composeStroke 要的就是它）");
    checkEq(tm.mask.width(),  tm.bounds.width(),  "蒙版宽 == bounds 宽");
    checkEq(tm.mask.height(), tm.bounds.height(), "蒙版高 == bounds 高");

    // bounds 必须盖住排版算出来的块（整数化后可能差 1~2 像素）
    const QRectF block = measureTextBlock(text, f, TextAlign::Left, 1.2, anchor);
    check(tm.bounds.contains(block.toAlignedRect()),
          "bounds 包含排版块（整数化后）",
          "包围盒算小了 → 字形会被切边");

    // ★★ 最强的一条：墨迹不能贴到蒙版最外一圈 ★★
    //
    //  它一行断言同时守住三件事：
    //    ① pad（余量）足够 —— 抗锯齿、"探出字身框"的斜体、下划线都没被切；
    //    ② 排版块的取整方向没写反；
    //    ③ 蒙版坐标系与 bounds 偏移一致（错位会让墨贴边甚至跑到画布外）。
    //  而且它**与字体无关** —— 换任何字体都应该成立。
    const QRect ink = inkBounds(tm.mask);
    check(!ink.isEmpty(), "蒙版里确实有墨");

    const int marginL = ink.left();
    const int marginT = ink.top();
    const int marginR = tm.mask.width()  - 1 - ink.right();
    const int marginB = tm.mask.height() - 1 - ink.bottom();
    check(marginL >= 1 && marginT >= 1 && marginR >= 1 && marginB >= 1,
          "墨迹四周都不贴蒙版边缘（余量足够，字形没被切）★");
    if (!(marginL >= 1 && marginT >= 1 && marginR >= 1 && marginB >= 1)) {
        std::printf("         实测四周余量 左%d 上%d 右%d 下%d\n",
                    marginL, marginT, marginR, marginB);
    }

    // 墨迹必须在块内（允许 pad 带来的外扩，但不该大幅超出）
    const QRect inkInImage = inkBoundsInImage(tm);
    check(inkInImage.width()  <= int(std::ceil(block.width()))  + 2,
          "墨迹宽度不超过块的宽度（+2 容差）");
    check(inkInImage.height() <= int(std::ceil(block.height())) + 2,
          "墨迹高度不超过块的高度（+2 容差）");

    // ── 覆盖度取值 ──
    const int maxV = inkMaxValue(tm.mask);
    check(maxV >= 250, "字干处覆盖度接近满值（>=250）",
          "若偏小 → 白色没画上去 / 被背景色污染");
    check(inkIntermediateCount(tm.mask) > 20,
          "存在大量中间灰度（抗锯齿真的生效）★",
          "二值化的文字边缘会有硬台阶，而这件事在界面上几乎看不出来");

    checkEq(int(tm.mask.constScanLine(0)[0]), 0,
            "蒙版角上是 0（底色黑：0 = 不覆盖）");
}

// ---------------------------------------------------------------------------
//  6. 画布裁剪
// ---------------------------------------------------------------------------
static void testCanvasClip()
{
    const QSize canvas(200, 100);
    const QFont f = testFont(24);
    const QString text = QStringLiteral("Hello");

    // 完全在画布外（左上）→ 什么都不该剩下
    check(rasterizeText(text, f, TextAlign::Left, 1.2,
                        QPointF(-400, -400), canvas).isEmpty(),
          "文字完全在画布外（左上）→ 空结果");

    // 完全在画布外（右下）
    check(rasterizeText(text, f, TextAlign::Left, 1.2,
                        QPointF(900, 900), canvas).isEmpty(),
          "文字完全在画布外（右下）→ 空结果");

    // 部分越界 → bounds 被裁到画布内，且仍有墨
    const TextMask tm = rasterizeText(text, f, TextAlign::Left, 1.2,
                                      QPointF(180, 40), canvas);
    check(!tm.isEmpty(), "文字骑在画布右边界上：仍有内容");
    check(canvas.width() > tm.bounds.right(),
          "被裁后的 bounds 右边缘不超出画布");

    // ★ 不变量：bounds 恒 ⊆ 画布（扫一批锚点）★
    //   这条挡住的是"只在某一个方向上求交"这类半成品裁剪。
    int outside = 0;
    const QRect canvasRect(QPoint(0, 0), canvas);
    for (int ax = -60; ax <= 260; ax += 20) {
        for (int ay = -60; ay <= 160; ay += 20) {
            const TextMask t = rasterizeText(text, f, TextAlign::Center, 1.2,
                                             QPointF(ax, ay), canvas);
            if (t.isEmpty())
                continue;
            if (!canvasRect.contains(t.bounds))
                ++outside;
        }
    }
    checkEq(outside, 0, "扫描一批锚点：bounds 恒在画布之内 ★");

    // 右对齐时锚点在画布右边缘 → 文字往左展开，不应该整块被裁掉
    const TextMask rightAligned = rasterizeText(text, f, TextAlign::Right, 1.2,
                                                QPointF(199, 40), canvas);
    check(!rightAligned.isEmpty(),
          "右对齐锚点在右边界：文字向左展开（不是丢掉）");
    if (!rightAligned.isEmpty())
        check(rightAligned.bounds.right() <= 199,
              "右对齐锚点在右边界：bounds 不越过锚点");
}

// ---------------------------------------------------------------------------
//  7. 换行符归一化（看不见的字符）
// ---------------------------------------------------------------------------
static void testNewlineNormalization()
{
    const QSize canvas(400, 200);
    const QFont f = testFont(32);

    const TextMask lf   = rasterizeText(QStringLiteral("ab\ncd"), f,
                                        TextAlign::Left, 1.2, QPointF(20, 20), canvas);
    const TextMask crlf = rasterizeText(QStringLiteral("ab\r\ncd"), f,
                                        TextAlign::Left, 1.2, QPointF(20, 20), canvas);
    const TextMask cr   = rasterizeText(QStringLiteral("ab\rcd"), f,
                                        TextAlign::Left, 1.2, QPointF(20, 20), canvas);

    check(!lf.isEmpty(), "两行文本（\\n）有内容");
    checkRectEq(crlf.bounds, lf.bounds, "\\r\\n 与 \\n 的包围盒相同 ★");
    checkRectEq(cr.bounds, lf.bounds, "\\r 与 \\n 的包围盒相同 ★");
    check(sameImage(lf.mask, crlf.mask), "\\r\\n 与 \\n 的蒙版逐字节相同 ★");
    check(sameImage(lf.mask, cr.mask), "\\r 与 \\n 的蒙版逐字节相同 ★");

    // 排版尺寸同理
    checkRectNearF(measureTextBlock(QStringLiteral("ab\r\ncd"), f, TextAlign::Left,
                                    1.2, QPointF(20, 20)),
                   measureTextBlock(QStringLiteral("ab\ncd"), f, TextAlign::Left,
                                    1.2, QPointF(20, 20)),
                   0.01, "排版尺寸对 \\r\\n 与 \\n 一致 ★");
}

// ---------------------------------------------------------------------------
//  8. 字距：必须由字体携带，引擎不得自己再施加一次
// ---------------------------------------------------------------------------
static void testLetterSpacingComesFromFont()
{
    const QSize canvas(600, 200);
    const QString text = QStringLiteral("spacing");

    QFont plain = testFont(36);
    QFont wide  = testFont(36);
    // TextOptions::toFont 就是这么干的：把字距写进字体。
    wide.setLetterSpacing(QFont::AbsoluteSpacing, 8.0);

    const QRectF rp = measureTextBlock(text, plain, TextAlign::Left, 1.2,
                                       QPointF(10, 10));
    const QRectF rw = measureTextBlock(text, wide, TextAlign::Left, 1.2,
                                       QPointF(10, 10));

    check(rw.width() > rp.width() + 10.0,
          "字体里的字距真的被采纳了（宽度明显变大）");

    // ★ 同源：引擎的宽度必须等于"带着字距的那个字体"的 advance ★
    //   若引擎自己再按 letterSpacing 加一次，这里会算出一个更大的值。
    //   这就是"同一件事有两处真相"的守门断言 —— 它挡的是**静默变宽**
    //   （文字只是比预期宽一点，没有任何报错）。
    const QFontMetricsF fmWide(wide);
    checkNear(rw.width(), fmWide.horizontalAdvance(text), 0.01,
              "带字距时，块宽仍严格等于该字体的 advance（引擎没有加两遍）★");

    const TextMask tmPlain = rasterizeText(text, plain, TextAlign::Left, 1.2,
                                           QPointF(10, 10), canvas);
    const TextMask tmWide  = rasterizeText(text, wide, TextAlign::Left, 1.2,
                                           QPointF(10, 10), canvas);
    check(!tmPlain.isEmpty() && !tmWide.isEmpty(), "两种字距都有墨");
    if (!tmPlain.isEmpty() && !tmWide.isEmpty())
        check(tmWide.bounds.width() > tmPlain.bounds.width(),
              "字距更大的蒙版更宽");
}

// ---------------------------------------------------------------------------
//  9. 三种对齐在**光栅结果**上的差异
// ---------------------------------------------------------------------------
static void testAlignInRaster()
{
    const QSize canvas(800, 300);
    const QFont f = testFont(36);
    const QString text = QStringLiteral("align");
    const QPointF anchor(400, 100);

    const TextMask l = rasterizeText(text, f, TextAlign::Left,   1.2, anchor, canvas);
    const TextMask c = rasterizeText(text, f, TextAlign::Center, 1.2, anchor, canvas);
    const TextMask r = rasterizeText(text, f, TextAlign::Right,  1.2, anchor, canvas);

    check(!l.isEmpty() && !c.isEmpty() && !r.isEmpty(), "三种对齐都产生了内容");
    if (l.isEmpty() || c.isEmpty() || r.isEmpty())
        return;

    const QRect il = inkBoundsInImage(l);
    const QRect ic = inkBoundsInImage(c);
    const QRect ir = inkBoundsInImage(r);

    const QRectF block = measureTextBlock(text, f, TextAlign::Left, 1.2, anchor);

    // ★ 位移关系：居中的墨迹恰好左移半个块宽，右对齐左移一个块宽 ★
    //   与"块宽"这条合成判据绑在一起，比"居中的墨迹应该在 380 附近"稳得多
    //   （后者把字体度量抄进了测试）。
    checkNear(qreal(ic.left() - il.left()), -block.width() / 2.0, 3.0,
              "光栅：居中相对左对齐左移半个块宽 ★");
    checkNear(qreal(ir.left() - il.left()), -block.width(), 3.0,
              "光栅：右对齐相对左对齐左移一个块宽 ★");

    // ★ 不变量：对齐不改内容 —— 墨迹的宽高、纵向位置必须完全一致 ★
    checkNear(qreal(ic.width()), qreal(il.width()), 2.0,
              "光栅：对齐不改墨迹宽度（居中）");
    checkNear(qreal(ir.width()), qreal(il.width()), 2.0,
              "光栅：对齐不改墨迹宽度（右对齐）");
    checkNear(qreal(ic.height()), qreal(il.height()), 2.0,
              "光栅：对齐不改墨迹高度（居中）");
    checkNear(qreal(ir.height()), qreal(il.height()), 2.0,
              "光栅：对齐不改墨迹高度（右对齐）");
    checkEq(ic.top(), il.top(), "光栅：对齐不改墨迹纵向位置（居中）");
    checkEq(ir.top(), il.top(), "光栅：对齐不改墨迹纵向位置（右对齐）");

    // 墨迹的中心/边缘应当落在锚点上
    checkNear(qreal(ic.left() + ic.width() / 2), anchor.x(), 4.0,
              "光栅：居中时墨迹中心 ≈ 锚点 x");
    checkNear(qreal(ir.left() + ir.width()), anchor.x(), 4.0,
              "光栅：右对齐时墨迹右边缘 ≈ 锚点 x");
    checkNear(qreal(il.left()), anchor.x(), 6.0,
              "光栅：左对齐时墨迹左边缘 ≈ 锚点 x（含字形左边距，容差 6）");
}

// ---------------------------------------------------------------------------
// 10. 多行光栅：真的分了行
// ---------------------------------------------------------------------------
static void testRasterMultiLine()
{
    const QSize canvas(400, 300);
    const QFont f = testFont(30);
    const QPointF anchor(20, 20);

    const TextMask one = rasterizeText(QStringLiteral("AA"), f, TextAlign::Left,
                                       1.0, anchor, canvas);
    const TextMask two = rasterizeText(QStringLiteral("AA\nAA"), f, TextAlign::Left,
                                       1.0, anchor, canvas);
    const TextMask twoLoose = rasterizeText(QStringLiteral("AA\nAA"), f,
                                            TextAlign::Left, 2.0, anchor, canvas);

    check(!one.isEmpty() && !two.isEmpty(), "单行与两行都有内容");

    // 两行必须比一行高（至少多出一行的大部分高度）
    const QFontMetricsF fm(f);
    check(two.bounds.height() > one.bounds.height() + int(fm.height() * 0.7),
          "两行的包围盒比一行高出约一行 ★");

    // ★ 两行之间必须真的有一整行空隙 ★
    //   若 '\n' 被当成空格或忽略，文本会挤在一行上，高度不会变大，
    //   而中间这行"无墨的行"也不存在 —— 这一条直接把它钉死。
    const QRect ink = inkBounds(two.mask);
    int blankRows = 0;
    for (int y = ink.top(); y <= ink.bottom(); ++y) {
        int a = -1, b = -1;
        rowInkRange(two.mask, y, a, b);
        if (a < 0)
            ++blankRows;
    }
    check(blankRows >= 3, "两行之间存在明显的空白行 ★");

    int blankRowsLoose = 0;
    const QRect inkL = inkBounds(twoLoose.mask);
    for (int y = inkL.top(); y <= inkL.bottom(); ++y) {
        int a = -1, b = -1;
        rowInkRange(twoLoose.mask, y, a, b);
        if (a < 0)
            ++blankRowsLoose;
    }
    check(blankRowsLoose > blankRows, "行距 2.0 时中间空白更多（行距真的生效）");

    // 第一行顶部不随行距变化（锚点是"首行顶部"，不是"块中心"）
    checkEq(inkBounds(twoLoose.mask).top(), inkBounds(two.mask).top(),
            "行距变大时首行顶部不动（锚点语义）");

    // 行的水平起点应当一致（左对齐下两行都从锚点 x 开始）
    int a1 = -1, b1 = -1, a2 = -1, b2 = -1;
    rowInkRange(two.mask, inkBounds(two.mask).top() + 2, a1, b1);
    rowInkRange(twoLoose.mask, inkBounds(twoLoose.mask).top() + 2, a2, b2);
    checkEq(a2, a1, "行距变化不影响首行的水平起点");
}

// ---------------------------------------------------------------------------
// 11. 幂等与无状态
// ---------------------------------------------------------------------------
static void testIdempotent()
{
    const QSize canvas(500, 200);
    const QFont f = testFont(40);
    const QString text = QStringLiteral("stable\n重复");
    const QPointF anchor(30, 30);

    const TextMask a = rasterizeText(text, f, TextAlign::Center, 1.4, anchor, canvas);
    const TextMask b = rasterizeText(text, f, TextAlign::Center, 1.4, anchor, canvas);

    checkRectEq(a.bounds, b.bounds, "同一输入两次：包围盒相同");
    check(sameImage(a.mask, b.mask), "同一输入两次：蒙版逐字节相同 ★");

    // 中间插一次别的调用，结果不能被影响（无残留状态）
    rasterizeText(QStringLiteral("别的文字"), f, TextAlign::Right, 3.0,
                  QPointF(7, 7), QSize(50, 50));
    const TextMask c = rasterizeText(text, f, TextAlign::Center, 1.4, anchor, canvas);
    check(sameImage(a.mask, c.mask), "夹一次别的调用之后仍然相同（无状态泄漏）★");

    // 排版函数是纯函数
    const QRectF r1 = measureTextBlock(text, f, TextAlign::Center, 1.4, anchor);
    const QRectF r2 = measureTextBlock(text, f, TextAlign::Center, 1.4, anchor);
    checkNear(r1.width(), r2.width(), 0.0, "排版函数是纯函数（宽）");
    checkNear(r1.height(), r2.height(), 0.0, "排版函数是纯函数（高）");

    // 中文字符也要能排（本工程的用户群，必须覆盖）
    const TextMask zh = rasterizeText(QStringLiteral("中文排版"),
                                      testFont(40), TextAlign::Left, 1.2,
                                      QPointF(10, 10), canvas);
    check(!zh.isEmpty(), "中文字符能排出内容 ★");
}

// ---------------------------------------------------------------------------
// 12. 健壮性
// ---------------------------------------------------------------------------
static void testRobustness()
{
    const QFont huge  = testFont(2000);
    const QFont tiny  = testFont(1);
    const QFont dflt;                                   // 完全不设 family
    const QString text = QStringLiteral("Wg");

    // 超大字号 + 小画布：必须被裁剪成画布大小，而不是分配一张巨大的图
    const QSize small(80, 40);
    const TextMask h = rasterizeText(text, huge, TextAlign::Left, 1.2,
                                     QPointF(0, 0), small);
    if (!h.isEmpty()) {
        check(small.width() >= h.bounds.width() && small.height() >= h.bounds.height(),
              "超大字号：结果恒不超过画布 ★");
        check(h.bounds.left() >= 0 && h.bounds.top() >= 0,
              "超大字号：结果原点不为负");
    } else {
        check(true, "超大字号：返回空也算合法（该字完全在画布外）");
    }

    // 极小字号：不崩；有没有墨都可以，但**不能返回一张尺寸对不上的蒙版**
    const TextMask t = rasterizeText(text, tiny, TextAlign::Left, 1.2,
                                     QPointF(0, 0), small);
    check(t.isEmpty() || t.mask.size() == t.bounds.size(),
          "极小字号：蒙版尺寸与 bounds 一致（或空）");

    // 默认字体（无 family）：不能崩
    const TextMask d = rasterizeText(text, dflt, TextAlign::Center, 1.2,
                                     QPointF(40, 20), small);
    check(d.isEmpty() || d.mask.size() == d.bounds.size(),
          "默认构造的字体：不崩且尺寸自洽");

    // 极端行距
    const TextMask big = rasterizeText(text, testFont(20), TextAlign::Left, 100.0,
                                       QPointF(0, 0), QSize(200, 130));
    check(big.isEmpty() || big.bounds.height() <= 130,
          "行距 100 倍：结果被裁到画布内");

    // 1×1 画布
    const TextMask one = rasterizeText(QStringLiteral("M"), testFont(64),
                                       TextAlign::Left, 1.2, QPointF(0, 0),
                                       QSize(1, 1));
    check(one.isEmpty() || one.mask.size() == one.bounds.size(),
          "1×1 画布：不崩且自洽");

    // 超长文本（自动换行不支持 → 只能靠画布裁剪兜住）
    const TextMask longText = rasterizeText(QString(4000, QLatin1Char('m')),
                                            testFont(20), TextAlign::Left, 1.2,
                                            QPointF(0, 0), QSize(300, 100));
    check(longText.isEmpty() || longText.bounds.width() <= 300,
          "超长单行：被画布宽度裁住（不折行是刻意的，见 Text.h）");
}

// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    // ★ 必须在这两件事之前设：Qt 读字体目录发生在 QGuiApplication 构造时 ★
    //   Qt 5.15 起不再自带字体，offscreen 平台默认一个字体都找不到 ——
    //   症状是"所有蒙版都是空的"，而不是报错。
    //   先设好，本测试在任何机器上都不需要外部再配环境变量。
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) {
        qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    }
    qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);

    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 文字排版引擎 离线自检\n");
    std::printf("================================================\n");

    preconditions();

    if (!g_fontOk) {
        std::printf("\n================================================\n");
        std::printf("  ★ 前置条件不成立：拿不到字体 / 画不出字形。\n");
        std::printf("    后面的断言全部没有意义，直接判失败。\n");
        std::printf("================================================\n");
        return 1;
    }

    testEmptyAndBlank();
    testMeasureSingleLine();
    testMeasureMultiLineAndLineHeight();
    testAlignGeometry();
    testMaskBoundsAndInk();
    testCanvasClip();
    testNewlineNormalization();
    testLetterSpacingComesFromFont();
    testAlignInRaster();
    testRasterMultiLine();
    testIdempotent();
    testRobustness();

    std::printf("\n================================================\n");
    std::printf("  text_test：通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
