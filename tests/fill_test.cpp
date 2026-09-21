// ============================================================================
//  fill_test.cpp —— engine/Fill 的离线自检（扫描线种子填充）
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    填充的 bug 几乎全是"不报错、只是填得比想象的多（或少）"的类型：
//
//      ① **把透明与不透明黑当成同色**
//         三通道色差对 (0,0,0,255) 与 (0,0,0,0) 给出 0。用户在一张实心
//         黑图上点油漆桶，填充会溢进整张透明画布 —— 不报错、不崩溃，
//         只是"填的范围不对"。本测试用 fillDistance 把它钉死。
//
//      ② **区域范围随遍历顺序变化**
//         若把判据写成"与**邻居**比较"，同一张图从左上点与从右下点
//         会得到不同形状的区域，而且会沿着渐变一路蔓延到天边。
//         正确做法是始终以**种子色**为参照。测试用一张渐变图覆盖它。
//
//      ③ **连通性被静默忽略**
//         region（颜色合格）与 visited（已被线段认领）是两件事。
//         少了 visited，连续模式就退化成非连续模式 ——
//         症状是"隔着一堵黑墙的另一块白区也被填了"。
//         测试用"一堵竖墙"与"一个甜甜圈"两张图分别覆盖。
//
//      ④ **扫描线栈的重复压栈**
//         没有 visited 时相邻线段会互相把对方重新压栈，栈无限增长。
//         表现为程序卡死 / 内存爆掉，与"算法写错"看起来毫无关系。
//
//    这些函数都不需要 QGuiApplication —— QImage 的逐扫描线访问在无 GUI
//    环境下就能工作。编译方式见 run_tests.sh。
// ============================================================================

#include "engine/Fill.h"
#include "engine/Brush.h"

#include <QImage>
#include <QRect>
#include <QtGlobal>

#include <cstdio>

using namespace zl;

static int g_failed = 0;
static int g_passed = 0;

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

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------

// ★ 一律用扫描线读写，不用 QImage::pixel() / setPixel() ★
//   setPixel() 在预乘格式上会"先预乘再写入"，pixel() 又会"反预乘读出"，
//   两次隐式转换叠在一起，测试数据就不是你写的那个值了。
static QRgb rawPixel(const QImage &img, int x, int y)
{
    return reinterpret_cast<const QRgb *>(img.constScanLine(y))[x];
}

static void rawSet(QImage &img, int x, int y, QRgb c)
{
    reinterpret_cast<QRgb *>(img.scanLine(y))[x] = c;
}

static void fillAll(QImage &img, QRgb c)
{
    for (int y = 0; y < img.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
            line[x] = c;
    }
}

static void fillRect(QImage &img, int x0, int y0, int x1, int y1, QRgb c)
{
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            rawSet(img, x, y, c);
}

static int maskAt(const QImage &m, int x, int y)
{
    return int(m.constScanLine(y)[x]);
}

// 蒙版中非零像素的个数
static int maskCount(const QImage &m)
{
    int n = 0;
    for (int y = 0; y < m.height(); ++y) {
        const uchar *line = m.constScanLine(y);
        for (int x = 0; x < m.width(); ++x)
            if (line[x] != 0)
                ++n;
    }
    return n;
}

static QImage makeImage(int w, int h, QRgb bg)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    fillAll(img, bg);
    return img;
}

static const QRgb kWhite = qRgba(255, 255, 255, 255);
static const QRgb kBlack = qRgba(0, 0, 0, 255);
static const QRgb kClear = qRgba(0, 0, 0, 0);
static const QRgb kRed   = qRgba(255, 0, 0, 255);

// ---------------------------------------------------------------------------
//  1. colorDistance —— 容差的度量
// ---------------------------------------------------------------------------
static void testColorDistance()
{
    std::printf("\n[1] colorDistance（三通道欧氏色差）\n");

    checkEq(colorDistance(kBlack, kWhite), 255, "黑 ↔ 白 = 255（理论上限）");
    checkEq(colorDistance(kBlack, kBlack), 0, "同色 = 0");

    // 灰阶上色差与灰阶差 1:1 —— 这是最直观的一档，也是容差滑块的标尺
    for (int v : { 1, 30, 64, 128, 200 }) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "灰阶差 %d 的色差 = %d", v, v);
        checkEq(colorDistance(kBlack, qRgba(v, v, v, 255)), v, buf);
    }

    // ★ 单通道差 d 对应色差 d/√3，不是 d ★
    //   这是"除以 3 再开方"的直接后果，也是最容易被误解的一点：
    //   容差 32 并不等于"每通道 32"，而是每通道约 55。
    checkEq(colorDistance(kBlack, qRgba(30, 0, 0, 255)), 17, "单通道差 30 → 色差 17");
    checkEq(colorDistance(kBlack, qRgba(0, 0, 255, 255)), 147, "单通道差 255 → 色差 147");
    checkEq(colorDistance(kBlack, qRgba(255, 255, 0, 255)), 208, "两通道差 255 → 色差 208");

    // 对称性：填充的判据必须与人眼一样不分先后
    checkEq(colorDistance(qRgba(10, 200, 30, 255), qRgba(240, 5, 90, 255)),
            colorDistance(qRgba(240, 5, 90, 255), qRgba(10, 200, 30, 255)),
            "色差对称");

    // ★ 这里就是 ①号坑：三通道色差看不见 alpha ★
    checkEq(colorDistance(kBlack, kClear), 0,
            "不透明黑与全透明 —— 三通道色差为 0（正是它不能单独当判据的原因）");
}

// ---------------------------------------------------------------------------
//  2. fillDistance —— 填充判据
// ---------------------------------------------------------------------------
static void testFillDistance()
{
    std::printf("\n[2] fillDistance（再带上 alpha）\n");

    // ★ 本组最关键的一条 ★
    checkEq(fillDistance(kBlack, kClear), 255,
            "不透明黑 ↔ 全透明 = 255（填充不会溢进透明区）");

    checkEq(fillDistance(kBlack, kBlack), 0, "同色同 alpha = 0");
    checkEq(fillDistance(qRgba(0, 0, 0, 200), qRgba(0, 0, 0, 150)), 50, "alpha 差 50 = 50");

    // RGB 差更大时由 RGB 说了算
    checkEq(fillDistance(kRed, kBlack), 147, "RGB 差占主导时取 RGB");
    // alpha 差更大时由 alpha 说了算
    checkEq(fillDistance(qRgba(255, 0, 0, 255), qRgba(255, 0, 0, 0)), 255,
            "alpha 差占主导时取 alpha");

    // fillDistance 恒 ≥ colorDistance（取 max 的直接后果）
    const QRgb samples[] = {
        kBlack, kWhite, kRed, kClear, qRgba(3, 200, 77, 128), qRgba(200, 3, 77, 250)
    };
    bool alwaysAtLeast = true;
    for (QRgb a : samples)
        for (QRgb b : samples)
            if (fillDistance(a, b) < colorDistance(a, b))
                alwaysAtLeast = false;
    check(alwaysAtLeast, "fillDistance >= colorDistance 恒成立");

    // 照片场景（alpha 恒 255）：两者必须逐位相等，不吃精度亏
    bool identicalOnOpaque = true;
    for (QRgb a : samples)
        for (QRgb b : samples)
            if (qAlpha(a) == 255 && qAlpha(b) == 255
                && fillDistance(a, b) != colorDistance(a, b))
                identicalOnOpaque = false;
    check(identicalOnOpaque, "alpha 全不透明时两者逐位相等");
}

// ---------------------------------------------------------------------------
//  3. 容差：单调性与精确刻度
// ---------------------------------------------------------------------------
static void testTolerance()
{
    std::printf("\n[3] 容差\n");

    // 256×1 的灰阶渐变，种子在最左（黑）。
    // 灰阶上色差 == 灰阶差，所以"容差 T"应当恰好填出 T+1 个像素。
    QImage grad(256, 1, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < 256; ++x)
        rawSet(grad, x, 0, qRgba(x, x, x, 255));

    const auto fillWith = [&](int tol) {
        FloodFillParams p;
        p.seed = QPoint(0, 0);
        p.tolerance = tol;
        p.contiguous = true;
        p.antialias = false;        // 关掉过渡带，刻度才干净
        return floodFill(grad, p);
    };

    checkEq(maskCount(fillWith(0).mask), 1, "容差 0 → 只填起点自己");
    checkEq(maskCount(fillWith(50).mask), 51, "容差 50 → 填 51 个像素");
    checkEq(maskCount(fillWith(200).mask), 201, "容差 200 → 填 201 个像素");
    checkEq(maskCount(fillWith(255).mask), 256, "容差 255 → 全填");

    // 单调性：容差变大，填充范围只能变大
    int prev = -1;
    bool monotone = true;
    for (int tol = 0; tol <= 255; tol += 5) {
        const int n = maskCount(fillWith(tol).mask);
        if (n < prev)
            monotone = false;
        prev = n;
    }
    check(monotone, "容差单调：范围内像素数不随容差下降");

    // 包围盒紧致性 —— 容差 50 恰好覆盖 x=0..50
    const FillResult r = fillWith(50);
    check(r.bounds == QRect(0, 0, 51, 1), "包围盒紧致（0,0,51,1）");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "包围盒实测 (%d,%d,%d,%d)",
                  r.bounds.x(), r.bounds.y(), r.bounds.width(), r.bounds.height());
    check(r.bounds == QRect(0, 0, 51, 1), buf);
}

// ---------------------------------------------------------------------------
//  4. 抗锯齿过渡带
// ---------------------------------------------------------------------------
static void testSoftBand()
{
    std::printf("\n[4] 抗锯齿过渡带\n");

    QImage grad(256, 1, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < 256; ++x)
        rawSet(grad, x, 0, qRgba(x, x, x, 255));

    FloodFillParams p;
    p.seed = QPoint(0, 0);
    p.tolerance = 50;
    p.contiguous = true;
    p.antialias = true;
    p.softBand = 32;
    const FillResult r = floodFill(grad, p);

    checkEq(maskAt(r.mask, 0, 0), 255, "过渡带：区域内是满覆盖");
    checkEq(maskAt(r.mask, 50, 0), 255, "过渡带：容差边界上仍是满覆盖");
    checkEq(maskAt(r.mask, 82, 0), 0, "过渡带：容差+带宽之外为 0");
    checkEq(maskAt(r.mask, 83, 0), 0, "过渡带：再远一个像素还是 0");

    // 带内必须是 1..254（严格介于两端之间），且单调递减
    bool inBand = true;
    bool descending = true;
    int prev = 256;
    for (int x = 51; x <= 81; ++x) {
        const int v = maskAt(r.mask, x, 0);
        if (v <= 0 || v >= 255)
            inBand = false;
        if (v > prev)
            descending = false;
        prev = v;
    }
    check(inBand, "过渡带内的覆盖度严格介于 0 与 255 之间");
    check(descending, "过渡带内覆盖度单调递减");

    // 关掉抗锯齿后，边界必须是硬的
    p.antialias = false;
    const FillResult hard = floodFill(grad, p);
    checkEq(maskAt(hard.mask, 50, 0), 255, "关闭抗锯齿：边界内满覆盖");
    checkEq(maskAt(hard.mask, 51, 0), 0, "关闭抗锯齿：边界外立刻为 0");

    // 带宽 0 等价于关闭
    p.antialias = true;
    p.softBand = 0;
    const FillResult zeroBand = floodFill(grad, p);
    checkEq(maskAt(zeroBand.mask, 51, 0), 0, "带宽 0 等价于关闭抗锯齿");
}

// ---------------------------------------------------------------------------
//  5. 连通性：连续 vs 非连续
// ---------------------------------------------------------------------------
static void testContiguity()
{
    std::printf("\n[5] 连通性\n");

    // ── 5.1 一堵竖墙把画布切成两块互不相连的白区 ──
    QImage walled = makeImage(40, 40, kWhite);
    fillRect(walled, 20, 0, 20, 39, kBlack);

    FloodFillParams p;
    p.seed = QPoint(5, 5);
    p.tolerance = 0;
    p.antialias = false;

    p.contiguous = true;
    const FillResult left = floodFill(walled, p);
    checkEq(maskAt(left.mask, 5, 5), 255, "连续：起点被填");
    checkEq(maskAt(left.mask, 19, 5), 255, "连续：墙左侧仍被填");
    checkEq(maskAt(left.mask, 21, 5), 0, "连续：墙右侧不被填（连通性生效）");
    checkEq(maskAt(left.mask, 39, 39), 0, "连续：右半部分整体不被填");
    checkEq(maskCount(left.mask), 20 * 40, "连续：恰好填满左半（20×40）");
    check(left.bounds == QRect(0, 0, 20, 40), "连续：包围盒就是左半");

    p.contiguous = false;
    const FillResult all = floodFill(walled, p);
    checkEq(maskAt(all.mask, 21, 5), 255, "非连续：墙右侧也被填");
    checkEq(maskAt(all.mask, 39, 39), 255, "非连续：角落也被填");
    checkEq(maskAt(all.mask, 20, 5), 0, "非连续：黑墙本身仍不被填");
    checkEq(maskCount(all.mask), 40 * 40 - 40, "非连续：填满除墙之外的全部");

    // ── 5.2 甜甜圈：黑环内部的封闭白区 ──
    //     这是最能暴露"没有 visited 就退化成非连续"的形状 ——
    //     外圈白区与内圈白区颜色完全相同、只被 1 像素的黑环隔开。
    QImage donut = makeImage(40, 40, kWhite);
    fillRect(donut, 10, 10, 30, 10, kBlack);   // 上边
    fillRect(donut, 10, 30, 30, 30, kBlack);   // 下边
    fillRect(donut, 10, 10, 10, 30, kBlack);   // 左边
    fillRect(donut, 30, 10, 30, 30, kBlack);   // 右边

    p.contiguous = true;
    p.seed = QPoint(20, 20);                   // 圆环内部
    const FillResult inner = floodFill(donut, p);
    checkEq(maskAt(inner.mask, 20, 20), 255, "甜甜圈：内部被填");
    checkEq(maskAt(inner.mask, 0, 0), 0, "甜甜圈：环外不被填");
    checkEq(maskAt(inner.mask, 39, 39), 0, "甜甜圈：环外另一角不被填");
    checkEq(maskAt(inner.mask, 10, 20), 0, "甜甜圈：黑环本身不被填");
    check(inner.bounds == QRect(11, 11, 19, 19), "甜甜圈：包围盒 = 环内 19×19");

    p.seed = QPoint(2, 2);                     // 圆环外部
    const FillResult outer = floodFill(donut, p);
    checkEq(maskAt(outer.mask, 0, 0), 255, "甜甜圈：从外部点，外面被填");
    checkEq(maskAt(outer.mask, 20, 20), 0, "甜甜圈：环内**不**被填穿");
}

// ---------------------------------------------------------------------------
//  6. 边界与健壮性
// ---------------------------------------------------------------------------
static void testRobustness()
{
    std::printf("\n[6] 边界与健壮性\n");

    QImage img = makeImage(16, 16, kWhite);

    // ── 6.1 起点越界 ──
    for (QPoint bad : { QPoint(-1, 0), QPoint(0, -1),
                        QPoint(16, 0), QPoint(0, 16), QPoint(999, 999) }) {
        FloodFillParams p;
        p.seed = bad;
        const FillResult r = floodFill(img, p);
        check(r.mask.isNull() && r.isEmpty(), "起点越界：返回空结果且不崩");
    }

    // ── 6.2 空图 ──
    {
        FloodFillParams p;
        p.seed = QPoint(0, 0);
        const FillResult r = floodFill(QImage(), p);
        check(r.mask.isNull() && r.isEmpty(), "空图：返回空结果");
    }

    // ── 6.3 1×1 ──
    {
        QImage one = makeImage(1, 1, kWhite);
        FloodFillParams p;
        p.seed = QPoint(0, 0);
        p.tolerance = 0;
        const FillResult r = floodFill(one, p);
        checkEq(maskAt(r.mask, 0, 0), 255, "1×1：被填");
        check(r.bounds == QRect(0, 0, 1, 1), "1×1：包围盒正确");
    }

    // ── 6.4 容差 0 + 全透明图：整张都是"同一个颜色"，应当全填 ──
    {
        QImage clear = makeImage(8, 8, kClear);
        FloodFillParams p;
        p.seed = QPoint(4, 4);
        p.tolerance = 0;
        p.contiguous = true;
        const FillResult r = floodFill(clear, p);
        checkEq(maskCount(r.mask), 64, "全透明图：容差 0 也填满整张（它们确实同色）");
    }

    // ── 6.5 容差 255：不管什么颜色都是同一个区域 ──
    {
        QImage mixed = makeImage(8, 8, kClear);
        fillRect(mixed, 0, 0, 3, 7, kRed);
        fillRect(mixed, 4, 0, 7, 7, kWhite);
        FloodFillParams p;
        p.seed = QPoint(0, 0);
        p.tolerance = 255;
        p.contiguous = true;
        const FillResult r = floodFill(mixed, p);
        checkEq(maskCount(r.mask), 64, "容差 255：整张图一次性连通");
    }

    // ── 6.6 非 ARGB32_Premultiplied 的输入也要能工作 ──
    {
        QImage rgb(8, 8, QImage::Format_RGB32);
        rgb.fill(QColor(200, 100, 50));
        FloodFillParams p;
        p.seed = QPoint(2, 2);
        p.tolerance = 0;
        const FillResult r = floodFill(rgb, p);
        checkEq(maskCount(r.mask), 64, "格式不是预乘 ARGB 时内部自动转换");
    }

    // ── 6.7 容差越界要被钳制，而不是越界读 ──
    {
        // ★ 必须用"两种颜色"的图来验 ★
        //   单色图上容差 0 也会填满整张（所有像素确实同色），
        //   拿它验"只填起点"永远验不出东西来 —— 第一版就栽在这里：
        //   断言写"期望 1、实际 256"，看起来像填充算法错了，
        //   其实是测试用例选错了底图。
        QImage two = makeImage(8, 8, kBlack);
        fillRect(two, 4, 0, 7, 7, kWhite);

        FloodFillParams p;
        p.contiguous = true;
        p.antialias = false;

        p.seed = QPoint(0, 0);
        p.tolerance = -50;                          // 负值 → 钳到 0
        checkEq(maskCount(floodFill(two, p).mask), 32, "负容差钳到 0：只填黑的那半");

        p.seed = QPoint(5, 5);
        checkEq(maskCount(floodFill(two, p).mask), 32, "负容差钳到 0：从白区点也只填白区");

        p.seed = QPoint(0, 0);
        p.tolerance = 1000;                         // 超界 → 钳到 255
        checkEq(maskCount(floodFill(two, p).mask), 64, "超界容差钳到 255：整张连通");
    }
}

// ---------------------------------------------------------------------------
//  7. composeFill —— 与画笔共用同一条合成路径
// ---------------------------------------------------------------------------
static void testComposeFill()
{
    std::printf("\n[7] composeFill（合成）\n");

    const QImage base = makeImage(4, 4, kWhite);

    // ── 7.1 覆盖度全 0 → 结果必须与输入逐位相同 ──
    {
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(0);
        const QImage out = composeFill(base, mask, kRed);
        bool same = true;
        for (int y = 0; y < 4 && same; ++y)
            for (int x = 0; x < 4 && same; ++x)
                if (rawPixel(out, x, y) != rawPixel(base, x, y))
                    same = false;
        check(same, "覆盖度全 0：像素逐位不变");
    }

    // ── 7.2 覆盖度全 255 + 不透明色 → 整块变成那个颜色 ──
    {
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(255);
        const QImage out = composeFill(base, mask, kRed);
        checkEq(rawPixel(out, 1, 1), kRed, "覆盖度 255：整块变红（含蓝色/绿色通道归零）");
        checkEq(rawPixel(out, 3, 3), kRed, "覆盖度 255：角落也是红的");
    }

    // ── 7.3 半覆盖：预乘空间里 alpha 应保持 255（底色本来就不透明） ──
    {
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(128);
        const QImage out = composeFill(base, mask, kRed);
        const QRgb c = rawPixel(out, 1, 1);
        checkEq(qAlpha(c), 255, "半覆盖叠在不透明底上：alpha 仍是 255");
        checkEq(qRed(c), 255, "半覆盖：红通道饱和");
        // 白底上盖半透明红 → 绿/蓝 ≈ 127
        check(qAbs(qGreen(c) - 127) <= 3 && qAbs(qBlue(c) - 127) <= 3,
              "半覆盖：绿/蓝通道落在白与红之间");
    }

    // ── 7.4 填充色的 alpha 就是"填充不透明度" ──
    {
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(255);

        // ★★ 这里踩到一个 Qt 的坑，值得单独钉住 ★★
        //
        //   QColor(qRgba(r, g, b, a)) 走的是 **QColor(QRgb)** 这个构造函数，
        //   而它**会把 alpha 丢掉**（置成 255）。
        //   想保留 alpha 必须用 QColor::fromRgba()。
        //
        //   症状：把"填充不透明度 0%"传进去，实际执行的是 100% ——
        //   油漆桶的对话框里把不透明度调到 0，图却被填满了。
        //   而且这个错误**没有任何征兆**：不崩、不报错，值看起来也对
        //   （alpha 字段读出来是 255，"不透明度 255/255 = 100%"逻辑自洽）。
        checkEq(QColor(qRgba(255, 0, 0, 0)).alpha(), 255,
                "QColor(QRgb) 会把 alpha 丢成 255（Qt 的既定行为，不是笔误）");
        checkEq(QColor::fromRgba(qRgba(255, 0, 0, 0)).alpha(), 0,
                "QColor::fromRgba 才保留 alpha");

        const QColor transparentRed = QColor::fromRgba(qRgba(255, 0, 0, 0));
        const QImage out = composeFill(base, mask, transparentRed);
        bool same = true;
        for (int y = 0; y < 4 && same; ++y)
            for (int x = 0; x < 4 && same; ++x)
                if (rawPixel(out, x, y) != rawPixel(base, x, y))
                    same = false;
        check(same, "填充色 alpha=0：等于什么都没填（不透明度语义）");
    }

    // ── 7.5 选区：只允许填左半 ──
    {
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(255);

        QImage sel(4, 4, QImage::Format_Grayscale8);
        sel.fill(0);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 2; ++x)
                reinterpret_cast<uchar *>(sel.scanLine(y))[x] = 255;

        const QImage out = composeFill(base, mask, kRed, sel);
        checkEq(rawPixel(out, 0, 0), kRed, "选区：选中的左半被填");
        checkEq(rawPixel(out, 1, 3), kRed, "选区：选中的左半（另一角）被填");
        checkEq(rawPixel(out, 2, 0), kWhite, "选区：右半**没**被填（选区限制生效）");
        checkEq(rawPixel(out, 3, 3), kWhite, "选区：右半（另一角）保持原样");
    }

    // ── 7.6 输入格式不是预乘 ARGB 时也能工作 ──
    {
        QImage rgbBase(4, 4, QImage::Format_RGB32);
        rgbBase.fill(QColor(0, 0, 0));
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(255);
        const QImage out = composeFill(rgbBase, mask, kRed);
        checkEq(qRed(rawPixel(out, 1, 1)), 255, "非预乘输入：填充仍然生效");
        checkEq(qBlue(rawPixel(out, 1, 1)), 0, "非预乘输入：颜色正确");
    }

    // ── 7.7 空输入不崩 ──
    {
        check(composeFill(QImage(), QImage(), kRed).isNull(), "空输入：不崩，返回空图");
        QImage mask(4, 4, QImage::Format_Grayscale8);
        mask.fill(255);
        check(composeFill(QImage(), mask, kRed).isNull(), "空底图：返回空图");
    }
}

// ---------------------------------------------------------------------------
//  8. 端到端：一张图上真的"点一下油漆桶"
// ---------------------------------------------------------------------------
static void testEndToEnd()
{
    std::printf("\n[8] 端到端\n");

    // 白底 + 一个黑框里的红方块，模拟"给图形内部填色"的常见用法
    QImage img = makeImage(64, 64, kWhite);
    fillRect(img, 16, 16, 47, 47, kBlack);      // 黑框（实心）
    fillRect(img, 20, 20, 43, 43, kWhite);      // 框内白区

    FloodFillParams p;
    p.seed = QPoint(30, 30);                    // 点在框内白区
    p.tolerance = 32;
    p.contiguous = true;
    const FillResult r = floodFill(img, p);

    check(!r.isEmpty(), "端到端：确实填到了东西");
    check(r.bounds == QRect(20, 20, 24, 24), "端到端：范围恰好是框内白区 24×24");

    const QImage out = composeFill(img, r.mask, kRed);
    checkEq(rawPixel(out, 30, 30), kRed, "端到端：框内变红");
    checkEq(rawPixel(out, 17, 17), kBlack, "端到端：黑框不受影响");
    checkEq(rawPixel(out, 2, 2), kWhite, "端到端：框外白底不受影响");
    checkEq(rawPixel(out, 5, 30), kWhite, "端到端：框外另一侧也不受影 ");
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 油漆桶填充引擎 离线自检\n");
    std::printf("================================================\n");

    testColorDistance();
    testFillDistance();
    testTolerance();
    testSoftBand();
    testContiguity();
    testRobustness();
    testComposeFill();
    testEndToEnd();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
