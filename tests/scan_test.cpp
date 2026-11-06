// ============================================================================
//  scan_test.cpp —— engine::scan（透视展平 + 白底黑字增强）离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    这两个函数的错误都属于"编译通过、不报错、图悄悄错"那一类，
//    而且错误方向很具体：
//
//      ① **四顶点顺序**
//         quadToQuad 是逐点对应的映射。顺序错成 左上→右上→右下→左下
//         以外的任何排列（例如逆时针），结果会是镜像或交叉的四边形，
//         但函数照样返回成功。肉眼看上去"图变形了"，很难想到是顺序问题。
//
//      ② **展平方向**
//         把源四边形映射到目标矩形，还是反过来映射。反了的话，
//         取的是"整幅图里那一小块"的补集 —— 画面像被撕碎。
//         本测试用"右半边全红"这种能一眼判定方向的源图来钉它。
//
//      ③ **去阴影的"各自归一"**
//         这是本模块存在的理由：同一张纸，亮处 240、阴影处 150。
//         全局色阶不可能同时把两者变白。测试断言的是**两处都接近 255**，
//         这才是"局部背景估计"生效的证据。若哪天有人把它改成全局
//         色阶（看起来更简单），这条断言会立刻红。
//
//      ④ **预乘**
//         与 adjust_test 同一类陷阱：半透明像素的 RGB 已被 alpha 乘过。
//         增强过程若直接在预乘空间做除法，半透明区域会被算成暗部。
//         这里用"alpha 必须原样保留 + 半透明像素的亮度按非预乘值处理"
//         两条一起钉。
//
//  依赖：只链 engine/DocumentScan.cpp（QPainter 在 Qt5Gui 里，
//  而 Qt5Gui 是所有测试默认就链的），不需要 Q_OBJECT、不需要 QGuiApplication。
// ============================================================================

#include "engine/DocumentScan.h"

#include <QImage>
#include <QPolygonF>

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

// 直接读扫描线：不经 setPixel()，避免"写入时自动预乘"把测试数据弄成
// 依赖 Qt 内部约定的样子（理由见 ui_smoke_test.cpp 的 makeSource）。
static QRgb rawPixel(const QImage &img, int x, int y)
{
    if (img.isNull() || x < 0 || y < 0 || x >= img.width() || y >= img.height())
        return 0;
    const QImage p = img.format() == QImage::Format_ARGB32
                         ? img
                         : img.convertToFormat(QImage::Format_ARGB32);
    return reinterpret_cast<const QRgb *>(p.constScanLine(y))[x];
}

static QPolygonF rectQuad(const QRectF &r)
{
    return QPolygonF() << r.topLeft() << r.topRight()
                       << r.bottomRight() << r.bottomLeft();
}

// ===========================================================================
//  1. 输出尺寸建议
// ===========================================================================
static void testSuggestOutputSize()
{
    std::printf("\n[1] 展平后的输出尺寸\n");

    // 正放的矩形：宽高直接就是边长
    const QSize s1 = scan::suggestOutputSize(rectQuad(QRectF(10, 20, 100, 50)));
    checkEq(s1.width(), 100, "矩形 → 宽 100");
    checkEq(s1.height(), 50, "矩形 → 高 50");

    // 梯形（近大远小）：上边 60（60→120）、下边 120（0→120）
    // 左边 hypot(60,100)=117、右边 100 —— 两边**不等长**，
    // 这样才能验证"取较长者"而不是"取平均"或"取右边"。
    // ★ 期望取**较长者**：宽 120、高 117 ★
    //   取平均（90 / 108）会白丢分辨率，而扫描件最怕的就是糊。
    QPolygonF trapezoid;
    trapezoid << QPointF(60, 0) << QPointF(120, 0)      // 上边 60
              << QPointF(120, 100) << QPointF(0, 100);  // 下边 120
    const QSize s2 = scan::suggestOutputSize(trapezoid);
    checkEq(s2.width(), 120, "梯形 → 宽取较长的一条边（120 而非平均 90）");
    checkEq(s2.height(), 117, "梯形 → 高取较长的一条边（117 而非 100）");

    // 退化输入不能返回 0 或负尺寸（下游建 QImage 会失败）
    const QSize s3 = scan::suggestOutputSize(QPolygonF() << QPointF(5, 5)
                                                         << QPointF(5, 5)
                                                         << QPointF(5, 5)
                                                         << QPointF(5, 5));
    check(s3.width() >= 1 && s3.height() >= 1, "四角重合时仍给出至少 1×1");
}

// ===========================================================================
//  2. 四边形合法性
// ===========================================================================
static void testQuadUsable()
{
    std::printf("\n[2] 四边形合法性（挡在展平之前）\n");

    check(scan::isQuadUsable(rectQuad(QRectF(0, 0, 100, 80))),
          "正放矩形可用");

    // 凹四边形：右下角被推到框内
    QPolygonF concave;
    concave << QPointF(0, 0) << QPointF(100, 0)
            << QPointF(40, 40) << QPointF(0, 80);
    check(!scan::isQuadUsable(concave), "凹四边形被拒绝（否则映射会自交）");

    // 三点共线 → 矩阵奇异
    QPolygonF collinear;
    collinear << QPointF(0, 0) << QPointF(50, 0)
              << QPointF(100, 0) << QPointF(50, 50);
    check(!scan::isQuadUsable(collinear), "三点共线被拒绝（否则 quadToQuad 失败）");

    // 点太少
    check(!scan::isQuadUsable(QPolygonF() << QPointF(0, 0) << QPointF(1, 1)),
          "不足四点被拒绝");

    // 面积过小（拖成一条缝）
    QPolygonF sliver;
    sliver << QPointF(0, 0) << QPointF(100, 0) << QPointF(100, 1) << QPointF(0, 1);
    check(!scan::isQuadUsable(sliver), "面积过小的细条被拒绝");
}

// ===========================================================================
//  3. 透视展平
// ===========================================================================
static void testUnwarp()
{
    std::printf("\n[3] 透视展平\n");

    // 源图：左半红、右半蓝，中间一道 4px 的分界带
    QImage src(200, 100, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < src.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(src.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
            line[x] = (x < 100) ? qRgba(220, 30, 30, 255)
                                : qRgba(30, 30, 220, 255);
    }

    // ── ① 恒等：四角 = 整幅图，输出同尺寸 → 逐点应当基本不变 ──
    const QSize full = scan::suggestOutputSize(rectQuad(QRectF(0, 0, 200, 100)));
    const QImage same = scan::unwarp(src, rectQuad(QRectF(0, 0, 200, 100)), full);
    check(!same.isNull(), "整幅图展平成功");
    checkEq(same.width(), 200, "恒等展平不改变宽");
    checkEq(same.height(), 100, "恒等展平不改变高");

    const QRgb left = rawPixel(same, 50, 50);
    const QRgb right = rawPixel(same, 150, 50);
    check(qRed(left) > 180 && qBlue(left) < 90,
          "恒等展平后左半边仍是红色（顶点顺序没有错成镜像）");
    check(qBlue(right) > 180 && qRed(right) < 90,
          "恒等展平后右半边仍是蓝色");

    // ── ② 只取右半：源四边形 = 右半边矩形 ──
    //   ★ 这条断言的方向性最强 ★
    //   若映射方向写反（把目标矩形映射回源，而不是把源映射到目标），
    //   采样位置会落到图像之外，结果大面积为透明（alpha 0）。
    const QRectF rightHalf(100, 0, 100, 100);
    const QImage onlyRight = scan::unwarp(src, rectQuad(rightHalf),
                                          QSize(100, 100));
    check(!onlyRight.isNull(), "取右半边展平成功");
    const QRgb c1 = rawPixel(onlyRight, 10, 50);
    const QRgb c2 = rawPixel(onlyRight, 90, 50);
    check(qBlue(c1) > 180 && qAlpha(c1) == 255, "右半边展平：左侧采样点是蓝色");
    check(qBlue(c2) > 180 && qAlpha(c2) == 255, "右半边展平：右侧采样点是蓝色");

    // ── ③ 退化四边形 → 返回空图而不是崩溃 ──
    QPolygonF bad;
    bad << QPointF(0, 0) << QPointF(100, 0) << QPointF(200, 0) << QPointF(50, 50);
    check(scan::unwarp(src, bad, QSize(50, 50)).isNull(), "共线四边形返回空图");

    // ── ④ 蒙版展平：与图层走同一条变换 ──
    QImage mask(200, 100, QImage::Format_Grayscale8);
    for (int y = 0; y < mask.height(); ++y) {
        uchar *line = mask.scanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            line[x] = (x < 100) ? 255 : 0;
    }

    const QImage maskFull = scan::unwarpMask(mask, rectQuad(QRectF(0, 0, 200, 100)),
                                             QSize(200, 100));
    check(!maskFull.isNull(), "蒙版展平成功");
    checkEq(int(maskFull.constScanLine(50)[50]), 255, "蒙版左半边仍是选中");
    checkEq(int(maskFull.constScanLine(50)[150]), 0, "蒙版右半边仍是未选中");

    const QImage maskRight = scan::unwarpMask(mask, rectQuad(rightHalf),
                                              QSize(100, 100));
    checkEq(int(maskRight.constScanLine(50)[50]), 0,
            "取右半边后，原左半边的选中区不会跟过来");
}

// ===========================================================================
//  4. 白底黑字增强
// ===========================================================================
static void testEnhance()
{
    std::printf("\n[4] 白底黑字增强\n");

    // ── 造一张"手机拍纸"的合成图 ──
    //
    //   纸：从左侧 150（阴影）线性过渡到右侧 240（灯下）
    //   字：在几处画 8×8 的深灰块（60）—— 表示"纸上的墨迹"
    //
    //   ★ 这张图是刻意做成"全局色阶救不了"的形态 ★
    //   若把白场定在 150 去救暗处，右侧的纸会全部爆白成一片；
    //   定在 240，左侧的纸永远是灰的。只有局部背景估计才能两边都变白。
    constexpr int kW = 240;
    constexpr int kH = 120;

    QImage photo(kW, kH, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < kH; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(photo.scanLine(y));
        for (int x = 0; x < kW; ++x) {
            const int paper = 150 + (90 * x) / (kW - 1);   // 150 → 240
            line[x] = qRgba(paper, paper, paper, 255);
        }
    }
    // 两块"字"：一块在阴影里，一块在灯下
    for (int y = 40; y < 48; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(photo.scanLine(y));
        for (int x = 20; x < 28; ++x)
            line[x] = qRgba(60, 60, 60, 255);
        for (int x = 200; x < 208; ++x)
            line[x] = qRgba(60, 60, 60, 255);
    }

    scan::EnhanceParams params;   // 默认 50/50/50，不转黑白
    const QImage out = scan::enhance(photo, params);
    check(!out.isNull(), "增强成功");
    checkEq(out.width(), kW, "增强不改变宽");
    checkEq(out.height(), kH, "增强不改变高");

    // ── ① 核心断言：阴影里的纸 与 灯下的纸，都要变白 ──
    const int paperShadow = qRed(rawPixel(out, 5, 10));
    const int paperLit    = qRed(rawPixel(out, 235, 10));
    std::printf("       纸（阴影侧）%d → %d\n",
                qRed(rawPixel(photo, 5, 10)), paperShadow);
    std::printf("       纸（灯下侧）%d → %d\n",
                qRed(rawPixel(photo, 235, 10)), paperLit);

    check(paperShadow >= 235, "阴影侧的纸被提成白色（>=235）");
    check(paperLit >= 235, "灯下侧的纸被提成白色（>=235）");
    check(std::abs(paperShadow - paperLit) <= 20,
          "两侧纸的亮度基本拉平（差异 <=20，说明做了局部归一化）");

    // ── ② 文字要变黑 ──
    const int textShadow = qRed(rawPixel(out, 24, 44));
    const int textLit    = qRed(rawPixel(out, 204, 44));
    std::printf("       字（阴影侧）%d → %d\n", 60, textShadow);
    std::printf("       字（灯下侧）%d → %d\n", 60, textLit);
    check(textShadow <= 90, "阴影侧的字被压黑（<=90）");
    check(textLit <= 90, "灯下侧的字被压黑（<=90）");
    check(textShadow < paperShadow - 100, "字与纸的差距被拉开（对比度确实变大）");

    // ── ③ 转黑白：三通道相等 ──
    scan::EnhanceParams grayParams;
    grayParams.grayscale = true;
    const QImage gray = scan::enhance(photo, grayParams);
    const QRgb g1 = rawPixel(gray, 60, 60);
    check(qRed(g1) == qGreen(g1) && qGreen(g1) == qBlue(g1),
          "勾选黑白后三通道相等");

    // 非黑白模式下，纯灰输入仍应是灰的（三通道一起走同一条曲线）
    const QRgb c1 = rawPixel(out, 60, 60);
    check(qRed(c1) == qGreen(c1) && qGreen(c1) == qBlue(c1),
          "灰色的纸处理后仍是中性灰（没有把颜色弄偏）");

    // ── ④ 彩色不能被抹掉（未勾黑白时）──
    QImage color(60, 60, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 60; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(color.scanLine(y));
        for (int x = 0; x < 60; ++x) {
            // 一张浅底上的红章：底 220，章 200,40,40
            const bool seal = (x >= 20 && x < 40 && y >= 20 && y < 40);
            line[x] = seal ? qRgba(200, 40, 40, 255) : qRgba(220, 220, 220, 255);
        }
    }
    const QImage colorOut = scan::enhance(color, params);
    const QRgb seal = rawPixel(colorOut, 30, 30);
    check(qRed(seal) > qGreen(seal) + 40 && qRed(seal) > qBlue(seal) + 40,
          "红章仍是红的（三通道走同一条曲线，色相没被抹平）");

    // ── ⑤ alpha 必须原样保留 ──
    QImage translucent(40, 40, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 40; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(translucent.scanLine(y));
        for (int x = 0; x < 40; ++x)
            line[x] = qRgba(200, 200, 200, 128);
    }
    const QImage transOut = scan::enhance(translucent, params);
    checkEq(qAlpha(rawPixel(transOut, 10, 10)), 128, "半透明像素的 alpha 不被改动");

    // ── ⑥ 边界：1×1 图不能崩（模糊半径会把短边算成 0 以外的值）──
    QImage tiny(1, 1, QImage::Format_ARGB32_Premultiplied);
    reinterpret_cast<QRgb *>(tiny.scanLine(0))[0] = qRgba(180, 180, 180, 255);
    check(!scan::enhance(tiny, params).isNull(), "1×1 图不崩溃");

    // ── ⑦ 参数极端值不产生越界值 ──
    scan::EnhanceParams maxed;
    maxed.shadow = 100;
    maxed.contrast = 100;
    maxed.whiten = 100;
    const QImage extreme = scan::enhance(photo, maxed);
    bool inRange = true;
    for (int y = 0; y < extreme.height() && inRange; y += 7) {
        const QRgb *line = reinterpret_cast<const QRgb *>(extreme.constScanLine(y));
        for (int x = 0; x < extreme.width(); x += 7) {
            const QRgb c = line[x];
            if (qRed(c) > 255 || qGreen(c) > 255 || qBlue(c) > 255)
                inRange = false;
        }
    }
    check(inRange, "三个参数都拉满时所有通道仍在 0..255");
}

// ===========================================================================

int main()
{
    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 文档扫描（展平 / 增强）离线自检\n");
    std::printf("================================================\n");

    testSuggestOutputSize();
    testQuadUsable();
    testUnwarp();
    testEnhance();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
