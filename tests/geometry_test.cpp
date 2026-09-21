// ============================================================================
//  geometry_test.cpp —— 几何变换与重采样的离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    几何类操作的错误全都是"编译通过、运行不报错、画面悄悄错"的类型，
//    而且错的是**用户最在意的东西**：内容的位置、有没有被裁掉、
//    缩小时颜色有没有偏。
//
//      ① **智能适配的偏移算错**（最隐蔽）
//         画布缩小时程序会主动平移内容。偏移量算错的表现是"图跑到角上
//         去了"或"东西被裁掉了"，而这两种都是用户无法自行纠正的 ——
//         他不知道该往哪个方向改锚点才能拿回内容。
//
//         本测试的做法是**端到端对账**：先用 canvasResizeOffset() 算出
//         期望偏移，再真的执行 Document::resizeCanvas()，然后去文档里
//         找那个标记像素落在哪。对话框的示意图与文档的执行结果共用
//         同一个函数，这条断言就是"预览不会说谎"的守门人。
//
//      ② **重采样忘记归一化权重**
//         输出像素映射到边界附近时，4 个采样点会有部分落在图外，
//         此时权重和不再是 1。忘记归一的表现是"整张图被悄悄提亮/压暗"，
//         且**偏差量随缩放比变化** —— 用户完全无法理解为什么
//         "只是改了个尺寸，图怎么就变灰了"。
//
//         本测试用**纯色图**来钉它：纯色图经过任何核、任何缩放比，
//         输出必须与原色**逐位相同**。权重和只要偏离 1，边角像素
//         立刻不等于原值。这是这个 bug 唯一能被精确定位的形式。
//
//      ③ **90° 旋转不是无损的**
//         如果 90° 旋转走了插值路径，画面会被白白模糊一次。
//         验证方法很硬：连续转四次必须与原图**逐像素相同**。
//
//    不需要 QGuiApplication —— QImage 的缩放、变换、逐像素访问在无 GUI
//    环境下都能工作。编译与运行方式见 run_tests.sh。
// ============================================================================

#include "core/Document.h"
#include "core/Layer.h"
#include "core/LayerList.h"
#include "core/Selection.h"
#include "engine/Geometry.h"
#include "engine/Resample.h"

#include <QColor>
#include <QImage>
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

static void checkEq(int actual, int expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %d，实际 %d\n", what, expected, actual);
    } else {
        ++g_passed;
    }
}

static void checkPoint(const QPoint &actual, const QPoint &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 (%d, %d)，实际 (%d, %d)\n",
                    what, expected.x(), expected.y(), actual.x(), actual.y());
    } else {
        ++g_passed;
    }
}

static void checkSize(const QSize &actual, const QSize &expected, const char *what)
{
    if (actual != expected) {
        ++g_failed;
        std::printf("  [失败] %s  期望 %dx%d，实际 %dx%d\n",
                    what, expected.width(), expected.height(),
                    actual.width(), actual.height());
    } else {
        ++g_passed;
    }
}

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------

// ★ 读"原始存储值"而不是 QImage::pixel() ★
//   pixel() 对预乘格式会做反预乘转换，把内存里的值粉饰成一个"看起来
//   合理"的值。用它的断言测不出预乘空间里的偏差。
static QRgb rawPixel(const QImage &img, int x, int y)
{
    const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
    return line[x];
}

// 生成一张"每个像素都不同"的图案图，用于无损性对账。
// 用 x*7 + y*13 这种互质系数，保证任意重排都会露馅 ——
// 如果用 x+y 之类的对称图案，"转 90°"和"翻转"会得到相同结果。
static QImage makePattern(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int v = (x * 7 + y * 13) & 0xFF;
            line[x] = qRgba(v, 255 - v, (v * 3) & 0xFF, 255);
        }
    }
    return img;
}

// 纯色图。注意：直接写扫描线，不用 setPixel() ——
// setPixel() 在预乘格式上会先做预乘再写，语义不直观，写测试数据时
// 很容易写出与预期不一致的值。
static QImage makeSolid(int w, int h, QRgb premulValue)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(premulValue);
    return img;
}

static Document *makeDoc(int w, int h, const QImage &layerImage)
{
    auto *doc = new Document(QSize(w, h), Qt::transparent);

    // ★ 用 activeLayer() 而不是 layers().layer(0) ★
    //   Document 只提供 const 版本的 layers()（外部不该绕过图层 API
    //   直接改位图），非 const 的入口是 activeLayer()。
    //   构造函数已经建好了初始图层，所以它一定非空。
    Layer *layer = doc->activeLayer();
    if (layer) {
        layer->setImage(layerImage);
        // 内容包围盒是**缓存**的，绕过正常的图层面板路径直接 setImage
        // 之后必须手动让它失效，否则 contentBounds() 会返回旧值 ——
        // 而本测试正是靠它算智能适配偏移的，读到旧值会让断言全错。
        doc->markDirty();
    }
    return doc;
}

// ===========================================================================
//  [1] 九宫格锚点
// ===========================================================================

static void testAnchorOffset()
{
    beginGroup("anchorOffset 九宫格");

    const QSize oldSize(100, 80);
    const QSize grow(140, 120);      // 两边各 +40
    const QSize shrink(60, 40);      // 两边各 -40

    // 放大：偏移为正，内容往右下走
    checkPoint(anchorOffset(Anchor::TopLeft,      oldSize, grow), QPoint(0,   0),   "放大·左上");
    checkPoint(anchorOffset(Anchor::TopCenter,    oldSize, grow), QPoint(20,  0),   "放大·上中");
    checkPoint(anchorOffset(Anchor::TopRight,     oldSize, grow), QPoint(40,  0),   "放大·右上");
    checkPoint(anchorOffset(Anchor::MiddleLeft,   oldSize, grow), QPoint(0,   20),  "放大·左中");
    checkPoint(anchorOffset(Anchor::Center,       oldSize, grow), QPoint(20,  20),  "放大·居中");
    checkPoint(anchorOffset(Anchor::MiddleRight,  oldSize, grow), QPoint(40,  20),  "放大·右中");
    checkPoint(anchorOffset(Anchor::BottomLeft,   oldSize, grow), QPoint(0,   40),  "放大·左下");
    checkPoint(anchorOffset(Anchor::BottomCenter, oldSize, grow), QPoint(20,  40),  "放大·下中");
    checkPoint(anchorOffset(Anchor::BottomRight,  oldSize, grow), QPoint(40,  40),  "放大·右下");

    // 缩小：偏移为负
    checkPoint(anchorOffset(Anchor::Center,      oldSize, shrink), QPoint(-20, -20), "缩小·居中");
    checkPoint(anchorOffset(Anchor::BottomRight, oldSize, shrink), QPoint(-40, -40), "缩小·右下");
    checkPoint(anchorOffset(Anchor::TopLeft,     oldSize, shrink), QPoint(0, 0),     "缩小·左上");

    // ★ 差值为奇数时向零取整，而不是向下取整 ★
    //   C++ 的整数除法向零截断：(-1) / 2 == 0。所以 101 → 100 时
    //   居中锚点给出偏移 0（内容贴左上），而不是 -1。
    //
    //   这是**刻意记录**的行为而不是要修的 bug：两个方向取整方式一致，
    //   内容不会被推到画布外一像素；换成"向下取整"反而会让向左的
    //   居中偏移比向右多一像素，看起来更歪。把它钉住是为了将来
    //   有人"顺手改成 floor"时能立刻发现。
    checkPoint(anchorOffset(Anchor::Center, QSize(101, 81), QSize(100, 80)),
               QPoint(0, 0), "差值为奇数时向零取整（已知且刻意）");
}

// ===========================================================================
//  [2] 智能适配
// ===========================================================================

static void testSmartOffset()
{
    beginGroup("computeSmartOffset 智能适配");

    const QSize oldSize(200, 200);
    const QSize newSize(100, 100);

    // ── ① 内容完整可见 → 完全不动，遵循用户选的锚点 ──
    //   内容在新画布里放得下，智能适配不该多管闲事。
    //
    //   ★ 构造用例时要注意 "放得下" 指的是**加上锚点偏移之后**还放得下 ★
    //     内容矩形比新画布小并不等于锚点不会把它推出去：居中锚点会把
    //     内容整体平移 (-50,-50)，原本在 (60,60) 的内容会变成 (10,10)。
    //     这里取 (60,60,40,40) 正是为了满足"推完仍在画布内"。
    {
        const QRect content(60, 60, 40, 40);       // 平移后右/下到 49
        const QPoint base = anchorOffset(Anchor::Center, oldSize, newSize);  // (-50,-50)
        const QPoint got  = computeSmartOffset(content, oldSize, newSize, Anchor::Center);
        checkPoint(got, base, "内容放得下时保持锚点语义（不擅自平移）");
    }

    // ── ①b 内容比新画布小，但锚点仍会把它推出去 → 智能适配介入 ──
    {
        const QRect content(10, 10, 80, 80);       // 平移 -50 后左边会变成 -40
        const QPoint base = anchorOffset(Anchor::Center, oldSize, newSize);
        const QPoint got  = computeSmartOffset(content, oldSize, newSize, Anchor::Center);

        // 合法范围：minOffset = -left = -10，maxOffset = 100-1-89 = 10
        // base 是 -50 → 被钳到 -10
        checkPoint(got, QPoint(-10, -10),
                   "内容放得下但锚点会推出画布时，钳到合法范围的内侧边界");
        check(got != base, "对照：与纯锚点结果不同，说明确实做了钳制");
    }

    // ── ② 放得下但锚点会把它推出画布 → 平移回画布内 ──
    //   内容在旧画布的右下角，锚点却是左上：直接按锚点算会被裁掉。
    {
        const QRect content(150, 150, 40, 40);     // 左/上 150，右/下 189
        check(!content.isEmpty(), "测试前提：内容非空");

        const QPoint got = computeSmartOffset(content, oldSize, newSize, Anchor::TopLeft);

        // 期望：把内容推到画布右/下边缘内
        //   minOffset = -contentRect.left() = -150
        //   maxOffset = newSize - 1 - contentRect.right() = 100 - 1 - 189 = -90
        //   baseOffset = 0 → qBound(-150, 0, -90) = -90
        checkPoint(got, QPoint(-90, -90), "内容会被推出画布时自动平移回画布内");

        const QRect inNew = content.translated(got);
        check(inNew.left() >= 0 && inNew.right() < newSize.width()
              && inNew.top() >= 0 && inNew.bottom() < newSize.height(),
              "平移后内容确实完整落在新画布内");
    }

    // ── ③ 内容比新画布还大 → 无论怎么移都装不下，退回锚点裁切 ──
    {
        const QRect content(0, 0, 200, 200);       // 与旧画布等大，比新画布大
        const QPoint base = anchorOffset(Anchor::Center, oldSize, newSize);  // (-50,-50)
        const QPoint got  = computeSmartOffset(content, oldSize, newSize, Anchor::Center);
        checkPoint(got, base, "内容装不下时退回锚点裁切（不再假装能救）");
    }

    // ── ④ 空内容 → 直接用锚点结果 ──
    {
        const QPoint base = anchorOffset(Anchor::MiddleRight, oldSize, newSize);
        const QPoint got  = computeSmartOffset(QRect(), oldSize, newSize, Anchor::MiddleRight);
        checkPoint(got, base, "内容包围盒为空时退化为纯锚点");
    }
}

// ===========================================================================
//  [3] 策略层：什么时候才用智能适配
// ===========================================================================

static void testCanvasResizeOffset()
{
    beginGroup("canvasResizeOffset 策略（Document 与对话框共用）");

    const QSize oldSize(200, 200);
    const QSize smaller(100, 100);
    const QSize bigger(300, 300);

    const QRect content(150, 150, 40, 40);

    // 关闭 smart：无论放大缩小都只按锚点
    checkPoint(canvasResizeOffset(content, oldSize, smaller, Anchor::TopLeft, false),
               anchorOffset(Anchor::TopLeft, oldSize, smaller),
               "smart=false 时缩小时也只按锚点");

    // 放大：按锚点（内容必然完整可见，智能适配无事可做）
    for (int i = 0; i < 9; ++i) {
        const Anchor a = static_cast<Anchor>(i);
        checkPoint(canvasResizeOffset(content, oldSize, bigger, a, true),
                   anchorOffset(a, oldSize, bigger),
                   "放大时 smart 不介入（等价于纯锚点）");
    }

    // 缩小 + 内容会被推出画布 → 与纯锚点不同（证明智能适配真的介入了）
    {
        const QPoint anchorOnly = anchorOffset(Anchor::TopLeft, oldSize, smaller);
        const QPoint smart      = canvasResizeOffset(content, oldSize, smaller,
                                                     Anchor::TopLeft, true);
        check(smart != anchorOnly, "缩小时智能适配确实改变了偏移");
        checkPoint(anchorOnly, QPoint(0, 0), "纯锚点结果就是 (0,0)，对照有效");
    }

    // ★ 只在一个方向缩小时也要介入 ★
    //   "任一方向变小"而不是"两个方向都变小" ——
    //   宽变小、高变大（例 4000×3000 → 2000×4000）同样会横裁。
    {
        const QSize wideShrink(100, 400);
        const QRect c(150, 0, 40, 40);
        const QPoint smart = canvasResizeOffset(c, oldSize, wideShrink, Anchor::TopLeft, true);
        check(smart.x() != 0, "只缩窄不缩高时同样触发智能适配（横向被救回）");
    }

    // 空内容包围盒：退化为"整个旧画布"，与改动前的 Document 行为一致
    {
        const QPoint got = canvasResizeOffset(QRect(), oldSize, smaller,
                                             Anchor::TopLeft, true);
        checkPoint(got, QPoint(0, 0),
                   "内容为空时按整个旧画布判断，结果退化为纯锚点");
    }
}

// ===========================================================================
//  [3b] 「打开为图层」的尺寸适配
// ---------------------------------------------------------------------------
//  这两条是"导入一张外来图片"时唯一的决策：
//    ① 画布要不要变大、变到多大   → unionSize
//    ② 图片放在哪里               → centeredOffset
//  它们都会直接决定用户看到的画面（内容被裁 / 位置偏了），
//  所以必须有不依赖控件的断言，而不是靠人眼看截图。
// ===========================================================================

static void testImportSizing()
{
    beginGroup("unionSize / centeredOffset（打开为图层的尺寸适配）");

    // ── unionSize：逐维取大，且是"能装下两者"的最小尺寸 ──

    checkSize(unionSize(QSize(800, 600), QSize(1024, 768)),
              QSize(1024, 768), "导入图更大 → 取导入图");
    checkSize(unionSize(QSize(800, 600), QSize(320, 240)),
              QSize(800, 600), "导入图更小 → 保持原画布");
    checkSize(unionSize(QSize(800, 600), QSize(1200, 400)),
              QSize(1200, 600),
              "★ 一维大、另一维小 → 逐维取大（不是等比缩放，也不是取整体更大的那张）");
    checkSize(unionSize(QSize(800, 600), QSize(400, 900)),
              QSize(800, 900), "★ 另一个方向的一维大 / 一维小同样逐维取大");
    checkSize(unionSize(QSize(100, 100), QSize(100, 100)),
              QSize(100, 100), "尺寸相同 → 结果就是它自己（不扩画布）");

    // ★ 不设"空尺寸"特判 —— 逐维取大本身就是正确行为 ★
    //
    //   这两条同时也是**墓碑断言**：挡住将来有人"顺手"加一句
    //   `if (a.isEmpty()) return b;`。那种改动看起来像防御性编程，
    //   实际是两行永远走不到的死代码 —— 而它会让结果在这两条上变样：
    //     · QSize(0,50) 与 QSize(50,0)：加特判会返回 (50,0)，
    //       逐维取大给出 (50,50)。后者才是"并集"该有的样子。
    //     · QSize() 的默认值是 (-1,-1)（不是 (0,0)），所以它参与取大
    //       本来就不会把结果拉成 0，"特判防 0"这个理由是站不住的。
    checkSize(unionSize(QSize(0, 50), QSize(50, 0)),
              QSize(50, 50),
              "★ 退化尺寸（两个方向各有一个 0）→ 仍是逐维取大，没有特判");
    checkSize(unionSize(QSize(800, 600), QSize()),
              QSize(800, 600),
              "★ QSize() 默认是 (-1,-1)，参与取大不会污染结果");

    // ── centeredOffset：居中放置 ──

    checkPoint(centeredOffset(QSize(800, 600), QSize(400, 200)),
               QPoint(200, 200), "小图居中");
    checkPoint(centeredOffset(QSize(100, 100), QSize(100, 100)),
               QPoint(0, 0), "尺寸相同 → 落在原点");
    checkPoint(centeredOffset(QSize(1024, 768), QSize(1024, 768)),
               QPoint(0, 0),
               "★ 导入图恰好等于画布 → 落在原点（这是「没扩画布」时的常见路径）");

    // 奇数差值：两个方向都向下取整，结果对称地偏向原点，不会"一维偏一维不偏"
    checkPoint(centeredOffset(QSize(11, 11), QSize(4, 4)),
               QPoint(3, 3), "差值是奇数时四个方向对称（都向下取整）");

    // ★ 刻意不钳到非负 ★
    //   画布装不下时返回负数（内容居中、四周溢出），而不是 qMax(0, …)
    //   把它挪到左上角。真实调用点永远先算 unionSize() 再居中，
    //   差值必然非负；这条断言守的是"别顺手加个钳位"——
    //   钳位会把"调用方把尺寸算错了"这个错误**藏起来**，
    //   让内容悄悄贴到左上角，比直接暴露成负数难查得多。
    checkPoint(centeredOffset(QSize(10, 10), QSize(40, 40)),
               QPoint(-15, -15),
               "★ 画布装不下时返回负数（不钳到 0）—— 不掩盖调用方的尺寸算错");

    // ── 两条组合起来：这正是 MainWindow::openAsLayer 用的两行 ──
    {
        const QSize canvas(800, 600);
        const QSize image(1200, 400);
        const QSize grown = unionSize(canvas, image);
        const QPoint pos  = centeredOffset(grown, image);

        checkSize(grown, QSize(1200, 600), "组合：画布扩到并集");
        checkPoint(pos, QPoint(0, 100),
                   "组合：★ 横向刚好占满（差 0）、纵向居中 —— "
                   "一个公式同时覆盖「放得下」与「放不下」两种情况");
        // 导入图必须完整落在画布内 —— 这是"不裁内容"的可断言形式
        check(pos.x() >= 0 && pos.y() >= 0
                  && pos.x() + image.width()  <= grown.width()
                  && pos.y() + image.height() <= grown.height(),
              "★ 导入图完整落在扩大后的画布内（一个像素都不在画布外）");
    }

    // 小图的情形：画布不变，图居中，同样要完整落在画布内
    {
        const QSize canvas(800, 600);
        const QSize image(300, 200);
        const QSize grown = unionSize(canvas, image);
        const QPoint pos  = centeredOffset(grown, image);

        checkSize(grown, canvas, "组合：小图时画布不变");
        checkPoint(pos, QPoint(250, 200), "组合：小图在画布中央");
        check(pos.x() >= 0 && pos.y() >= 0
                  && pos.x() + image.width()  <= grown.width()
                  && pos.y() + image.height() <= grown.height(),
              "小图同样完整落在画布内");
    }
}

// ===========================================================================
//  [4] 直角判定与角度归一化
// ===========================================================================

static void testAngles()
{
    beginGroup("isRightAngle / normalizeAngle");

    int turns = -1;

    check(isRightAngle(90.0, &turns), "90° 判定为直角");
    checkEq(turns, 1, "90° 对应 1 个直角单位");

    check(isRightAngle(180.0, &turns), "180° 判定为直角");
    checkEq(turns, 2, "180° 对应 2 个直角单位");

    check(isRightAngle(270.0, &turns), "270° 判定为直角");
    checkEq(turns, 3, "270° 对应 3 个直角单位");

    // ★ 浮点误差必须被容差吸收 ★
    //   角度经过几次运算后 90 会变成 89.999999999；若用 == 比较就会
    //   走插值路径，把本该无损的 90° 旋转变成一次模糊。
    check(isRightAngle(89.99999999, &turns), "89.99999999° 被当作 90°（吸收浮点误差）");
    checkEq(turns, 1, "浮点误差下仍得到正确的直角单位");

    check(isRightAngle(-90.0, &turns), "-90° 等价于 270°");
    checkEq(turns, 3, "-90° 归一化后是 270°");

    check(isRightAngle(450.0, &turns), "450° 等价于 90°");
    checkEq(turns, 1, "450° 归一化后是 90°");

    // 容差之外必须判为非直角，否则用户真的想转 89.9° 会被当成 90°
    check(!isRightAngle(89.9, &turns), "89.9° 不算直角（容差不能太宽）");
    check(!isRightAngle(90.02, &turns), "90.02° 不算直角");
    check(!isRightAngle(45.0, &turns), "45° 不算直角");

    // normalizeAngle
    check(std::abs(normalizeAngle(0.0)   - 0.0)   < 1e-9, "normalize(0) = 0");
    check(std::abs(normalizeAngle(360.0) - 0.0)   < 1e-9, "normalize(360) = 0");
    check(std::abs(normalizeAngle(450.0) - 90.0)  < 1e-9, "normalize(450) = 90");
    check(std::abs(normalizeAngle(-90.0) - 270.0) < 1e-9, "normalize(-90) = 270");
    check(std::abs(normalizeAngle(-450.0) - 270.0) < 1e-9, "normalize(-450) = 270");
}

// ===========================================================================
//  [5] 旋转后的包围盒
// ===========================================================================

static void testRotatedBounds()
{
    beginGroup("rotatedBoundsSize");

    checkSize(rotatedBoundsSize(QSize(100, 50), 0.0),   QSize(100, 50), "0° 包围盒不变");
    checkSize(rotatedBoundsSize(QSize(100, 50), 180.0), QSize(100, 50), "180° 包围盒不变");
    checkSize(rotatedBoundsSize(QSize(100, 50), 90.0),  QSize(50, 100), "90° 宽高互换");
    checkSize(rotatedBoundsSize(QSize(100, 50), 270.0), QSize(50, 100), "270° 宽高互换");

    // 45°：W' = ceil(100·cos45 + 100·sin45) = ceil(141.42) = 142
    // ★ 向上取整而不是四舍五入 ★
    //   少一个像素就会把边缘只有半个像素的淡色内容切掉，看得出缺口。
    checkSize(rotatedBoundsSize(QSize(100, 100), 45.0), QSize(142, 142),
              "45° 向上取整（宁可多一个透明像素，也不能裁掉内容）");

    // 90+a 与 90-a 对称
    checkSize(rotatedBoundsSize(QSize(80, 40), 60.0),
              rotatedBoundsSize(QSize(80, 40), 120.0),
              "90±a 的包围盒相同（对称性）");

    checkSize(rotatedBoundsSize(QSize(0, 0), 45.0), QSize(), "空尺寸返回空");
}

// ===========================================================================
//  [6] 90° 旋转的无损性
// ===========================================================================

static void testRotateLossless()
{
    beginGroup("90° 旋转必须无损（转四次逐像素回原点）");

    const int w = 37, h = 23;      // 用互质且非方的尺寸，转错方向立刻露馅
    const QImage pattern = makePattern(w, h);

    Document *doc = makeDoc(w, h, pattern);

    checkSize(doc->canvasSize(), QSize(w, h), "初始画布尺寸");

    // 转四次
    for (int i = 0; i < 4; ++i)
        check(doc->rotateImage90(true), "第 N 次顺时针 90° 返回成功");

    checkSize(doc->canvasSize(), QSize(w, h), "转四次后画布回到原尺寸");

    const QImage after = doc->layers().layer(0)->image();
    check(after.size() == QSize(w, h), "转四次后位图尺寸正确");
    check(after == pattern, "★★ 转四次后与原图逐像素相同（否则走了插值路径）");

    // 单次 90° 的宽高互换
    Document *doc2 = makeDoc(w, h, pattern);
    check(doc2->rotateImage90(true), "单次顺时针 90° 成功");
    checkSize(doc2->canvasSize(), QSize(h, w), "单次 90° 后宽高互换");
    check(doc2->layers().layer(0)->image().size() == QSize(h, w), "位图尺寸随画布互换");

    // 顺时针 + 逆时针 = 恒等
    Document *doc3 = makeDoc(w, h, pattern);
    doc3->rotateImage90(true);
    doc3->rotateImage90(false);
    check(doc3->layers().layer(0)->image() == pattern,
          "顺时针 + 逆时针 = 原图（两个方向都无损）");

    delete doc;
    delete doc2;
    delete doc3;
}

// ===========================================================================
//  [7] 翻转的无损性
// ===========================================================================

static void testFlipLossless()
{
    beginGroup("翻转必须无损（翻两次回原点）");

    const QImage pattern = makePattern(29, 41);

    Document *doc = makeDoc(29, 41, pattern);

    check(doc->flipImage(FlipDirection::Horizontal), "水平翻转成功");
    check(doc->layers().layer(0)->image() != pattern, "水平翻转后确实变了（对照有效）");
    check(doc->flipImage(FlipDirection::Horizontal), "再水平翻转成功");
    check(doc->layers().layer(0)->image() == pattern, "★ 水平翻两次逐像素回原点");

    check(doc->flipImage(FlipDirection::Vertical), "垂直翻转成功");
    check(doc->flipImage(FlipDirection::Vertical), "再垂直翻转成功");
    check(doc->layers().layer(0)->image() == pattern, "★ 垂直翻两次逐像素回原点");

    checkSize(doc->canvasSize(), QSize(29, 41), "翻转不改变画布尺寸");

    delete doc;
}

// ===========================================================================
//  [8] 重采样：纯色不漂移
// ===========================================================================

static void testResampleKeepsSolid()
{
    beginGroup("重采样不能让纯色漂移（权重归一化）");

    // ★ 这是本文件最重要的断言 ★
    //
    //   纯色图经过任何核、任何缩放比，输出必须与原色**逐位相同**：
    //   每个输出像素都是若干个相同值的加权平均，只要权重之和为 1，
    //   结果必然还是那个值。权重和一旦偏离 1（忘记归一化），
    //   边角像素立刻不等于原值。
    //
    //   检查边角而不是只检查中心，正是因为 bug 只在取样窗口越界处显形。
    const QRgb opaqueSolid = qRgba(100, 140, 200, 255);
    const QSize src(60, 40);
    const QSize nonInteger(137, 91);    // 两个方向都不是整数倍，必然越界取样

    const QImage srcImg = makeSolid(src.width(), src.height(), opaqueSolid);

    const QImage up = resample::scale(srcImg, nonInteger, ResampleMethod::Bicubic);
    checkSize(up.size(), nonInteger, "缩放到非整数倍后尺寸正确");

    int bad = 0;
    const int xs[] = { 0, 1, nonInteger.width() - 2, nonInteger.width() - 1,
                       nonInteger.width() / 2 };
    const int ys[] = { 0, 1, nonInteger.height() - 2, nonInteger.height() - 1,
                       nonInteger.height() / 2 };
    for (int xi = 0; xi < 5; ++xi)
        for (int yi = 0; yi < 5; ++yi)
            if (rawPixel(up, xs[xi], ys[yi]) != opaqueSolid)
                ++bad;
    checkEq(bad, 0, "★ 放大后四角与中心都与原色逐位相同（权重归一化正确）");

    // 缩小同样要成立
    const QImage down = resample::scale(srcImg, QSize(23, 17), ResampleMethod::Bicubic);
    bad = 0;
    for (int yi = 0; yi < 5; ++yi)
        for (int xi = 0; xi < 5; ++xi)
            if (rawPixel(down, xs[xi] * 23 / 137, ys[yi] * 17 / 91) != opaqueSolid)
                ++bad;
    checkEq(bad, 0, "★ 缩小时纯色同样不漂移");

    // ── 半透明纯色：在**预乘空间**做加权平均才是对的 ──
    //
    //   (50,60,70,128) 是预乘表示，等价于"alpha 50% 的 (100,120,140)"。
    //   如果实现改在非预乘空间插值，透明区域的 RGB 会参与平均，
    //   结果就不是这个值了。
    const QRgb translucent = qRgba(50, 60, 70, 128);
    const QImage transImg = makeSolid(60, 40, translucent);

    const QImage transUp = resample::scale(transImg, nonInteger, ResampleMethod::Bicubic);
    bad = 0;
    for (int yi = 0; yi < 5; ++yi)
        for (int xi = 0; xi < 5; ++xi)
            if (rawPixel(transUp, xs[xi], ys[yi]) != translucent)
                ++bad;
    checkEq(bad, 0, "★ 半透明纯色在预乘空间保持逐位不变");

    // ── 四种算法都不能漂移 ──
    //   邻近取样对纯色是精确的；双线性/Lanczos 与双三次同理。
    const ResampleMethod methods[] = { ResampleMethod::Nearest, ResampleMethod::Bilinear,
                                       ResampleMethod::Bicubic, ResampleMethod::Lanczos };
    for (int i = 0; i < 4; ++i) {
        const QImage r = resample::scale(srcImg, QSize(97, 63), methods[i]);
        const bool ok = !r.isNull()
                     && rawPixel(r, 0, 0) == opaqueSolid
                     && rawPixel(r, 96, 62) == opaqueSolid
                     && rawPixel(r, 48, 31) == opaqueSolid;
        check(ok, "四种重采样算法都保持纯色不漂移");
    }
}

// ===========================================================================
//  [9] Document::scaleImage
// ===========================================================================

static void testDocumentScaleImage()
{
    beginGroup("Document::scaleImage");

    const QImage solid = makeSolid(60, 40, qRgba(100, 140, 200, 255));
    Document *doc = makeDoc(60, 40, solid);

    // 尺寸没变时应当拒绝（返回 false），否则会产生一条什么都没做的撤销记录
    check(!doc->scaleImage(QSize(60, 40), ResampleMethod::Bicubic),
          "尺寸未变时拒绝执行（避免空撤销记录）");

    check(doc->scaleImage(QSize(137, 91), ResampleMethod::Bicubic), "缩放成功");
    checkSize(doc->canvasSize(), QSize(137, 91), "画布尺寸跟着变");
    checkSize(doc->layers().layer(0)->image().size(), QSize(137, 91), "图层位图也跟着变");

    // 缩放后内容仍应是原来的纯色（与 [8] 的引擎断言呼应，这里是走 Document 路径）
    const QImage &scaled = doc->layers().layer(0)->image();
    check(rawPixel(scaled, 0, 0) == qRgba(100, 140, 200, 255),
          "★ 走 Document 路径缩放后纯色依旧不漂移");

    delete doc;
}

// ===========================================================================
//  [10] Document::resizeCanvas —— 与对话框预览公式对账
// ===========================================================================

static void testDocumentResizeCanvas()
{
    beginGroup("Document::resizeCanvas（锚点 / 填充 / 智能适配对账）");

    // ── 放大 + 居中：内容偏移 = 差值的一半 ──
    {
        QImage img(100, 100, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        reinterpret_cast<QRgb *>(img.scanLine(10))[10] = qRgba(255, 0, 0, 255);

        Document *doc = makeDoc(100, 100, img);

        check(doc->resizeCanvas(QSize(150, 150), Anchor::Center, Qt::transparent, true),
              "画布放大成功");
        checkSize(doc->canvasSize(), QSize(150, 150), "放大后画布尺寸正确");

        const QImage &after = doc->layers().layer(0)->image();
        check(rawPixel(after, 35, 35) == qRgba(255, 0, 0, 255),
              "居中放大时标记像素偏移 (+25, +25)（按锚点）");

        // 新区域必须是填充色（透明）
        check(rawPixel(after, 0, 0) == qRgba(0, 0, 0, 0), "扩展区域填充为透明");

        delete doc;
    }

    // ── 放大 + 填充色 ──
    {
        QImage img(20, 20, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        Document *doc = makeDoc(20, 20, img);
        check(doc->resizeCanvas(QSize(30, 30), Anchor::TopLeft, QColor(10, 20, 30), true),
              "带填充色的画布放大成功");

        const QImage &after = doc->layers().layer(0)->image();
        check(rawPixel(after, 25, 25) == qRgba(10, 20, 30, 255),
              "扩展区域用指定的填充色");

        delete doc;
    }

    // ── ★ 缩小 + 智能适配：与对话框预览公式端到端对账 ★ ──
    //
    //   对话框的示意图用 canvasResizeOffset() 算出偏移并据此作图；
    //   Document 用同一个函数执行。这条断言就是"预览不会说谎"的守门人：
    //   两边只要有一处分叉，标记像素落点就会与预期不符。
    {
        QImage img(200, 200, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        // 标记放在右下角 —— 锚点选左上的话，按纯锚点算必然被裁掉
        reinterpret_cast<QRgb *>(img.scanLine(150))[150] = qRgba(255, 0, 0, 255);

        Document *doc = makeDoc(200, 200, img);

        const QRect content = doc->contentBounds();
        check(content == QRect(150, 150, 1, 1), "内容包围盒精确到那一个像素");

        const QSize newSize(100, 100);
        const QPoint expected = canvasResizeOffset(content, QSize(200, 200), newSize,
                                                   Anchor::TopLeft, true);

        // 期望 (-51,-51)：把 (150,150) 推到画布右/下边缘 (99,99)
        checkPoint(expected, QPoint(-51, -51), "引擎算出的智能偏移 (150,150) → (99,99)");
        check(expected != anchorOffset(Anchor::TopLeft, QSize(200, 200), newSize),
              "对照：纯锚点偏移不同，智能适配确实介入了");

        check(doc->resizeCanvas(newSize, Anchor::TopLeft, Qt::transparent, true),
              "缩小画布成功");
        checkSize(doc->canvasSize(), newSize, "缩小后画布尺寸正确");

        const QImage &after = doc->layers().layer(0)->image();
        const QPoint want(150 + expected.x(), 150 + expected.y());
        checkPoint(want, QPoint(99, 99), "期望落点就是画布右下角内侧");
        check(rawPixel(after, want.x(), want.y()) == qRgba(255, 0, 0, 255),
              "★★ 文档实际偏移与预览公式完全一致（内容没被裁掉）");

        delete doc;
    }

    // ── 缩小 + 关闭智能适配：内容按锚点被裁掉 ──
    {
        QImage img(200, 200, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        reinterpret_cast<QRgb *>(img.scanLine(150))[150] = qRgba(255, 0, 0, 255);

        Document *doc = makeDoc(200, 200, img);
        check(doc->resizeCanvas(QSize(100, 100), Anchor::TopLeft, Qt::transparent, false),
              "smart=false 时缩小成功");

        check(doc->contentBounds().isEmpty(),
              "对照：关掉智能适配后标记像素被裁掉（内容包围盒变空）");

        delete doc;
    }

    // ── 尺寸没变时必须拒绝 ──
    {
        QImage img(50, 50, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        Document *doc = makeDoc(50, 50, img);
        check(!doc->resizeCanvas(QSize(50, 50), Anchor::Center, Qt::transparent, true),
              "画布尺寸未变时拒绝执行");
        delete doc;
    }
}

// ===========================================================================
//  [11] Document::crop
// ===========================================================================

static void testDocumentCrop()
{
    beginGroup("Document::crop");

    const QImage pattern = makePattern(80, 60);
    Document *doc = makeDoc(80, 60, pattern);

    const QRect area(30, 20, 25, 15);
    check(doc->crop(area), "裁剪成功");
    checkSize(doc->canvasSize(), QSize(25, 15), "裁剪后画布尺寸 = 裁剪矩形尺寸");

    const QImage &after = doc->layers().layer(0)->image();
    checkSize(after.size(), QSize(25, 15), "裁剪后位图尺寸正确");

    // 裁剪后 (0,0) 应当等于原图 area.topLeft() 处的像素
    check(rawPixel(after, 0, 0) == rawPixel(pattern, area.left(), area.top()),
          "★ 裁剪后左上角对应原图的裁剪起点");
    check(rawPixel(after, 24, 14) == rawPixel(pattern, area.right(), area.bottom()),
          "★ 裁剪后右下角对应原图的裁剪终点");

    delete doc;
}

// ===========================================================================
//  [12] 百分比 ↔ 像素（尺寸对话框的"单位"换算）
// ===========================================================================
//  这两个函数是"输入框单位"的全部算术，平凡到看起来不值得单开一组。
//  但它守的是下面这条契约，而这条契约一旦破了，用户会看到"数字自己会跳"：
//
//    目标尺寸显示成百分比，再由百分比反推回尺寸 —— 结果必须落回**原来的
//    那个像素数**。
//
//  取整方式就是这条契约的全部：写成截断（`int()`）而不是四舍五入，
//  "缩到 50% 再放回 100%"这种往返就会每次少一个像素。用户来回拉几下，
//  图就真的变小了 —— 而他并没有做任何"修改尺寸"的操作。
//
//  纯色图与 90° 旋转那两组测的是"像素有没有错"，这一组测的是"数字有没有错"，
//  而且它不需要 QApplication，跑起来比界面测试便宜得多。

static void testPercentConversion()
{
    beginGroup("百分比 ↔ 像素");

    // ── 恒等：100% 必须是原尺寸本身 ──
    static const int sizes[] = { 1, 2, 3, 7, 199, 200, 1000, 3000, 4000, 20000 };
    const int sizeCount = int(sizeof(sizes) / sizeof(sizes[0]));

    int identityOk = 0;
    for (int i = 0; i < sizeCount; ++i)
        if (percentToPixels(100, sizes[i]) == sizes[i])
            ++identityOk;
    checkEq(identityOk, sizeCount,
            "★ 100% 恒等于原尺寸（地基：差一像素就等于白重采样一次）");

    int backOk = 0;
    for (int i = 0; i < sizeCount; ++i)
        if (pixelsToPercent(sizes[i], sizes[i]) == 100)
            ++backOk;
    checkEq(backOk, sizeCount, "★ 原尺寸反解回百分比恒为 100");

    // ── 常见倍数 ──
    checkEq(percentToPixels(200, 4000), 8000, "200% of 4000 = 8000");
    checkEq(percentToPixels(50,  4000), 2000, "50% of 4000 = 2000");
    checkEq(percentToPixels(33,  4000), 1320, "33% of 4000 = 1320（整除，不受取整影响）");
    checkEq(percentToPixels(25,  3000), 750,  "25% of 3000 = 750");

    // ── 取整：四舍五入，而不是截断 ──
    //   特意挑 .5 与 .9 这两个截断会出错的位置：
    //   3 的 50% 是 1.5（截断得 1），7 的 14% 是 0.98（截断得 0）。
    //   "得 0"尤其致命 —— 尺寸不能是 0，对话框只能钳成 1，
    //   于是"输入 14% 却得到 1 像素"，而百分比显示会跳回 14，看着像卡住了。
    checkEq(percentToPixels(50, 3), 2, "★ 1.5 进为 2（截断会得 1 → 往返漂移）");
    checkEq(percentToPixels(14, 7), 1, "★ 0.98 进为 1（截断会得 0 → 非法的零尺寸）");
    checkEq(percentToPixels(33, 7), 2, "33% of 7 = 2.31 → 2");
    checkEq(percentToPixels(150, 3), 5, "★ 4.5 进为 5");

    // ── 反解 ──
    checkEq(pixelsToPercent(8000,  4000), 200, "8000/4000 → 200%");
    checkEq(pixelsToPercent(1320,  4000), 33,  "★ 1320/4000 → 33%（不会被浮点写成 34）");
    checkEq(pixelsToPercent(20000, 4000), 500, "20000/4000 → 500%");
    checkEq(pixelsToPercent(1, 200), 1,        "1/200 = 0.5% → 1%");

    // ── 上界：对话框用 (kMax*100)/orig 当输入框上限，必须是"够得着且不越界" ──
    //   向下取整的理由就在这里：500% 恰好等于 20000（上限），
    //   而 501% 就超了。若上界向上取整成 501，用户把数字拨到最大反而
    //   会被回写成一个更小的值 —— 一个"拨不到头"的滑块。
    checkEq(percentToPixels(500, 4000), 20000, "500% of 4000 恰好等于上限 20000");
    check(percentToPixels(501, 4000) > 20000,  "★ 501% 就已越界（所以上限必须向下取整）");

    // ── 单调性：百分比涨，像素数不可能跌 ──
    {
        int monotonic = 1;
        for (int pct = 1; pct <= 2000; ++pct) {
            if (percentToPixels(pct, 1234) < percentToPixels(pct - 1, 1234)) {
                monotonic = 0;
                break;
            }
        }
        checkEq(monotonic, 1, "★ 单调：百分比增加时目标像素不会减少");
    }

    // ── ★ 往返稳定 ★ ──
    //   px = f(p) 就是"用户输入 p% 之后真正生效的像素数"。
    //   回写显示的 g(px) 再解析一次必须仍然得到 px —— 否则用户下一次
    //   碰到任何控件（甚至只是切一下单位），数字就会自己跳一格。
    //
    //   注意这个循环里的 px 一定来自 f(p)，而不能随便取一个像素数：
    //   f 是多对一的（1234 宽的图上，30.8% 与 30.9% 都是 380 像素），
    //   任意像素数本来就未必对应某个整数百分比。这里考察的是
    //   "对话框实际走过的那条路径"。
    {
        const int originals[] = { 100, 200, 101, 1234, 3000, 4000 };
        int total = 0, unstable = 0;
        int worstPct = 0, worstOriginal = 0, worstPx = 0, worstBack = 0;

        for (int oi = 0; oi < int(sizeof(originals) / sizeof(originals[0])); ++oi) {
            const int o = originals[oi];
            const int maxPct = (20000 * 100) / o;

            for (int pct = 1; pct <= maxPct; pct += 7) {
                const int px = percentToPixels(pct, o);
                if (px < 1)
                    continue;                 // 对话框会把 0 钳成 1，不考察这一段

                ++total;
                const int shown    = pixelsToPercent(px, o);      // 回写到输入框的数字
                const int roundTrip = percentToPixels(shown, o);  // 用户若原样提交，会得到的像素数

                if (roundTrip != px) {
                    ++unstable;
                    if (unstable == 1) {
                        worstPct = pct; worstOriginal = o;
                        worstPx = px;   worstBack = roundTrip;
                    }
                }
            }
        }

        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "★ 百分比往返稳定：%d/%d 组（首个不稳定：原 %d px 输入 %d%% → %d px → 回写 %d%% → %d px）",
                      total - unstable, total, worstOriginal, worstPct,
                      worstPx, pixelsToPercent(worstPx, worstOriginal), worstBack);
        checkEq(unstable, 0, buf);
    }

    // ── 退化输入：原尺寸 ≤ 0 时不做除零，也不假装有一个尺寸 ──
    checkEq(percentToPixels(100, 0),  0, "原尺寸为 0 → 返回 0（不除零）");
    checkEq(pixelsToPercent(100, 0),  0, "反解同样返回 0");
    checkEq(percentToPixels(100, -5), 0, "负数原尺寸同样返回 0");
    checkEq(percentToPixels(0, 4000), 0, "0% → 0 像素（合法的算术，由调用方钳到 1）");

    // ── 相对模式的定义：+100% 就是两倍（与"绝对 200%"同一个结果）──
    checkEq(percentToPixels(100 + 100, 4000), 8000,
            "★ 「相对 +100%」与「绝对 200%」是同一个目标尺寸");
}

// ===========================================================================
//  [13] 任意角度旋转（画布按包围盒扩大 + 填充色）
// ===========================================================================
//
//  任意角度是唯一"必然改变画布尺寸、且不可逆"的几何操作。三个要点：
//
//    ① 画布尺寸必须等于 rotatedBoundsSize() —— 对话框给用户看的就是
//       这个数，两处一旦分叉，用户看到的就是"说好的尺寸没兑现"。
//    ② **内容一个像素都不能被裁掉**。判据很硬：包围盒的定义就是
//       "刚好容纳旋转后的内容"，所以内容的外接矩形必须铺满整块画布。
//    ③ 四角空出来的地方必须是调用方指定的填充色；90° 倍数则相反 ——
//       它走无损重排，画布与内容等大，填充色无从体现。

static void testRotateArbitrary()
{
    beginGroup("任意角度旋转：包围盒扩大、内容不裁、填充色生效");

    const int w = 40, h = 30;
    const QImage solid = makeSolid(w, h, qRgba(255, 0, 0, 255));   // 不透明纯红
    const QColor fill(0, 0, 255);                                  // 不透明蓝

    // ── 1) 输出尺寸 = 包围盒公式 ──
    const QImage rotated = rotateImage(solid, 30.0, ResampleMethod::Bicubic, fill);
    const QSize expectSize = rotatedBoundsSize(QSize(w, h), 30.0);

    checkSize(rotated.size(), expectSize,
              "★ 旋转后的尺寸与 rotatedBoundsSize() 一致（对话框说的与做的同源）");
    check(rotated.width() > w && rotated.height() > h,
          "30° 旋转后画布比原图大（按包围盒扩大，而不是裁切）");

    // ── 2) 四个角是填充区，且就是调用方要的填充色 ──
    //
    //   非 90° 倍数时四个角必然是空的（内容转成了斜的），所以这里
    //   取到的应当是**纯**填充色，而不是抗锯齿的过渡像素。
    {
        const QRgb tl = rawPixel(rotated, 0, 0);
        const QRgb tr = rawPixel(rotated, rotated.width() - 1, 0);
        const QRgb bl = rawPixel(rotated, 0, rotated.height() - 1);
        const QRgb br = rawPixel(rotated, rotated.width() - 1, rotated.height() - 1);

        check(qBlue(tl) > 200 && qRed(tl) < 40 && qAlpha(tl) == 255,
              "左上角是填充色（蓝，且不透明）");
        check(qBlue(tr) > 200, "右上角是填充色");
        check(qBlue(bl) > 200, "左下角是填充色");
        check(qBlue(br) > 200, "右下角是填充色");
    }

    // ── 3) ★ 内容一个像素都不能被裁掉 ★ ──
    //
    //   容差 2 像素：内容边缘的抗锯齿像素是**半透明**红，预乘存储后
    //   红通道也跟着减半，不会被"红"的判据收进来 —— 所以实测边界
    //   比理论边界最多内缩一两个像素。这是判据的代价，不是裁切。
    {
        int minX = rotated.width(), minY = rotated.height(), maxX = -1, maxY = -1;
        for (int y = 0; y < rotated.height(); ++y) {
            for (int x = 0; x < rotated.width(); ++x) {
                const QRgb c = rawPixel(rotated, x, y);
                if (qRed(c) > 200 && qGreen(c) < 60 && qBlue(c) < 60) {   // 红 = 内容
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }

        check(maxX >= 0, "旋转结果里能找到内容");

        if (maxX >= 0) {
            std::printf("     内容包围盒 x=%d..%d y=%d..%d （画布 %d×%d）\n",
                        minX, maxX, minY, maxY, rotated.width(), rotated.height());
        }

        check(minX <= 1, "内容的左边界贴着画布左边（没被裁）");
        check(minY <= 1, "内容的上边界贴着画布上边（没被裁）");
        check(maxX >= rotated.width()  - 2, "内容的右边界贴着画布右边（没被裁）");
        check(maxY >= rotated.height() - 2, "内容的下边界贴着画布下边（没被裁）");
    }

    // ── 4) 不传填充色时默认透明 ──
    {
        const QImage t = rotateImage(solid, 30.0, ResampleMethod::Bicubic);
        checkEq(qAlpha(rawPixel(t, 0, 0)), 0,
                "不传填充色时四角是透明的（默认参数）");
    }

    // ── 5) 90° 倍数：走无损路径，填充色被忽略 ──
    {
        const QImage img90 = rotateImage(solid, 90.0, ResampleMethod::Bicubic,
                                         QColor(Qt::black));
        const QImage lossless = rotateQuarterTurns(solid, 1);

        checkSize(img90.size(), QSize(h, w), "90° 旋转后宽高互换（不扩大）");
        check(img90 == lossless,
              "★ 90° 走无损路径：与 rotateQuarterTurns 逐像素相同，填充色无从体现");

        // 浮点误差下的 90 也必须被判成 90
        check(rotateImage(solid, 89.9999999, ResampleMethod::Bicubic) == lossless,
              "89.9999999° 仍被判为 90°（浮点容差生效，不会被白糊一次）");

        check(rotateImage(solid, 0.0, ResampleMethod::Bicubic) == solid,
              "0° 返回与原图逐像素相同的副本");
    }

    // ── 6) Document 侧：尺寸、填充色、无损路径 ──
    {
        Document *doc = makeDoc(w, h, solid);
        checkSize(doc->canvasSize(), QSize(w, h), "初始画布尺寸");

        // 0° 视为"无需操作"——避免在历史里留下一条什么都没改的记录
        check(!doc->rotateImageArbitrary(0.0, ResampleMethod::Bicubic),
              "0° 不执行任何操作（返回 false）");

        check(doc->rotateImageArbitrary(30.0, ResampleMethod::Bicubic, fill),
              "Document 任意角度旋转成功");
        checkSize(doc->canvasSize(), expectSize,
                  "★ Document 的画布尺寸与 rotatedBoundsSize() 一致");
        check(doc->layers().layer(0)->image().size() == expectSize,
              "图层位图尺寸跟着画布一起变");

        const QImage &layerImg = doc->layers().layer(0)->image();
        check(qBlue(rawPixel(layerImg, 0, 0)) > 200,
              "Document 侧的填充色同样生效（对话框选的色真的透到了结果里）");

        // 90° 经 Document 也必须是无损的：连转四次回到原图
        Document *doc90 = makeDoc(w, h, makePattern(w, h));
        for (int i = 0; i < 4; ++i)
            doc90->rotateImageArbitrary(90.0, ResampleMethod::Bicubic);
        check(doc90->layers().layer(0)->image() == makePattern(w, h),
              "★ 经 Document 连转四次 90° 逐像素回原点（确实走了无损路径）");

        delete doc;
        delete doc90;
    }
}

// ===========================================================================

int main()
{
    std::printf("=== geometry_test：几何变换与重采样 ===\n\n");

    testAnchorOffset();
    testSmartOffset();
    testCanvasResizeOffset();
testImportSizing();
    testAngles();
    testRotatedBounds();
    testRotateLossless();
    testFlipLossless();
    testResampleKeepsSolid();
    testDocumentScaleImage();
    testDocumentResizeCanvas();
    testDocumentCrop();
    testPercentConversion();
    testRotateArbitrary();

    std::printf("\n----------------------------------------\n");
    std::printf("geometry_test: 通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
