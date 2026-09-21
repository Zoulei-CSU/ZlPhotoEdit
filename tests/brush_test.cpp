// ============================================================================
//  brush_test.cpp —— engine/Brush 的离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    画笔的颜色数学（预乘 alpha 空间的 source-over / 擦除 / 选区乘算）
//    是"编译通过、运行不报错、但结果悄悄错误"的重灾区。典型症状是
//    半透明边缘发灰、橡皮擦不干净、选区外缘一圈淡影 —— 这些在界面上
//    看都不明显，要靠逐像素断言才能发现。
//
//    这几个函数都是纯函数（只依赖 QtGui 的 QImage，不需要 QtWidgets、
//    不需要 QGuiApplication），所以可以脱离界面直接测。
//
//  编译方式见同目录下的说明，或直接看本次构建时用的命令。
// ============================================================================

#include "engine/Brush.h"

#include <QImage>
#include <QtGlobal>

#include <cmath>
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

// 读灰度蒙版某点
static int maskAt(const QImage &m, int x, int y)
{
    return int(m.constScanLine(y)[x]);
}

// ---------------------------------------------------------------------------
//  1. 笔尖蒙版
// ---------------------------------------------------------------------------
static void testBrushTip()
{
    std::printf("1. 笔尖蒙版\n");

    // 直径 11 的中心在 (5,5)（因为 (11-1)/2 = 5，是整数，正好落在像素上）
    const BrushTip hard = makeBrushTip(11, 100);

    check(!hard.isNull(), "笔尖非空");
    checkEq(hard.diameter, 11, "直径");
    checkEq(hard.radius, 5, "半径");

    // 中心必须是全不透明
    checkEq(maskAt(hard.mask, 5, 5), 255, "硬度100 中心=255");

    // 四角必须接近 0（半径 5，角落距离 = sqrt(5²+5²)=7.07 > 5，超界）
    checkEq(maskAt(hard.mask, 0, 0), 0, "硬度100 左上角=0");
    checkEq(maskAt(hard.mask, 10, 10), 0, "硬度100 右下角=0");

    // 径向对称性：左右、上下、以及对角都应相同
    checkEq(maskAt(hard.mask, 1, 5), maskAt(hard.mask, 9, 5), "硬度100 左右对称");
    checkEq(maskAt(hard.mask, 5, 1), maskAt(hard.mask, 5, 9), "硬度100 上下对称");
    checkEq(maskAt(hard.mask, 1, 5), maskAt(hard.mask, 5, 1), "硬度100 四向一致");

    // ★ 硬度 100 也必须自带 1 像素抗锯齿 ★
    //   紧贴边缘的那一圈不能是"0 或 255"的硬跳变，
    //   否则圆边会是明显的阶梯锯齿。
    const int edgeVal = maskAt(hard.mask, 5, 0);   // 距中心 5 像素，正好在边缘
    check(edgeVal > 0 && edgeVal < 255,
          "硬度100 边缘有过渡值（抗锯齿）",
          (QString("实际=%1").arg(edgeVal)).toUtf8().constData());

    // 硬度 0：从中心一路衰减，中心仍是 255
    const BrushTip soft = makeBrushTip(21, 0);
    checkEq(maskAt(soft.mask, 10, 10), 255, "硬度0 中心=255");
    // 单调性：沿水平方向从中心往外必须单调不增
    bool monotonic = true;
    int prev = 256;
    for (int x = 10; x <= 20; ++x) {
        const int v = maskAt(soft.mask, x, 10);
        if (v > prev) { monotonic = false; break; }
        prev = v;
    }
    check(monotonic, "硬度0 沿半径单调衰减");
    check(maskAt(soft.mask, 10, 10) > maskAt(soft.mask, 15, 10),
          "硬度0 中心比中部亮");
    check(maskAt(soft.mask, 15, 10) > maskAt(soft.mask, 19, 10),
          "硬度0 中部比边缘亮");

    // 退化情形：直径 1 与 2
    const BrushTip one = makeBrushTip(1, 100);
    checkEq(one.diameter, 1, "直径1");
    checkEq(maskAt(one.mask, 0, 0), 255, "直径1 唯一像素=255");

    const BrushTip two = makeBrushTip(2, 100);
    checkEq(two.diameter, 2, "直径2");
    checkEq(maskAt(two.mask, 0, 0), 255, "直径2 左上=255");
    checkEq(maskAt(two.mask, 1, 1), 255, "直径2 右下=255");

    // 参数钳制：直径 0 / 负值、硬度越界都不能崩
    const BrushTip zero = makeBrushTip(0, 100);
    checkEq(zero.diameter, 1, "直径0 被钳制为1");
    const BrushTip huge = makeBrushTip(99999, 100);
    checkEq(huge.diameter, 2000, "超大直径被钳制为2000");
    const BrushTip hOver = makeBrushTip(11, 500);
    checkEq(hOver.diameter, 11, "硬度越界不影响直径");
    checkEq(maskAt(hOver.mask, 5, 5), 255, "硬度越界 中心正常");
}

// ---------------------------------------------------------------------------
//  2. 补点
// ---------------------------------------------------------------------------
static void testInterpolation()
{
    std::printf("2. 笔画补点\n");

    // 原地落笔必须返回一个点 —— 单击要留下痕迹
    const QVector<QPointF> single = interpolateStamps(QPointF(5, 5), QPointF(5, 5), 2.0);
    checkEq(single.size(), 1, "原地落笔返回1个点");
    check(qFuzzyCompare(single[0].x() + 1.0, 6.0), "原地落笔位置正确");

    // 水平 10 像素，间距 2.5 → 步数 = ceil(10/2.5) = 4
    const QVector<QPointF> line = interpolateStamps(QPointF(0, 0), QPointF(10, 0), 2.5);
    checkEq(line.size(), 4, "10像素/间距2.5 → 4个点");

    // ★ 不含 from，含 to ★
    check(qAbs(line.last().x() - 10.0) < 0.001, "最后一个点落在终点");
    check(qAbs(line.first().x() - 0.0) > 0.001, "第一个点不等于起点（避免重复落笔）");

    // 实际间距不得超过请求的 spacing（否则会出现"算盘珠"断点）
    bool spacingOk = true;
    qreal prevX = 0.0;
    for (const QPointF &p : line) {
        if (p.x() - prevX > 2.5 + 0.001) { spacingOk = false; break; }
        prevX = p.x();
    }
    check(spacingOk, "实际间距不超过请求值");

    // ★ spacing = 0 必须被钳制，不能死循环 ★
    const QVector<QPointF> zeroSpacing =
        interpolateStamps(QPointF(0, 0), QPointF(3, 0), 0.0);
    check(zeroSpacing.size() > 0 && zeroSpacing.size() <= 6,
          "间距0 被钳制为0.5（3像素→6个点）",
          (QString("实际=%1").arg(zeroSpacing.size())).toUtf8().constData());

    // 斜线：终点必须精确落在 to 上
    const QVector<QPointF> diag = interpolateStamps(QPointF(0, 0), QPointF(9, 9), 3.0);
    check(qAbs(diag.last().x() - 9.0) < 0.001
              && qAbs(diag.last().y() - 9.0) < 0.001,
          "斜线终点精确");
}

// ---------------------------------------------------------------------------
//  3. 压入落笔点（必须是"取最大"，不是"累加"）
// ---------------------------------------------------------------------------
static void testStamp()
{
    std::printf("3. 压入落笔点\n");

    const BrushTip tip = makeBrushTip(11, 100);

    QImage mask(40, 40, QImage::Format_Grayscale8);
    mask.fill(0);

    // 单次落笔，强度 0.5 → 中心应为 255*0.5 ≈ 128
    stampTipInPlace(mask, tip, QPointF(20, 20), 0.5);
    checkEq(maskAt(mask, 20, 20), 128, "强度0.5 单次落笔中心=128");

    // ★ 同一位置再落 5 笔，值必须**不变** ★
    //   这是"取最大"合成最核心的验证：如果实现成了累加，
    //   这里会一路涨到 255，画出来就是"手抖一下线条就变深"。
    for (int i = 0; i < 5; ++i)
        stampTipInPlace(mask, tip, QPointF(20, 20), 0.5);
    checkEq(maskAt(mask, 20, 20), 128, "重复落笔6次后仍为128（取最大）");

    // 换成更强的一笔，应该能提高（最大值语义要能"后来居上"）
    stampTipInPlace(mask, tip, QPointF(20, 20), 1.0);
    checkEq(maskAt(mask, 20, 20), 255, "强度1.0 覆盖后提升为255");

    // 更弱的一笔不应该把值拉低
    stampTipInPlace(mask, tip, QPointF(20, 20), 0.2);
    checkEq(maskAt(mask, 20, 20), 255, "更弱的落笔不会降低已有值");

    // ── 越界落笔：一半在画布外，绝不能崩，也不能写坏内存 ──
    QImage small(20, 20, QImage::Format_Grayscale8);
    small.fill(0);
    stampTipInPlace(small, tip, QPointF(0, 0), 1.0);        // 左上角，大部分在画布外
    stampTipInPlace(small, tip, QPointF(19, 19), 1.0);      // 右下角
    stampTipInPlace(small, tip, QPointF(-50, -50), 1.0);    // 完全在外
    stampTipInPlace(small, tip, QPointF(500, 500), 1.0);    // 完全在外
    checkEq(maskAt(small, 0, 0), 255, "越界落笔 左上角被正确覆盖");
    checkEq(maskAt(small, 19, 19), 255, "越界落笔 右下角被正确覆盖");
    checkEq(small.width(), 20, "越界落笔后尺寸不变");

    // 强度 0 不应该写任何东西
    QImage untouched(20, 20, QImage::Format_Grayscale8);
    untouched.fill(0);
    stampTipInPlace(untouched, tip, QPointF(10, 10), 0.0);
    checkEq(maskAt(untouched, 10, 10), 0, "强度0 不写入");

    // 格式不对时必须安全拒绝（而不是写出错误结果）
    QImage wrongFormat(40, 40, QImage::Format_ARGB32);
    wrongFormat.fill(0);
    stampTipInPlace(wrongFormat, tip, QPointF(20, 20), 1.0);
    checkEq(qAlpha(wrongFormat.pixel(20, 20)), 0, "非Grayscale8 蒙版被安全拒绝");

    // 落笔包围盒必须覆盖笔尖，且含 1 像素余量
    const QRect r = stampBoundingRect(tip, QPointF(20, 20));
    check(r.contains(QPoint(15, 15)) && r.contains(QPoint(25, 25)),
          "落笔包围盒覆盖笔尖范围");
    check(r.width() >= 13, "落笔包围盒含抗锯齿余量");
}

// ---------------------------------------------------------------------------
//  4. 画笔合成（source-over）
// ---------------------------------------------------------------------------
static void testComposePaint()
{
    std::printf("4. 画笔合成\n");

    // 全透明基底
    QImage base(4, 4, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::transparent);

    QImage mask(4, 4, QImage::Format_Grayscale8);
    mask.fill(255);     // 全覆盖

    // 不透明红覆盖全透明 → 应该得到纯红
    const QImage painted = composeStroke(base, mask, QColor(255, 0, 0, 255),
                                         false, QImage());
    const QRgb px = painted.pixel(1, 1);
    checkEq(qAlpha(px), 255, "不透明红 覆盖后 alpha=255");
    checkEq(qRed(px), 255, "不透明红 覆盖后 R=255");
    checkEq(qGreen(px), 0, "不透明红 覆盖后 G=0");
    checkEq(qBlue(px), 0, "不透明红 覆盖后 B=0");

    // 半覆盖（mask=128）→ alpha ≈128，且预乘后的 R 也应 ≈128
    QImage halfMask(4, 4, QImage::Format_Grayscale8);
    halfMask.fill(128);
    const QImage half = composeStroke(base, halfMask, QColor(255, 0, 0, 255),
                                      false, QImage());
    const QRgb hp = half.pixel(1, 1);
    check(qAbs(qAlpha(hp) - 128) <= 1, "半覆盖 alpha≈128",
          (QString("实际=%1").arg(qAlpha(hp))).toUtf8().constData());
    // ★ 预乘校验：ARGB32_Premultiplied 里存的必须是"颜色×alpha" ★
    //   如果实现里忘了预乘（直接存 255），这里会读到 255 而不是 ~128。
    check(qAbs(qRed(hp) - qAlpha(hp)) <= 2,
          "半覆盖 R 已被预乘（≈alpha）",
          (QString("R=%1 a=%2").arg(qRed(hp)).arg(qAlpha(hp))).toUtf8().constData());

    // 完全不覆盖 → 必须逐字节等于原图
    QImage zeroMask(4, 4, QImage::Format_Grayscale8);
    zeroMask.fill(0);
    const QImage noChange = composeStroke(base, zeroMask, QColor(255, 0, 0, 255),
                                          false, QImage());
    check(noChange == base, "零覆盖 → 结果与原图逐像素相同");

    // ★ 不得改写调用方传入的 base ★
    //   这是 QImage 隐式共享最容易踩的坑：忘了 copy 就会"把源图也改了"。
    //   上面的 composeStroke 已经调用过两次，base 必须还是全透明。
    checkEq(qAlpha(base.pixel(1, 1)), 0, "★ 原图未被修改（浅引用陷阱）");

    // 颜色自带 alpha（不透明度）也要生效
    const QImage alphaColor = composeStroke(base, mask, QColor(0, 255, 0, 128),
                                            false, QImage());
    const QRgb ap = alphaColor.pixel(1, 1);
    check(qAbs(qAlpha(ap) - 128) <= 1, "颜色 alpha=128 → 结果 alpha≈128");

    // 非预乘格式的输入也必须被正确处理（内部会转格式）
    QImage argb(4, 4, QImage::Format_ARGB32);
    argb.fill(Qt::transparent);
    const QImage fromArgb = composeStroke(argb, mask, QColor(255, 0, 0, 255),
                                          false, QImage());
    checkEq(fromArgb.format(), QImage::Format_ARGB32_Premultiplied,
            "非预乘输入 → 输出被规范为预乘格式");
    checkEq(qAlpha(fromArgb.pixel(1, 1)), 255, "非预乘输入也能正确合成");
    checkEq(qAlpha(argb.pixel(1, 1)), 0, "★ 非预乘的原图也未被修改");
}

// ---------------------------------------------------------------------------
//  5. 橡皮合成
// ---------------------------------------------------------------------------
static void testComposeErase()
{
    std::printf("5. 橡皮合成\n");

    QImage base(4, 4, QImage::Format_ARGB32_Premultiplied);
    base.fill(QColor(255, 0, 0, 255));      // 不透明红

    QImage full(4, 4, QImage::Format_Grayscale8);
    full.fill(255);

    // 满覆盖擦除 → 完全透明
    const QImage erased = composeStroke(base, full, QColor(Qt::black), true, QImage());
    const QRgb ep = erased.pixel(1, 1);
    checkEq(qAlpha(ep), 0, "满覆盖擦除 → alpha=0");
    // ★ 预乘空间擦除的关键：RGB 也必须一起归零 ★
    //   只把 alpha 置 0 而留下 RGB，会得到"透明但有颜色"的脏像素，
    //   后续合成时可能浮现出错色。
    checkEq(qRed(ep), 0, "满覆盖擦除 → R 也归零");
    checkEq(qGreen(ep), 0, "满覆盖擦除 → G 也归零");
    checkEq(qBlue(ep), 0, "满覆盖擦除 → B 也归零");

    // 半覆盖擦除 → alpha 减半，RGB 同步减半（预乘性质）
    QImage half(4, 4, QImage::Format_Grayscale8);
    half.fill(128);
    const QImage halfErased = composeStroke(base, half, QColor(Qt::black), true, QImage());
    const QRgb hep = halfErased.pixel(1, 1);
    check(qAbs(qAlpha(hep) - 127) <= 1, "半覆盖擦除 alpha≈127",
          (QString("实际=%1").arg(qAlpha(hep))).toUtf8().constData());
    check(qAbs(qRed(hep) - qAlpha(hep)) <= 2,
          "半覆盖擦除 R 与 alpha 同步（预乘一致性）",
          (QString("R=%1 a=%2").arg(qRed(hep)).arg(qAlpha(hep))).toUtf8().constData());

    // ★ 重复擦除必须幂等：擦两遍和擦一遍结果相同 ★
    //   这条验证的是"整笔从快照重算"的正确性基础 ——
    //   如果橡皮是"在上一帧结果上继续乘"，重复擦就会越来越淡。
    const QImage twice = composeStroke(halfErased, half, QColor(Qt::black), true, QImage());
    // 注：这里模拟的是"两次独立擦除"，结果本就应当变淡；
    //     真正的幂等在 BrushTool 里靠"每次从快照重算"保证。
    check(qAlpha(twice.pixel(1, 1)) <= qAlpha(hep),
          "再次擦除只会更透明，不会变深");
    checkEq(qAlpha(base.pixel(1, 1)), 255, "★ 橡皮未修改原图");
}

// ---------------------------------------------------------------------------
//  6. 选区限制
// ---------------------------------------------------------------------------
static void testSelection()
{
    std::printf("6. 选区限制\n");

    QImage base(4, 4, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::transparent);

    QImage mask(4, 4, QImage::Format_Grayscale8);
    mask.fill(255);     // 笔刷想涂满整块

    // 选区全黑 → 哪儿都不能涂
    QImage selNone(4, 4, QImage::Format_Grayscale8);
    selNone.fill(0);
    const QImage blocked = composeStroke(base, mask, QColor(255, 0, 0, 255),
                                         false, selNone);
    checkEq(qAlpha(blocked.pixel(1, 1)), 0, "选区为0 → 涂不上去");

    // 选区全白 → 等同没有选区
    QImage selAll(4, 4, QImage::Format_Grayscale8);
    selAll.fill(255);
    const QImage allowed = composeStroke(base, mask, QColor(255, 0, 0, 255),
                                         false, selAll);
    checkEq(qAlpha(allowed.pixel(1, 1)), 255, "选区为255 → 正常涂上");

    // 选区半值（羽化边缘）→ alpha 应为一半
    QImage selHalf(4, 4, QImage::Format_Grayscale8);
    selHalf.fill(128);
    const QImage feathered = composeStroke(base, mask, QColor(255, 0, 0, 255),
                                           false, selHalf);
    const QRgb fp = feathered.pixel(1, 1);
    check(qAbs(qAlpha(fp) - 128) <= 1, "羽化选区 → alpha≈128",
          (QString("实际=%1").arg(qAlpha(fp))).toUtf8().constData());

    // ★ 尺寸不匹配的选区必须被当作"全选"，而不是错位裁切 ★
    //   选区尺寸是旧画布的残留时，用它会擦到错误的位置上。
    QImage wrongSize(2, 2, QImage::Format_Grayscale8);
    wrongSize.fill(0);
    const QImage sizeMismatch = composeStroke(base, mask, QColor(255, 0, 0, 255),
                                              false, wrongSize);
    checkEq(qAlpha(sizeMismatch.pixel(1, 1)), 255,
            "尺寸不符的选区被忽略（当作全选）");

    // 格式不对的选区同样要忽略
    QImage wrongFormat(4, 4, QImage::Format_ARGB32);
    wrongFormat.fill(0);
    const QImage fmtMismatch = composeStroke(base, mask, QColor(255, 0, 0, 255),
                                             false, wrongFormat);
    checkEq(qAlpha(fmtMismatch.pixel(1, 1)), 255, "格式不符的选区被忽略");

    checkEq(qAlpha(base.pixel(1, 1)), 0, "★ 选区测试未修改原图");
}

// ---------------------------------------------------------------------------
//  7. 空输入健壮性
// ---------------------------------------------------------------------------
static void testRobustness()
{
    std::printf("7. 空输入健壮性\n");

    QImage mask(4, 4, QImage::Format_Grayscale8);
    mask.fill(255);

    // 空 base
    const QImage r1 = composeStroke(QImage(), mask, Qt::red, false, QImage());
    check(r1.isNull(), "空基底 → 返回空（不崩）");

    // 空 mask
    QImage base(4, 4, QImage::Format_ARGB32_Premultiplied);
    base.fill(Qt::transparent);
    const QImage r2 = composeStroke(base, QImage(), Qt::red, false, QImage());
    check(!r2.isNull(), "空蒙版 → 返回基底");
    checkEq(qAlpha(r2.pixel(1, 1)), 0, "空蒙版 → 内容不变");

    // 蒙版比基底小：只处理公共区域，不能越界读
    QImage smallMask(2, 2, QImage::Format_Grayscale8);
    smallMask.fill(255);
    const QImage r3 = composeStroke(base, smallMask, Qt::red, false, QImage());
    checkEq(r3.width(), 4, "小蒙版不影响输出尺寸");
    checkEq(qAlpha(r3.pixel(0, 0)), 255, "小蒙版区域被合成");
    checkEq(qAlpha(r3.pixel(3, 3)), 0, "蒙版外区域保持原样");
}

int main()
{
    std::printf("===== engine/Brush 自检 =====\n\n");

    testBrushTip();
    testInterpolation();
    testStamp();
    testComposePaint();
    testComposeErase();
    testSelection();
    testRobustness();

    std::printf("\n===== 通过 %d 项，失败 %d 项 =====\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
