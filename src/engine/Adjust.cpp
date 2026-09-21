#include "engine/Adjust.h"

#include <QtMath>
#include <QColor>
#include <cmath>

namespace zl {
namespace adjust {

// ===========================================================================
//  亮度
// ===========================================================================

CurveLut brightnessLut(int brightness)
{
    // 参数钳制。界面上滑块范围是 [-100,100]，但代码不能依赖界面的约束 ——
    // 撤销栈恢复、脚本调用等路径都可能传入越界值。
    const int b = qBound(-100, brightness, 100);

    // 恒等时直接返回恒等表，省掉一次逐项计算
    if (b == 0)
        return identityLut();

    // 把 -100~100 映射到 -0.5~0.5 的归一化偏移量。
    // 上限 0.5 是刻意的：若映射到 ±1.0，滑块拉到两端就完全压死成纯黑/纯白，
    // 用户会觉得"滑到这里图就没了"，体验很差。0.5 保证即使拉到极限
    // 图像仍是可辨识的。
    const qreal offset = b / 200.0;

    return bakeLutFromFn([offset](qreal in) -> qreal {
        // 在接近 0 和 1 的地方压缩变化率，避免大面积"黑死"/"白死"。
        //
        // 用一个简单的软钳制：把偏移量按 (1 - |2*in - 1|) 的权重衰减，
        // 中灰处权重最大（全量应用偏移），两端权重趋近 0。
        const qreal weight = 1.0 - qAbs(2.0 * in - 1.0);

        // weight 在最暗/最亮处为 0，这里给一个下限 0.5，
        // 否则纯黑区域完全不受影响，会显得"提亮提不动"。
        const qreal effective = offset * (0.5 + 0.5 * weight);

        return in + effective;
    });
}

// ===========================================================================
//  对比度
// ===========================================================================

CurveLut contrastLut(int contrast)
{
    const int c = qBound(-100, contrast, 100);
    if (c == 0)
        return identityLut();

    // 把 [-100, 100] 映射为缩放系数。
    //
    // 用指数映射而不是线性映射，原因：
    //   线性会给正负方向不对称的感受 —— 用户觉得"+50"和"-50"的效果
    //   应该是对称的，但线性缩放下 +100 是 ×2、-100 是 ×0（完全崩掉）。
    //   指数映射下 +100 得 ×3、-100 得 ×1/3，视觉上对称得多。
    const qreal factor = qPow(2.0, c / 100.0);

    return bakeLutFromFn([factor](qreal in) -> qreal {
        // 围绕中灰 0.5 缩放
        return (in - 0.5) * factor + 0.5;
    });
}

CurveLut brightnessContrastLut(int brightness, int contrast)
{
    // 顺序：先亮度，后对比度。
    // 这个顺序必须与界面预览、撤销恢复路径完全一致 —— 否则会出现
    // "确定后结果与预览不同"的 bug，且极难排查（因为两步单独看都对）。
    const CurveLut bLut = brightnessLut(brightness);
    const CurveLut cLut = contrastLut(contrast);

    // 合成：先穿过亮度表，再穿过对比度表
    return composeLut(bLut, cLut);
}

// ===========================================================================
//  三通道相关的运算
// ===========================================================================

namespace {

// 把图像统一成预乘 ARGB32，并记录原格式以便最后转回
inline QImage toPremultiplied(const QImage &src, bool &wasPremultiplied)
{
    wasPremultiplied = (src.format() == QImage::Format_ARGB32_Premultiplied);
    if (wasPremultiplied)
        return src;
    return src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

inline QImage restoreFormat(const QImage &img, const QImage &original,
                            bool wasPremultiplied)
{
    if (!wasPremultiplied && original.format() == QImage::Format_ARGB32)
        return img.convertToFormat(QImage::Format_ARGB32);
    return img;
}

// 亮度权重：人眼对绿色最敏感，对蓝色最不敏感。
// 这组系数来自 Rec.601（标清电视标准），也是 Photoshop 的"去色"所用系数。
constexpr qreal kLumaR = 0.299;
constexpr qreal kLumaG = 0.587;
constexpr qreal kLumaB = 0.114;

} // namespace

QImage applySaturation(const QImage &src, int saturation)
{
    if (src.isNull())
        return QImage();

    const int s = qBound(-100, saturation, 100);
    if (s == 0)
        return src.copy();

    // 把 [-100, 100] 映射为插值系数：
    //   s = -100 → factor = 0    （完全灰）
    //   s =    0 → factor = 1    （不变）
    //   s =  100 → factor = 2    （饱和度加倍）
    //
    // 注意：正向最多只能到 2 倍。如果允许更大，颜色会迅速超出色域被钳制，
    // 结果是"越加越糊"，用户会以为程序有问题。
    const qreal factor = (s < 0) ? (1.0 + s / 100.0)   // -100 → 0
                                 : (1.0 + s / 100.0);  // 100 → 2

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    const int w = work.width();
    const int h = work.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int a = qAlpha(px);

            if (a == 0) {
                dstLine[x] = 0;
                continue;
            }

            // 预乘 → 非预乘（与 Lut.cpp 同理，必须先还原真实颜色）
            const int r0 = qBound(0, qRed(px)   * 255 / a, 255);
            const int g0 = qBound(0, qGreen(px) * 255 / a, 255);
            const int b0 = qBound(0, qBlue(px)  * 255 / a, 255);

            // 按人眼亮度权重算灰度值。
            // 用浮点累加再取整，避免三次整数除法引入的误差。
            const qreal gray = kLumaR * r0 + kLumaG * g0 + kLumaB * b0;

            // 在原色与灰度之间线性插值
            const int r = int(qRound(gray + (r0 - gray) * factor));
            const int g = int(qRound(gray + (g0 - gray) * factor));
            const int b = int(qRound(gray + (b0 - gray) * factor));

            // 回预乘
            dstLine[x] = qRgba(qBound(0, r * a + 127, 255 * a) / 255,
                               qBound(0, g * a + 127, 255 * a) / 255,
                               qBound(0, b * a + 127, 255 * a) / 255,
                               a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

QImage applyHue(const QImage &src, int hueShift)
{
    if (src.isNull())
        return QImage();

    // 归一化到 [-180, 180]
    int shift = hueShift % 360;
    if (shift > 180)  shift -= 360;
    if (shift < -180) shift += 360;

    if (shift == 0)
        return src.copy();

    // ── 用旋转矩阵实现色相旋转 ──
    //
    // 色相旋转在几何上等价于"在 RGB 空间中绕灰度轴旋转"。绕轴旋转可以写成
    // 一个 3×3 矩阵，避免了逐像素做 RGB↔HSL 往返（后者涉及大量分支，
    // 慢得多，且容易在灰色像素上产生 NaN）。
    //
    // 矩阵来自绕 (1,1,1)/√3 轴旋转 θ 角的 Rodrigues 公式的展开形式。
    const qreal rad = qDegreesToRadians(qreal(shift));
    const qreal cosA = qCos(rad);
    const qreal sinA = qSin(rad);

    // 1/3 与 √3/3 的常用组合
    const qreal oneThird = 1.0 / 3.0;
    const qreal sqrtOneThird = qSqrt(oneThird);

    // 余弦项的权重分配
    const qreal m00 = cosA + (1.0 - cosA) * oneThird;
    const qreal m01 = oneThird * (1.0 - cosA) - sqrtOneThird * sinA;
    const qreal m02 = oneThird * (1.0 - cosA) + sqrtOneThird * sinA;
    const qreal m10 = m02;
    const qreal m11 = m00;
    const qreal m12 = m01;
    const qreal m20 = m01;
    const qreal m21 = m02;
    const qreal m22 = m00;

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    const int w = work.width();
    const int h = work.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int a = qAlpha(px);

            if (a == 0) {
                dstLine[x] = 0;
                continue;
            }

            const qreal r0 = qBound(0, qRed(px)   * 255 / a, 255);
            const qreal g0 = qBound(0, qGreen(px) * 255 / a, 255);
            const qreal b0 = qBound(0, qBlue(px)  * 255 / a, 255);

            // 矩阵乘
            const int r = int(qRound(m00 * r0 + m01 * g0 + m02 * b0));
            const int g = int(qRound(m10 * r0 + m11 * g0 + m12 * b0));
            const int b = int(qRound(m20 * r0 + m21 * g0 + m22 * b0));

            dstLine[x] = qRgba(qBound(0, r, 255) * a / 255,
                               qBound(0, g, 255) * a / 255,
                               qBound(0, b, 255) * a / 255,
                               a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

QImage applyLightness(const QImage &src, int lightness)
{
    if (src.isNull())
        return QImage();

    const int l = qBound(-100, lightness, 100);
    if (l == 0)
        return src.copy();

    // 与"亮度"用加法不同，明度用**乘法**：
    //   l > 0：out = in * (1 + l/100 * (1 - in))   —— 越亮的地方提升越少，
    //                                              避免高光直接过曝成纯白
    //   l < 0：out = in * (1 + l/100)              —— 简单压暗
    //
    // 正向的公式做了软限制（乘以 (1-in)），这样拉到 +100 时
    // 高光只是趋近 255 而不会硬生生截断，保留层次。
    const qreal amount = l / 100.0;

    // 用 LUT 实现：这是纯逐通道的映射，不必三通道相关处理
    const CurveLut lut = bakeLutFromFn([amount](qreal in) -> qreal {
        if (amount >= 0.0)
            return in + amount * in * (1.0 - in);
        return in * (1.0 + amount);
    });

    return applyLutRgb(src, lut);
}

QImage applyHueSaturationLightness(const QImage &src,
                                   int hueShift, int saturation, int lightness)
{
    if (src.isNull())
        return QImage();

    // 三个都为零时快速返回。
    // 这条快速路径有实际意义：对话框打开后用户没动任何滑块就点确定。
    if (hueShift == 0 && saturation == 0 && lightness == 0)
        return src.copy();

    // 顺序：色相 → 饱和度 → 明度。
    //
    // 为什么是这个顺序（与 Photoshop 一致）：
    //   先转色相，此时饱和度与明度都还没变，旋转结果可预测；
    //   再调饱和度，基于新色相调整色彩浓度；
    //   最后调明度，作为整体的最后一道调整。
    // 若把明度放在最前，饱和度的效果会随明度变化而变，
    // 用户拖饱和度滑块时会觉得"效果时强时弱"，体验很怪。
    QImage result = src;
    if (hueShift != 0)
        result = applyHue(result, hueShift);
    if (saturation != 0)
        result = applySaturation(result, saturation);
    if (lightness != 0)
        result = applyLightness(result, lightness);

    return result;
}

// ===========================================================================
//  色阶
// ===========================================================================

CurveLut levelsLut(const LevelsParams &params)
{
    // ── 参数校验 ──
    // 输入黑场必须小于白场。若用户把两者拉到交叉（界面上的滑块允许这种情况），
    // 会导致除零或产生负数的缩放系数，结果是整张图变成纯黑或纯白。
    // 这里做一次交换而不是报错 —— 交换的行为更符合"用户其实想要反转"的直觉。
    int inBlack = qBound(0, params.inBlack, 255);
    int inWhite = qBound(0, params.inWhite, 255);
    if (inWhite <= inBlack) {
        std::swap(inBlack, inWhite);
        // 仍然相等（都是同一个值）时，强制留出最小范围
        if (inWhite <= inBlack)
            inWhite = qMin(255, inBlack + 1);
    }

    const int outBlack = qBound(0, params.outBlack, 255);
    const int outWhite = qBound(0, params.outWhite, 255);

    // gamma 必须为正。0 或负数会让 qPow 产生 inf/NaN。
    const qreal gamma = qBound(0.01, params.gamma, 100.0);

    const qreal inRange = qreal(inWhite - inBlack);
    const qreal outRange = qreal(outWhite - outBlack);

    return bakeLutFromFn([inBlack, inRange, outBlack, outRange, gamma](qreal in) -> qreal {
        // 归一化输入到 [0,1]
        const qreal normalized = (in * 255.0 - inBlack) / inRange;
        const qreal clamped = qBound(0.0, normalized, 1.0);

        // gamma 校正：gamma > 1 变亮（中间调上移）
        const qreal gammaCorrected = qPow(clamped, 1.0 / gamma);

        // 映射到输出范围
        return (outBlack + gammaCorrected * outRange) / 255.0;
    });
}

// ===========================================================================
//  自动色阶
// ===========================================================================

LevelsParams computeAutoLevels(const QImage &src, qreal clipPercent)
{
    LevelsParams params;    // 默认值即"不变"

    if (src.isNull())
        return params;

    // ── 统计直方图 ──
    // 用亮度直方图而不是逐通道统计：
    //   逐通道独立拉伸会导致色彩偏移（整体色偏），
    //   基于亮度拉伸能保持原有色调关系，效果更自然（也就是 Photoshop 的
    //   "自动对比度"行为；"自动色阶"是逐通道的，这里选了前者的效果）。
    int histogram[256] = { 0 };
    quint64 total = 0;

    const int w = src.width();
    const int h = src.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];
            // 忽略完全透明的像素 —— 它们不参与成像，计入统计会干扰黑白场判断
            if (qAlpha(px) == 0)
                continue;

            // 计算感知亮度。用与 applySaturation 相同的系数。
            const qreal a = qAlpha(px) / 255.0;
            const int r = int(qRed(px)   / a);
            const int g = int(qGreen(px) / a);
            const int b = int(qBlue(px)  / a);

            const int luma = qBound(0,
                int(qRound(kLumaR * r + kLumaG * g + kLumaB * b)), 255);

            ++histogram[luma];
            ++total;
        }
    }

    if (total == 0)
        return params;

    // ── 按分位数确定黑白场 ──
    //
    // clipPercent 表示"两端各截掉多少比例的像素"。
    // 默认 0.1%：图像里常有极少数纯黑或纯白的噪点，如果按最小/最大值
    // 来定黑白场，一个噪点就能让整张图几乎不变（拉伸范围被噪点撑满）。
    const qreal clip = qBound(0.0, clipPercent, 5.0);
    const quint64 clipCount = quint64(total * clip / 100.0);

    int black = 0;
    quint64 accumulated = 0;
    for (int i = 0; i < 256; ++i) {
        accumulated += quint64(histogram[i]);
        if (accumulated > clipCount) {
            black = i;
            break;
        }
    }

    int white = 255;
    accumulated = 0;
    for (int i = 255; i >= 0; --i) {
        accumulated += quint64(histogram[i]);
        if (accumulated > clipCount) {
            white = i;
            break;
        }
    }

    // 安全兜底：极端情况下（例如纯色图）黑白场会重合，此时不做调整
    if (white <= black + 1)
        return params;

    params.inBlack = black;
    params.inWhite = white;
    params.gamma = 1.0;
    params.outBlack = 0;
    params.outWhite = 255;

    return params;
}

} // namespace adjust
} // namespace zl
