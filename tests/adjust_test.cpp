// ============================================================================
//  adjust_test.cpp —— engine 层的调整/曲线/直方图离线自检
// ----------------------------------------------------------------------------
//  为什么专门写这个测试：
//
//    这三个模块共同承担"把像素值映射成另一个值"的职责，而它们的错误
//    几乎全都是**编译通过、运行不报错、画面悄悄错**的类型：
//
//      ① **预乘陷阱**（最隐蔽）
//         图层是 ARGB32_Premultiplied，RGB 分量已经乘过 alpha。
//         如果直接拿预乘后的分量去查表，半透明区域会被当成"暗部"处理。
//         一张 50% 透明的白色，预乘后是 128 —— 查表时它被当作中间灰，
//         结果颜色明显不对，但只有半透明像素受影响，很容易被忽略。
//         本测试用"精确到个位数"的断言把它钉死。
//
//      ② **曲线过冲**
//         用 Catmull-Rom 插值时，陡峭的 S 形会在段内回勾，画面出现
//         暗部色带与反相噪点。这种错误在缩略图上完全看不出来，
//         必须靠"256 个输出值全面单调"的断言来守。
//
//      ③ **直方图算错空间**
//         同样因为预乘：半透明白像素会被统计到亮度 128 的桶里，
//         于是"调色时看的分布"和"实际画面"不符，用户会照着错的
//         直方图去调曲线。
//
//    这些函数都不需要 QGuiApplication —— QImage 的格式转换与逐像素
//    访问在无 GUI 环境下就能工作。编译方式见 run_tests.sh。
// ============================================================================

#include "engine/Adjust.h"
#include "engine/Curves.h"
#include "engine/Histogram.h"
#include "engine/Lut.h"

#include <QImage>
#include <QList>
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

// ---------------------------------------------------------------------------
//  工具
// ---------------------------------------------------------------------------

// ★ 读"原始存储值"而不是 QImage::pixel() ★
//   QImage::pixel() 对预乘格式会做反预乘转换（把 (128,128,128,128) 报成
//   255,255,255,128），于是"预乘陷阱"这类 bug 会被它粉饰掉 ——
//   用它写断言等于白测。这里直接读扫描线，看到的就是内存里真实的值。
static QRgb rawPixel(const QImage &img, int x, int y)
{
    const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
    return line[x];
}

static QImage makeSolid(int w, int h, QRgb premulValue)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(premulValue);
    return img;
}

// 全部 256 项单调不减 —— 曲线"不过冲"的可操作定义
static bool isNonDecreasing(const CurveLut &lut)
{
    for (int i = 1; i < 256; ++i) {
        if (lut[size_t(i)] < lut[size_t(i - 1)])
            return false;
    }
    return true;
}

// 单调不减 **或** 单调不增 —— 都算"方向一致、无回勾"。
//
// ★ 为什么判据不能写成"必须不减" ★
//   内置的"反相（负片）"预设就是 (0,1) → (1,0) 的一条递减直线，
//   它当然不是"不减"的。但它是完全正确的。
//
//   真正危险的不是曲线的方向，而是**段内回勾**：曲线在某一小段里
//   先升后降（或反之），映射到图像上就是原本连续的渐变被压出色带、
//   高光里冒出反相的噪点。所以判据是"整体方向一致"。
static bool isMonotone(const CurveLut &lut)
{
    bool nonDecreasing = true;
    bool nonIncreasing = true;
    for (int i = 1; i < 256; ++i) {
        if (lut[size_t(i)] < lut[size_t(i - 1)])
            nonDecreasing = false;
        if (lut[size_t(i)] > lut[size_t(i - 1)])
            nonIncreasing = false;
    }
    return nonDecreasing || nonIncreasing;
}

static bool lutIsIdentity(const CurveLut &lut)
{
    for (int i = 0; i < 256; ++i) {
        if (int(lut[size_t(i)]) != i)
            return false;
    }
    return true;
}

// 把"预乘值 255 → 100、128 → 7"这条表造出来。
// 它的用途是区分"查表前有没有反预乘"：
//   正确：半透明白先反预乘成 255 → 查表得 100 → 再预乘回 50
//   错误：直接拿预乘值 128 查表得 7
static CurveLut makeOverriddenLut()
{
    CurveLut lut = identityLut();
    lut[255] = 100;
    lut[128] = 7;
    return lut;
}

static CurveLut makeInvertLut()
{
    CurveLut lut{};
    for (int i = 0; i < 256; ++i)
        lut[size_t(i)] = quint8(255 - i);
    return lut;
}

// ---------------------------------------------------------------------------
//  1. ★ 核心回归：LUT 的预乘往返 ★
// ---------------------------------------------------------------------------
static void testLutPremultiplied()
{
    std::printf("\n[1] LUT 与预乘 alpha\n");

    // 50% 透明的纯白：预乘后 R = 255 * 128/255 = 128
    const QImage halfWhite = makeSolid(8, 8, qRgba(128, 128, 128, 128));

    // ── 1.1 恒等表：半透明像素必须逐位不变 ──
    const QImage same = applyLutRgb(halfWhite, identityLut());
    checkEq(qRed(rawPixel(same, 0, 0)), 128, "恒等表不改变预乘分量");
    checkEq(qAlpha(rawPixel(same, 0, 0)), 128, "恒等表不改变 alpha");

    // ── 1.2 ★ 决定性的断言 ★ ──
    //     若漏了反预乘，128 会去查 128 那一档（=7）；
    //     正确实现是 255 去查表（=100），再乘回 alpha 得 50。
    //     7 与 50 差了 7 倍，绝不可能混淆。
    const QImage mapped = applyLutRgb(halfWhite, makeOverriddenLut());
    const int gotR = qRed(rawPixel(mapped, 0, 0));
    check(gotR >= 48 && gotR <= 52,
          "半透明像素查的是「反预乘后的值」", "(期望约 50)");
    if (gotR < 48 || gotR > 52)
        std::printf("         实际 %d（若为 7 说明漏了反预乘）\n", gotR);

    // ── 1.3 同一张表作用在不透明像素上：必须得到表里的原值 ──
    const QImage opaqueWhite = makeSolid(8, 8, qRgba(255, 255, 255, 255));
    checkEq(qRed(rawPixel(applyLutRgb(opaqueWhite, makeOverriddenLut()), 0, 0)),
            100, "不透明像素直接得到表值");

    // ── 1.4 反相：50% 白应当变成 50% 黑（R=0），而不是变成中间灰 ──
    const QImage inverted = applyLutRgb(halfWhite, makeInvertLut());
    const int invR = qRed(rawPixel(inverted, 0, 0));
    check(invR <= 2, "反相后半透明白变半透明黑", "(期望 0)");
    if (invR > 2)
        std::printf("         实际 %d\n", invR);
    checkEq(qAlpha(rawPixel(inverted, 0, 0)), 128, "反相不动 alpha");

    // ── 1.5 全透明像素：alpha 保持 0，不产生鬼影 ──
    const QImage transparent = makeSolid(8, 8, qRgba(0, 0, 0, 0));
    checkEq(qAlpha(rawPixel(applyLutRgb(transparent, makeInvertLut()), 0, 0)), 0,
            "全透明像素保持全透明");

    // ── 1.6 composeLut：先 a 后 b 等价于先查 a 再查 b ──
    const CurveLut a = makeInvertLut();
    const CurveLut b = makeOverriddenLut();
    const CurveLut ab = composeLut(a, b);
    bool composeOk = true;
    for (int i = 0; i < 256; ++i) {
        if (int(ab[size_t(i)]) != int(b[size_t(a[size_t(i)])])) {
            composeOk = false;
            break;
        }
    }
    check(composeOk, "composeLut 与连续两次查表一致");

    check(lutIsIdentity(composeLut(identityLut(), identityLut())),
          "两张恒等表合成仍是恒等表");

    // ── 1.7 ★ 彩色像素：三个通道必须各查各的表 ★ ────────────────────────
    //
    //  为什么这一段必须存在（它抓到过一个真实且严重的 bug）：
    //
    //    ChannelLuts::isMonochrome() 表示"三条曲线长得一样"，
    //    曾经在 applyLuts 里被误解成"像素的 R/G/B 三个分量一样"，
    //    于是写成：
    //        if (mono) { const int v = lutR[ir]; r = g = b = v; }
    //    即只查红通道再广播到三通道。
    //
    //    由于 applyLutRgb() 构造出的 ChannelLuts 天然满足 isMonochrome()，
    //    这条错误路径覆盖了**亮度/对比度/RGB 主曲线的全部调用** ——
    //    用户拉一下亮度滑块，彩色照片会整体变成"用红通道值渲染"的单色图。
    //
    //    ★ 而上面 1.1~1.6 全部通过，因为它们用的都是灰度像素
    //      （R == G == B）。灰度图上两种写法结果完全相同。
    //      **这就是"只用灰像素测 LUT"的致命盲区。**
    //
    //  所以这里一律用 R≠G≠B 的彩色像素。
    const QImage colored = makeSolid(8, 8, qRgba(128, 96, 64, 255));

    // 1.7a 恒等表：彩色像素必须逐位不变（最强的探测器 —— 任何广播
    //      都会把 G/B 拉平成 R 的值）
    const QImage coloredSame = applyLutRgb(colored, identityLut());
    checkEq(qRed(rawPixel(coloredSame, 0, 0)),   128, "恒等表下彩色像素 R 不变");
    checkEq(qGreen(rawPixel(coloredSame, 0, 0)),  96, "恒等表下彩色像素 G 不变");
    checkEq(qBlue(rawPixel(coloredSame, 0, 0)),   64, "恒等表下彩色像素 B 不变");

    // 1.7b 真实调整：三通道各自查表 → 输入不同则输出不同
    const QImage coloredAdj =
        applyLutRgb(colored, adjust::brightnessContrastLut(50, 30));
    checkEq(qAlpha(rawPixel(coloredAdj, 0, 0)), 255, "调整彩色像素不破坏 alpha");
    // 灰阶顺序必须保持：输入的 R > G > B 在提亮后应仍然成立。
    // 广播 bug 会让三者完全相等，这条能独立抓到它。
    check(qRed(rawPixel(coloredAdj, 0, 0)) > qGreen(rawPixel(coloredAdj, 0, 0))
              && qGreen(rawPixel(coloredAdj, 0, 0)) > qBlue(rawPixel(coloredAdj, 0, 0)),
          "通道间大小关系被保留（未发生通道广播）",
          "若三值相等即说明 G/B 被 R 覆盖");

    // 1.7c 三张**互不相同**的表：每个通道必须走自己那张
    ChannelLuts perChannel;
    perChannel.r = makeInvertLut();          // r: 128 → 127
    perChannel.g = identityLut();            // g: 96  → 96
    perChannel.b = makeOverriddenLut();      // b: 64 未被覆盖 → 64
    check(!perChannel.isMonochrome(), "三张不同的表不算 monochrome");
    const QImage perChannelRes = applyLuts(colored, perChannel);
    checkEq(qRed(rawPixel(perChannelRes, 0, 0)),   127, "R 通道走自己的表（反相）");
    checkEq(qGreen(rawPixel(perChannelRes, 0, 0)),  96, "G 通道走自己的表（恒等）");
    checkEq(qBlue(rawPixel(perChannelRes, 0, 0)),   64, "B 通道走自己的表（覆盖表未命中 64）");

    // 1.7d 半透明 + 彩色：预乘往返与"各查各的表"必须同时成立。
    //      预乘空间里这一个像素是 (64, 48, 32, 128)。
    const QImage coloredHalf = makeSolid(8, 8, qRgba(64, 48, 32, 128));
    const QImage coloredHalfSame = applyLutRgb(coloredHalf, identityLut());
    checkEq(qAlpha(rawPixel(coloredHalfSame, 0, 0)), 128, "彩色半透明像素 alpha 不变");
    checkEq(qRed(rawPixel(coloredHalfSame, 0, 0)),    64, "彩色半透明像素 R 往返不变");
    checkEq(qGreen(rawPixel(coloredHalfSame, 0, 0)),  48, "彩色半透明像素 G 往返不变");
    checkEq(qBlue(rawPixel(coloredHalfSame, 0, 0)),   32, "彩色半透明像素 B 往返不变");
}

// ---------------------------------------------------------------------------
//  2. 亮度 / 对比度 / 色阶
// ---------------------------------------------------------------------------
static void testBasicAdjust()
{
    std::printf("\n[2] 亮度 / 对比度 / 色阶\n");

    // 参数为 0 时必须是恒等表 —— 否则"打开对话框什么都没动"也会改画质
    check(lutIsIdentity(adjust::brightnessLut(0)), "亮度 0 是恒等表");
    check(lutIsIdentity(adjust::contrastLut(0)), "对比度 0 是恒等表");
    check(lutIsIdentity(adjust::brightnessContrastLut(0, 0)),
          "亮度对比度都为 0 是恒等表");

    // 亮度 +50 必须整体提亮，且暗部的升幅要被压缩（"轻微 S 形收敛"）。
    //
    // ★ 这里刻意不断言"纯黑保持 0" ★
    //   Adjust.h 明确写明了实现选择：亮度用**加法**而不是乘法，
    //   加法会让黑场一起抬高（乘法才保持黑场不动，但高光容易过曝）。
    //   加法的副作用由一个 S 形收敛来缓解：越靠近两端，变化率越小。
    //   所以正确的期望是"黑场被抬起，但抬的幅度明显小于中间调"，
    //   而不是"黑场纹丝不动"。写成后者会与设计文档冲突，
    //   而且会把一个被记录过的取舍误判成 bug。
    const CurveLut brighter = adjust::brightnessLut(50);
    const int liftAtBlack = int(brighter[0]) - 0;
    const int liftAtMid   = int(brighter[128]) - 128;
    check(brighter[128] > 128, "亮度 +50 抬高中灰");
    check(liftAtBlack < liftAtMid, "亮度提亮时暗部升幅被压缩（S 形收敛）");
    check(liftAtBlack <= 48, "亮度 +50 不会把黑场抬成灰雾");
    if (liftAtBlack >= liftAtMid)
        std::printf("         黑场升幅 %d，中间调升幅 %d\n", liftAtBlack, liftAtMid);
    checkEq(int(brighter[255]), 255, "亮度不能把纯白压下去");
    check(isNonDecreasing(brighter), "亮度表单调不减");

    // 对比度是围绕中灰缩放：暗部更暗、亮部更亮、中灰基本不动
    const CurveLut contrast = adjust::contrastLut(50);
    check(contrast[64] < 64, "对比度 +50 压暗暗部");
    check(contrast[192] > 192, "对比度 +50 提亮亮部");
    check(std::abs(int(contrast[128]) - 128) <= 2, "对比度基本不动中灰");
    check(isNonDecreasing(contrast), "对比度表单调不减");

    // 色阶：默认参数 = 恒等
    adjust::LevelsParams def;
    check(lutIsIdentity(adjust::levelsLut(def)), "色阶默认参数是恒等表");

    // gamma > 1 变亮（Photoshop 的约定）
    adjust::LevelsParams g = def;
    g.gamma = 2.0;
    const CurveLut gammaLut = adjust::levelsLut(g);
    check(gammaLut[128] > 128, "gamma 2.0 提亮中间调");
    checkEq(int(gammaLut[0]), 0, "gamma 不动黑场");
    checkEq(int(gammaLut[255]), 255, "gamma 不动白场");

    // 输入黑白场：把 50..200 拉伸到 0..255
    adjust::LevelsParams stretch;
    stretch.inBlack = 50;
    stretch.inWhite = 200;
    const CurveLut stretchLut = adjust::levelsLut(stretch);
    checkEq(int(stretchLut[50]), 0, "输入黑场映射为 0");
    checkEq(int(stretchLut[200]), 255, "输入白场映射为 255");
    check(isNonDecreasing(stretchLut), "色阶表单调不减");

    // ── 色阶的参数校验：界面能敲出的一切都必须在引擎侧兜住 ──
    //
    //   ★ 为什么值得单独钉住 ★
    //     色阶对话框的黑白场是数值框，用户可以在两次输入之间敲出
    //     inBlack > inWhite；"自动"在纯色图上也会算出黑白场重合。
    //     引擎必须先于任何除零/NaN 发生之前把它们处理掉 ——
    //     否则症状是"整幅突然变成纯黑或纯白"，而根因藏在浮点运算里。
    {
        adjust::LevelsParams crossed;
        crossed.inBlack = 200;
        crossed.inWhite = 50;
        const CurveLut l = adjust::levelsLut(crossed);

        // 引擎的约定是"交换"而不是报错（见 Adjust.cpp 的注释）：
        // 交换后等价于 inBlack = 50、inWhite = 200
        checkEq(int(l[50]), 0, "黑白场交叉时被交换：原白场位置映射为 0");
        checkEq(int(l[200]), 255, "黑白场交叉时被交换：原黑场位置映射为 255");
        check(isNonDecreasing(l), "交叉参数交换后仍是单调表（没有除零）");
    }

    {
        // 完全重合：纯色图上的自动色阶会给出这种结果
        adjust::LevelsParams same;
        same.inBlack = 128;
        same.inWhite = 128;
        const CurveLut l = adjust::levelsLut(same);
        checkEq(int(l[0]), 0, "黑白场重合时暗端映射为 0");
        checkEq(int(l[255]), 255, "黑白场重合时亮端映射为 255");
        check(isNonDecreasing(l), "黑白场重合时强制留出最小范围，不会除零");
    }

    {
        // 极端 gamma：0 会让 qPow 产生 inf/NaN，必须被钳制
        adjust::LevelsParams g0;
        g0.gamma = 0.0;
        const CurveLut l0 = adjust::levelsLut(g0);
        checkEq(int(l0[0]), 0, "gamma = 0 被钳制：黑端不产生 NaN");
        checkEq(int(l0[255]), 255, "gamma = 0 被钳制：白端不溢出");

        adjust::LevelsParams gBig;
        gBig.gamma = 1000.0;
        const CurveLut lBig = adjust::levelsLut(gBig);
        checkEq(int(lBig[0]), 0, "gamma 极大时黑端保持 0");
        checkEq(int(lBig[255]), 255, "gamma 极大时白端保持 255");
        check(isNonDecreasing(lBig), "gamma 极大时不出现反向映射");
    }

    {
        // 输出范围反转 = 负片效果。
        // levelsLut() 不约束 outBlack < outWhite，这是有意的 —— 它就是
        // "反相"的一种表达方式。这里只钉住"不会溢出成乱值"。
        adjust::LevelsParams neg;
        neg.outBlack = 255;
        neg.outWhite = 0;
        const CurveLut l = adjust::levelsLut(neg);
        checkEq(int(l[0]), 255, "输出范围反转时黑端映射到 255（负片，不是溢出）");
        checkEq(int(l[255]), 0, "输出范围反转时白端映射到 0");
    }

    // ── 自动色阶：必须忽略极端噪点 ──
    //   造一张"渐变 50..200 + 两个噪点 (0 与 255)"的图。
    //   若按 min/max 定黑白场，结果会是 0/255（等于什么都没做）；
    //   按分位数才会得到接近 50/200。
    QImage ramp(100, 100, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            const int v = 50 + (200 - 50) * x / 99;
            ramp.setPixel(x, y, qRgba(v, v, v, 255));
        }
    }
    ramp.setPixel(0, 0, qRgba(0, 0, 0, 255));       // 噪点：纯黑
    ramp.setPixel(99, 99, qRgba(255, 255, 255, 255)); // 噪点：纯白

    const adjust::LevelsParams autoLv = adjust::computeAutoLevels(ramp, 0.1);
    check(autoLv.inBlack >= 20 && autoLv.inBlack <= 80, "自动色阶忽略暗部噪点");
    check(autoLv.inWhite >= 180 && autoLv.inWhite <= 240, "自动色阶忽略亮部噪点");
    if (autoLv.inBlack < 20 || autoLv.inBlack > 80)
        std::printf("         inBlack 实际 %d\n", autoLv.inBlack);
    if (autoLv.inWhite < 180 || autoLv.inWhite > 240)
        std::printf("         inWhite 实际 %d\n", autoLv.inWhite);
}

// ---------------------------------------------------------------------------
//  3. 饱和度 / 色相 / 明度
// ---------------------------------------------------------------------------
static void testColorAdjust()
{
    std::printf("\n[3] 饱和度 / 色相 / 明度\n");

    // 造一张有明显颜色的图（纯红）
    const QImage red = makeSolid(8, 8, qRgba(255, 0, 0, 255));

    // 参数全 0 必须是恒等（允许 ±1 的舍入误差，因为这三项运算
    // 内部要走浮点，完全不差 1 不现实）
    const QImage same = adjust::applyHueSaturationLightness(red, 0, 0, 0);
    const QRgb p = rawPixel(same, 0, 0);
    check(std::abs(qRed(p) - 255) <= 1 && std::abs(qGreen(p) - 0) <= 1
              && std::abs(qBlue(p) - 0) <= 1,
          "色相饱和度明度都为 0 时不变");

    // 饱和度 -100 = 完全去色：三个通道必须相等
    const QRgb gray = rawPixel(adjust::applySaturation(red, -100), 0, 0);
    checkEq(qRed(gray), qGreen(gray), "饱和度 -100 后 R==G");
    checkEq(qGreen(gray), qBlue(gray), "饱和度 -100 后 G==B");
    // 纯红的亮度按 0.299 权重应为 76
    check(std::abs(qRed(gray) - 76) <= 2, "去色用 0.299/0.587/0.114 权重");

    // 色相旋转 0 度不变；旋转 120 度应把红变成绿（近似）
    const QRgb hue120 = rawPixel(adjust::applyHue(red, 120), 0, 0);
    check(qGreen(hue120) > qRed(hue120) && qGreen(hue120) > qBlue(hue120),
          "色相 +120° 把红转成绿");

    // 明度 +50 变亮
    const QRgb lighter = rawPixel(adjust::applyLightness(red, 50), 0, 0);
    check(qRed(lighter) >= qRed(rawPixel(red, 0, 0)),
          "明度 +50 不会把亮部压暗");

    // 半透明像素不能被当成暗部处理（与 1.2 同一个陷阱，换个入口再验一次）
    const QImage halfRed = makeSolid(8, 8, qRgba(128, 0, 0, 128));
    const QRgb halfGray = rawPixel(adjust::applySaturation(halfRed, -100), 0, 0);
    // 反预乘后 R=255 → 灰度 76 → 再乘回 alpha 128/255 得 38
    check(halfGray > 0 && qRed(halfGray) <= 40,
          "半透明像素的饱和度运算也在非预乘空间", "(期望约 38)");
}

// ---------------------------------------------------------------------------
//  4. ★ 曲线：单调性与不过冲 ★
// ---------------------------------------------------------------------------
static void testCurves()
{
    std::printf("\n[4] 曲线\n");

    // 默认曲线是恒等
    Curve identity;
    check(identity.isIdentity(), "默认曲线是恒等曲线");
    check(lutIsIdentity(identity.bake(true)), "恒等曲线烘焙出恒等表");

    // 两点直线：y = 0.5x 应恰好是折半（线性插值必须精确）
    {
        QList<CurvePoint> pts;
        pts.append(CurvePoint(0.0, 0.0));
        pts.append(CurvePoint(1.0, 0.5));
        const CurveLut lut = Curve(pts).bake(true);
        check(std::abs(int(lut[200]) - 100) <= 1, "两点曲线是精确直线");
    }

    // ── 单调性：这是"不过冲"的可操作判据 ──
    //    下面几组控制点都是会让 Catmull-Rom 过冲的形状：
    //    陡峭的 S 形、长平台后突然上升、密集点之间的大落差。
    {
        const qreal cases[][8] = {
            // 经典 S 形
            { 0.0, 0.0,   0.4, 0.10,  0.6, 0.90,  1.0, 1.0 },
            // 暗部平台 + 陡升（最容易在平台内部回勾）
            { 0.0, 0.0,   0.45, 0.05, 0.50, 0.95, 1.0, 1.0 },
            // 三段陡降后回升的极端形状
            { 0.0, 0.0,   0.2, 0.0,   0.8, 1.0,   1.0, 1.0 },
            // 中段几乎水平
            { 0.0, 0.0,   0.3, 0.5,   0.7, 0.52,  1.0, 1.0 },
        };

        for (int c = 0; c < 4; ++c) {
            QList<CurvePoint> pts;
            for (int i = 0; i < 4; ++i)
                pts.append(CurvePoint(cases[c][i * 2], cases[c][i * 2 + 1]));

            const CurveLut lut = Curve(pts).bake(true);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "第 %d 组 S 曲线全程单调（不过冲）", c + 1);
            check(isNonDecreasing(lut), buf);
        }
    }

    // ── "压到底"的平段：两个 y 相同的相邻点之间必须严格为 0 ──
    //    这正因为选了单调样条而不是 Catmull-Rom 才成立 ——
    //    后者的切线会从邻段借力，在两平点之间拱出一个包。
    {
        QList<CurvePoint> pts;
        pts.append(CurvePoint(0.0, 0.0));
        pts.append(CurvePoint(0.4, 0.0));
        pts.append(CurvePoint(0.6, 0.0));
        pts.append(CurvePoint(1.0, 1.0));
        const CurveLut lut = Curve(pts).bake(true);
        checkEq(int(lut[128]), 0, "两平点之间严格压到底（无过冲凸包）");
        check(isNonDecreasing(lut), "平段曲线整体单调");
    }

    // ── 直折线模式：段内线性 ──
    {
        QList<CurvePoint> pts;
        pts.append(CurvePoint(0.0, 0.0));
        pts.append(CurvePoint(0.5, 1.0));
        pts.append(CurvePoint(1.0, 1.0));
        const CurveLut lut = Curve(pts).bake(false);
        check(std::abs(int(lut[64]) - 128) <= 1 || int(lut[64]) > 100,
              "折线模式段内线性");
        checkEq(int(lut[200]), 255, "折线模式后面是平台");
    }

    // ── 控制点操作的约束 ──
    {
        Curve c;
        c.addPoint(CurvePoint(0.5, 0.5));
        checkEq(c.points().size(), 3, "新增一个中间控制点");

        // 同 x 再加点：替换 y，而不是变成 4 个点
        c.addPoint(CurvePoint(0.5, 0.8));
        checkEq(c.points().size(), 3, "同 x 的点只替换 y（曲线是单值函数）");

        // 端点 x 固定
        const int lastIdx = c.points().size() - 1;
        Curve c2 = c;
        c2.movePoint(0, CurvePoint(0.3, 0.3));
        check(c2.points().first().x < 1e-6, "起点的 x 不可移动");
        c2 = c;
        c2.movePoint(lastIdx, CurvePoint(0.3, 0.9));
        check(c2.points().last().x > 1.0 - 1e-6, "终点的 x 不可移动");

        // 中间点不能越过邻居
        Curve c3;
        c3.addPoint(CurvePoint(0.3, 0.3));
        c3.addPoint(CurvePoint(0.6, 0.6));
        c3.movePoint(1, CurvePoint(0.95, 0.3));
        check(c3.points()[1].x < 0.61, "中间点不能越过右邻居");
        check(c3.points()[1].x > 0.001, "中间点也不能越过左邻居");

        // 端点不可删
        check(!c.removePoint(0), "起点不可删除");
        check(!c.removePoint(c.points().size() - 1), "终点不可删除");
        check(c.removePoint(1), "中间点可以删除");
    }

    // ── CurvesSettings：四条曲线的合成 ──
    {
        CurvesSettings s;
        check(s.isIdentity(), "默认 CurvesSettings 是恒等");
        check(s.bakeLuts().isMonochrome(), "恒等设置产出单表（可走快路径）");

        // 只动红通道：三张表必须不再相同
        Curve redCurve;
        redCurve.addPoint(CurvePoint(0.5, 0.7));
        s.setCurve(CurveChannel::Red, redCurve);
        check(!s.isIdentity(), "改了红通道后不再是恒等");
        check(!s.bakeLuts().isMonochrome(), "各通道表不再相同");

        // 恒等设置作用到图上应逐位不变
        CurvesSettings ident;
        const QImage src = makeSolid(4, 4, qRgba(200, 100, 50, 255));
        check(rawPixel(applyCurves(src, ident), 0, 0) == rawPixel(src, 0, 0),
              "恒等曲线作用后图像逐位不变");
    }

    // ── 内置预设：每一个都必须"无回勾" ──
    {
        const QList<CurvePreset> presets = builtinPresets();
        check(presets.size() >= 4, "内置预设数量合理");

        bool allMonotone = true;
        bool allNamed = true;
        QString badName;
        for (int i = 0; i < presets.size(); ++i) {
            if (presets[i].name.isEmpty())
                allNamed = false;
            if (!isMonotone(Curve(presets[i].points).bake(true))) {
                allMonotone = false;
                badName = presets[i].name;
            }
        }
        check(allNamed, "所有预设都有名字");
        check(allMonotone, "所有内置预设烘焙后方向一致（不会带色带）");
        if (!allMonotone)
            std::printf("         出问题的预设：%s\n", qPrintable(badName));

        // "Invert" 预设必须是严格递减的 —— 它是唯一一个方向朝下的预设，
        // 如果哪天它被误改成递增，画面会从负片变成一片灰。
        {
            bool found = false;
            for (int i = 0; i < presets.size(); ++i) {
                if (presets[i].name != QObject::tr("Invert"))
                    continue;
                found = true;
                const CurveLut lut = Curve(presets[i].points).bake(true);
                checkEq(int(lut[0]), 255, "反相预设的黑场映射为白");
                checkEq(int(lut[255]), 0, "反相预设的白场映射为黑");
            }
            check(found, "内置预设里存在「Invert」");
        }

        // 预设名不能重复 —— 重名会让用户分不清选的是哪一个
        bool noDup = true;
        for (int i = 0; i < presets.size() && noDup; ++i)
            for (int j = i + 1; j < presets.size(); ++j)
                if (presets[i].name == presets[j].name)
                    noDup = false;
        check(noDup, "预设名不重复");
    }
}

// ---------------------------------------------------------------------------
//  5. 直方图
// ---------------------------------------------------------------------------
static void testHistogram()
{
    std::printf("\n[5] 直方图\n");

    // ── 5.1 计数守恒 ──
    {
        QImage img(10, 10, QImage::Format_ARGB32_Premultiplied);
        img.fill(qRgba(128, 128, 128, 255));
        const QVector<int> h = histogram::luminance(img);
        checkEq(h.size(), 256, "直方图有 256 个桶");

        int sum = 0;
        for (int v : h)
            sum += v;
        checkEq(sum, 100, "桶内计数之和 == 不透明像素数");

        check(h[128] == 100, "全部 128 灰落在第 128 桶");
    }

    // ── 5.2 ★ 必须转非预乘再统计 ★ ──
    //     50% 透明的纯白，画面上看是亮的；若在预乘空间统计，
    //     它会落进 128 桶 —— 用户会照着这个错的分布去调曲线。
    {
        const QImage halfWhite = makeSolid(10, 10, qRgba(128, 128, 128, 128));
        const QVector<int> h = histogram::luminance(halfWhite, true);
        checkEq(h[255], 100, "半透明白统计在亮度 255（先反预乘）");
        checkEq(h[128], 0, "半透明白没有落进 128 桶");
    }

    // ── 5.3 全透明像素默认跳过 ──
    {
        QImage img(4, 4, QImage::Format_ARGB32_Premultiplied);
        img.fill(qRgba(80, 80, 80, 255));
        img.setPixel(0, 0, qRgba(0, 0, 0, 0));

        int sumSkip = 0;
        const QVector<int> skip = histogram::luminance(img, true);
        for (int v : skip)
            sumSkip += v;
        checkEq(sumSkip, 15, "默认跳过全透明像素");

        int sumAll = 0;
        const QVector<int> all = histogram::luminance(img, false);
        for (int v : all)
            sumAll += v;
        checkEq(sumAll, 16, "传 false 时计入全透明像素");

        checkEq(all[0], 1, "全透明像素会堆在 0 桶（所以默认要跳过）");
    }

    // ── 5.4 归一化：对数缩放 ──
    {
        QVector<int> h(256, 0);
        h[10] = 1000000;   // 一根极高的柱
        h[200] = 5;        // 一根极矮的柱
        const QVector<qreal> n = histogram::normalized(h, true);
        checkEq(n.size(), 256, "归一化后仍是 256 项");
        check(std::abs(n[10] - 1.0) < 1e-9, "最大值归一化为 1");

        // ★ 对数缩放的意义 ★
        //   线性缩放下 5/1000000 会小到画不出来（0.000005）；
        //   对数缩放把它拉到 0.13 左右 —— 这才是"能看见"的量级。
        const QVector<qreal> lin = histogram::normalized(h, false);
        check(n[200] > 0.10, "对数缩放让极矮的柱仍可见");
        check(lin[200] < 0.001, "对照组：线性缩放下极矮的柱几乎不可见");
        check(n[200] > lin[200] * 100, "对数缩放把动态范围压缩了百倍以上");
    }

    // ── 5.5 单通道 ──
    {
        QImage img(2, 2, QImage::Format_ARGB32_Premultiplied);
        img.fill(qRgba(255, 0, 0, 255));
        checkEq(histogram::singleChannel(img, 0)[255], 4, "R 通道统计正确");
        checkEq(histogram::singleChannel(img, 1)[0], 4, "G 通道统计正确");
        checkEq(histogram::singleChannel(img, 2)[0], 4, "B 通道统计正确");
        // 越界通道按 R 处理而不是崩溃
        checkEq(histogram::singleChannel(img, 99)[255], 4, "越界通道回退到 R");
    }

    // ── 5.6 空图不能崩 ──
    {
        const QImage empty;
        const QVector<int> h = histogram::luminance(empty);
        checkEq(h.size(), 256, "空图返回的是全 0 的 256 项表");
        int sum = 0;
        for (int v : h)
            sum += v;
        checkEq(sum, 0, "空图计数全为 0");
    }
}

// ---------------------------------------------------------------------------
//  6. 端到端：一条"曲线调整 + 选区混合"的近似流程
// ---------------------------------------------------------------------------
static void testEndToEnd()
{
    std::printf("\n[6] 端到端\n");

    // 造一张含多个灰阶的图
    QImage img(256, 4, QImage::Format_ARGB32_Premultiplied);
    for (int x = 0; x < 256; ++x)
        for (int y = 0; y < 4; ++y)
            img.setPixel(x, y, qRgba(x, x, x, 255));

    // 一条"暗部压暗"的曲线：应满足 out(x) <= x，且端点不动
    Curve c;
    c.addPoint(CurvePoint(0.25, 0.15));
    c.addPoint(CurvePoint(0.75, 0.75));

    const QImage out = applyCurves(img, CurvesSettings());
    // 注意：这里用恒等设置，验证的是"应用管线不动画面"
    bool same = true;
    for (int x = 0; x < 256 && same; ++x)
        if (rawPixel(out, x, 0) != rawPixel(img, x, 0))
            same = false;
    check(same, "恒等设置下 256 级灰阶逐级不变");

    // 用刚建的曲线单跑一遍，验证压缩暗部
    CurvesSettings s;
    s.setCurve(CurveChannel::Rgb, c);
    const QImage adjusted = applyCurves(img, s);

    const int in64 = qRed(rawPixel(adjusted, 64, 0));
    const int in192 = qRed(rawPixel(adjusted, 192, 0));
    check(in64 < 64, "自定义曲线压暗了暗部");
    check(in192 > 180, "自定义曲线基本保留了亮部");
    checkEq(qRed(rawPixel(adjusted, 0, 0)), 0, "曲线不动黑场");
    checkEq(qRed(rawPixel(adjusted, 255, 0)), 255, "曲线不动白场");
}

// ---------------------------------------------------------------------------

int main()
{
    std::printf("================================================\n");
    std::printf("  ZlPhotoEdit 调整 / 曲线 / 直方图 离线自检\n");
    std::printf("================================================\n");

    testLutPremultiplied();
    testBasicAdjust();
    testColorAdjust();
    testCurves();
    testHistogram();
    testEndToEnd();

    std::printf("\n================================================\n");
    std::printf("  通过 %d 项，失败 %d 项\n", g_passed, g_failed);
    std::printf("================================================\n");

    return g_failed == 0 ? 0 : 1;
}
