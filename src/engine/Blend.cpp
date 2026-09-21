#include "engine/Blend.h"

#include <QColor>
#include <QtMath>

namespace zl {
namespace blend {

// ===========================================================================
//  标量混合公式
// ===========================================================================
//  所有函数都在 [0,1] 归一化空间运算。使用 double 而不是 float，
//  因为批量运算中累积误差会体现为可见的色带。
//
//  文件内用 static 而非匿名 namespace，是为了让这些函数的符号在调试时
//  仍然可见（匿名 namespace 会给符号加随机后缀，调试器里很难看）。
// ===========================================================================

// ── 正常组 ──

qreal blendNormal(qreal /*base*/, qreal blend)
{
    return blend;
}

// ── 变暗组 ──

qreal blendDarken(qreal base, qreal blend)
{
    return qMin(base, blend);
}

qreal blendMultiply(qreal base, qreal blend)
{
    return base * blend;
}

qreal blendColorBurn(qreal base, qreal blend)
{
    // Photoshop 的公式：1 - (1-base)/blend
    // blend 为 0 时结果应为 0（而非除零），必须单独处理。
    if (blend <= 0.0)
        return 0.0;
    return 1.0 - qMin(1.0, (1.0 - base) / blend);
}

qreal blendLinearBurn(qreal base, qreal blend)
{
    return qMax(0.0, base + blend - 1.0);
}

// ── 变亮组 ──

qreal blendLighten(qreal base, qreal blend)
{
    return qMax(base, blend);
}

qreal blendScreen(qreal base, qreal blend)
{
    return base + blend - base * blend;
}

qreal blendColorDodge(qreal base, qreal blend)
{
    // 1 - base/blend… 不，正确公式是 base / (1 - blend)
    if (blend >= 1.0)
        return 1.0;
    return qMin(1.0, base / (1.0 - blend));
}

qreal blendLinearDodge(qreal base, qreal blend)
{
    return qMin(1.0, base + blend);
}

// ── 对比组 ──

qreal blendOverlay(qreal base, qreal blend)
{
    // 叠加 = 对调参数的强光。base 决定走"变暗"还是"变亮"分支。
    return base <= 0.5
               ? 2.0 * base * blend
               : 1.0 - 2.0 * (1.0 - base) * (1.0 - blend);
}

qreal blendHardLight(qreal base, qreal blend)
{
    // 强光 = 对调参数的叠加
    return blend <= 0.5
               ? 2.0 * base * blend
               : 1.0 - 2.0 * (1.0 - base) * (1.0 - blend);
}

qreal blendSoftLight(qreal base, qreal blend)
{
    // Photoshop 使用的 W3C 推荐公式，分为 blend <= 0.5 与 > 0.5 两支。
    // 这个公式的形态不是凭直觉能推出来的 —— 它是为了让软光的效果
    // 在极端值附近平滑收敛而专门设计的，请勿"简化"。
    if (blend <= 0.5) {
        return base - (1.0 - 2.0 * blend) * base * (1.0 - base);
    }

    // 第二支需要一个辅助函数 D(base)
    qreal d;
    if (base <= 0.25)
        d = ((16.0 * base - 12.0) * base + 4.0) * base;
    else
        d = qSqrt(base);

    return base + (2.0 * blend - 1.0) * (d - base);
}

qreal blendVividLight(qreal base, qreal blend)
{
    // 亮光：blend < 0.5 时做"颜色加深"，>= 0.5 时做"颜色减淡"，
    // 且 blend 需要先重映射到 [0,1] 的完整范围。
    if (blend <= 0.0)
        return base <= 0.0 ? 0.0 : 0.0;      // 加深到极限 = 全黑
    if (blend >= 1.0)
        return base >= 1.0 ? 1.0 : 1.0;      // 减淡到极限 = 全白

    if (blend < 0.5) {
        // 颜色加深，blend 映射为 2*blend
        const qreal b = 2.0 * blend;
        if (b <= 0.0)
            return 0.0;
        return 1.0 - qMin(1.0, (1.0 - base) / b);
    }

    // 颜色减淡，blend 映射为 2*(blend-0.5)
    const qreal b = 2.0 * (blend - 0.5);
    if (b >= 1.0)
        return 1.0;
    return qMin(1.0, base / (1.0 - b));
}

qreal blendLinearLight(qreal base, qreal blend)
{
    // 线性光 = 线性加深 / 线性减淡 的分支
    if (blend < 0.5)
        return qMax(0.0, base + 2.0 * blend - 1.0);
    return qMin(1.0, base + 2.0 * (blend - 0.5));
}

qreal blendPinLight(qreal base, qreal blend)
{
    // 点光：按 blend 与 0.5 的关系在变暗/变亮间硬切换
    if (blend < 0.5)
        return qMin(base, 2.0 * blend);
    return qMax(base, 2.0 * (blend - 0.5));
}

// ── 差值组 ──

qreal blendDifference(qreal base, qreal blend)
{
    return qAbs(base - blend);
}

qreal blendExclusion(qreal base, qreal blend)
{
    return base + blend - 2.0 * base * blend;
}

// ===========================================================================
//  分派
// ===========================================================================

qreal blendChannel(BlendMode mode, qreal base, qreal blend)
{
    switch (mode) {
    case BlendMode::Normal:
    case BlendMode::Dissolve:     // 溶解需要随机数，暂按正常处理
        return blendNormal(base, blend);

    case BlendMode::Darken:       return blendDarken(base, blend);
    case BlendMode::Multiply:     return blendMultiply(base, blend);
    case BlendMode::ColorBurn:    return blendColorBurn(base, blend);
    case BlendMode::LinearBurn:   return blendLinearBurn(base, blend);

    case BlendMode::Lighten:      return blendLighten(base, blend);
    case BlendMode::Screen:       return blendScreen(base, blend);
    case BlendMode::ColorDodge:   return blendColorDodge(base, blend);
    case BlendMode::LinearDodge:  return blendLinearDodge(base, blend);

    case BlendMode::Overlay:      return blendOverlay(base, blend);
    case BlendMode::SoftLight:    return blendSoftLight(base, blend);
    case BlendMode::HardLight:    return blendHardLight(base, blend);
    case BlendMode::VividLight:   return blendVividLight(base, blend);
    case BlendMode::LinearLight:  return blendLinearLight(base, blend);
    case BlendMode::PinLight:     return blendPinLight(base, blend);

    case BlendMode::Difference:   return blendDifference(base, blend);
    case BlendMode::Exclusion:    return blendExclusion(base, blend);

    // 分量模式在 compositeLayer 里单独处理，不会走到这里。
    // 万一走到了，退化为正常模式而不是抛异常 —— 渲染管线里宁可结果略错，
    // 也不能因为一个未处理的枚举值把整个程序搞崩。
    case BlendMode::Hue:
    case BlendMode::Saturation:
    case BlendMode::Color:
    case BlendMode::Luminosity:
    default:
        return blendNormal(base, blend);
    }
}

// ===========================================================================
//  分量模式辅助：RGB ↔ HSL
// ===========================================================================
//  色相/饱和度/颜色/明度这四种模式的定义是：
//    在 HSL 空间里，各取一个分量来自上层、其余来自下层。
//    色相   ：上层色相 + 下层饱和度、明度
//    饱和度 ：下层色相、明度 + 上层饱和度
//    颜色   ：上层色相、饱和度 + 下层明度
//    明度   ：下层色相、饱和度 + 上层明度
//  用 QColor 做转换虽然方便，但在逐像素循环里每次构造 QColor 太慢，
//  因此这里实现一套轻量的 HSL 转换（输入输出均为 [0,1] 归一化分量）。
// ===========================================================================

namespace {

struct Hsl
{
    qreal h;   // [0, 1)，1 等价于 0
    qreal s;   // [0, 1]
    qreal l;   // [0, 1]
};

// RGB → HSL。这是标准算法，网上有无数版本，但要注意：很多版本在
// 分母为 0 时不做保护，会在灰色像素（r==g==b）上产生 NaN。
// NaN 一旦进入后续运算会污染整行像素，表现为莫名其妙的黑块或白块。
Hsl rgbToHsl(qreal r, qreal g, qreal b)
{
    const qreal maxC = qMax(r, qMax(g, b));
    const qreal minC = qMin(r, qMin(g, b));
    const qreal l = (maxC + minC) * 0.5;
    const qreal delta = maxC - minC;

    Hsl result;
    result.l = l;

    // 灰色（无饱和度）：色相与饱和度都定义为 0，避免除零
    if (qFuzzyIsNull(delta)) {
        result.h = 0.0;
        result.s = 0.0;
        return result;
    }

    result.s = (l > 0.5) ? delta / (2.0 - maxC - minC) : delta / (maxC + minC);

    if (qFuzzyCompare(maxC, r))
        result.h = (g - b) / delta + (g < b ? 6.0 : 0.0);
    else if (qFuzzyCompare(maxC, g))
        result.h = (b - r) / delta + 2.0;
    else
        result.h = (r - g) / delta + 4.0;

    result.h /= 6.0;
    return result;
}

// HSL → RGB，标准算法
void hslToRgb(const Hsl &hsl, qreal &r, qreal &g, qreal &b)
{
    if (qFuzzyIsNull(hsl.s)) {
        // 无饱和度 = 灰度，三通道同值
        r = g = b = hsl.l;
        return;
    }

    const qreal q = (hsl.l < 0.5) ? hsl.l * (1.0 + hsl.s)
                                  : hsl.l + hsl.s - hsl.l * hsl.s;
    const qreal p = 2.0 * hsl.l - q;

    auto hueToRgb = [](qreal pp, qreal qq, qreal t) -> qreal {
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        if (t < 1.0 / 6.0) return pp + (qq - pp) * 6.0 * t;
        if (t < 1.0 / 2.0) return qq;
        if (t < 2.0 / 3.0) return pp + (qq - pp) * (2.0 / 3.0 - t) * 6.0;
        return pp;
    };

    r = hueToRgb(p, q, hsl.h + 1.0 / 3.0);
    g = hueToRgb(p, q, hsl.h);
    b = hueToRgb(p, q, hsl.h - 1.0 / 3.0);
}

// 判断是否为"分量模式"（需要走 HSL 路径）
bool isComponentMode(BlendMode mode)
{
    return mode == BlendMode::Hue
        || mode == BlendMode::Saturation
        || mode == BlendMode::Color
        || mode == BlendMode::Luminosity;
}

} // namespace

// ===========================================================================
//  批量合成
// ===========================================================================

QImage compositeLayer(const QImage &base, const QImage &blendImage,
                      BlendMode mode, qreal opacity)
{
    if (base.isNull())
        return base;
    if (blendImage.isNull() || opacity <= 0.0)
        return base.copy();

    // 输出始终是预乘格式。若输入不是（比如调用方忘了转），在这里补一次转换，
    // 而不是假设调用方做对了 —— 这个函数是渲染管线的心脏，不能有隐含假设。
    QImage result = (base.format() == QImage::Format_ARGB32_Premultiplied)
                        ? base.copy()
                        : base.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage upper = (blendImage.format() == QImage::Format_ARGB32_Premultiplied)
                       ? blendImage
                       : blendImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    const int w = result.width();
    const int h = result.height();

    // 不透明度钳制到合法范围。调用方可能传 1.5 或负数（例如从滑块取值时的
    // 浮点误差），越界值会让整个画面变成白色或黑色。
    const qreal layerAlpha = qBound(0.0, opacity, 1.0);

    const bool componentMode = isComponentMode(mode);

    // 逐行处理，每行一次 scanLine 调用，比逐像素调用 pixel()/setPixel() 快
    // 一到两个数量级（后者每次都做格式转换和边界检查）。
    for (int y = 0; y < h; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        // 上层图像尺寸可能小于下层，超出范围的行直接跳过（视为全透明）
        if (y >= upper.height())
            continue;

        const QRgb *srcLine = reinterpret_cast<const QRgb *>(upper.constScanLine(y));
        const int copyW = qMin(w, upper.width());

        for (int x = 0; x < copyW; ++x) {
            const QRgb src = srcLine[x];

            // 提取上层分量。注意这是在预乘空间 ——
            // 预乘像素的 RGB 已经乘过 alpha，要还原"原始颜色"必须先除回去，
            // 否则半透明图层的颜色会被错误地压暗。
            const qreal sa = qAlpha(src) / 255.0;
            if (sa <= 0.0)
                continue;   // 完全透明，对结果无影响

            // 有效 alpha = 图层自身的 alpha × 图层不透明度
            const qreal effAlpha = sa * layerAlpha;
            if (effAlpha <= 0.0)
                continue;

            // 还原上层的非预乘颜色
            const qreal sr = qAlpha(src) > 0 ? qreal(qRed(src))   / qAlpha(src) : 0.0;
            const qreal sg = qAlpha(src) > 0 ? qreal(qGreen(src)) / qAlpha(src) : 0.0;
            const qreal sb = qAlpha(src) > 0 ? qreal(qBlue(src))  / qAlpha(src) : 0.0;

            // 还原下层的非预乘颜色
            const QRgb dst = dstLine[x];
            const qreal da = qAlpha(dst) / 255.0;
            const qreal dr = qAlpha(dst) > 0 ? qreal(qRed(dst))   / qAlpha(dst) : 0.0;
            const qreal dg = qAlpha(dst) > 0 ? qreal(qGreen(dst)) / qAlpha(dst) : 0.0;
            const qreal db = qAlpha(dst) > 0 ? qreal(qBlue(dst))  / qAlpha(dst) : 0.0;

            qreal br = dr, bg = dg, bb = db;

            if (componentMode) {
                // ── 分量模式：在 HSL 空间里各取所需 ──
                const Hsl baseHsl = rgbToHsl(dr, dg, db);
                const Hsl srcHsl  = rgbToHsl(sr, sg, sb);
                Hsl outHsl = baseHsl;

                switch (mode) {
                case BlendMode::Hue:
                    // 色相取上层，饱和度与明度取下层
                    outHsl.h = srcHsl.h;
                    break;
                case BlendMode::Saturation:
                    outHsl.s = srcHsl.s;
                    break;
                case BlendMode::Color:
                    // 颜色 = 上层色相 + 上层饱和度 + 下层明度
                    outHsl.h = srcHsl.h;
                    outHsl.s = srcHsl.s;
                    break;
                case BlendMode::Luminosity:
                    outHsl.l = srcHsl.l;
                    break;
                default:
                    break;
                }

                hslToRgb(outHsl, br, bg, bb);
            } else {
                // ── 通路模式：逐通道套用公式 ──
                br = blendChannel(mode, dr, sr);
                bg = blendChannel(mode, dg, sg);
                bb = blendChannel(mode, db, sb);
            }

            // ── 按 alpha 做普通合成（source-over）──
            // 结果 alpha：da + ea*(1-da)
            const qreal outA = da + effAlpha * (1.0 - da);
            if (outA <= 0.0) {
                dstLine[x] = 0;
                continue;
            }

            // 颜色的 alpha 加权混合
            const qreal mixR = (br * effAlpha + dr * da * (1.0 - effAlpha)) ;
            const qreal mixG = (bg * effAlpha + dg * da * (1.0 - effAlpha)) ;
            const qreal mixB = (bb * effAlpha + db * da * (1.0 - effAlpha)) ;

            // ── 写回：★ 这三行就是预乘结果，不要再乘一次 alpha ★ ──
            //
            // 推导（非预乘颜色用大写、预乘用小写，均归一化到 0-1）：
            //     source-over 的定义是
            //         Co·Ao = Sr·Sa + Dr·Da·(1-Sa)
            //     左边 Co·Ao 就是"结果的预乘颜色"。
            //     而上面那三行的右侧恰好是
            //         br·ea + (dr·da)·(1-ea)   ← dr·da 正是下层的预乘颜色
            //     所以 mixR/mixG/mixB **已经是预乘量**，直接 ×255 即可。
            //
            // 原先这里写的是 `mixR * finalA`，等于把 alpha 乘了第二次，
            // 也就是"二次预乘"。后果：只要结果的 alpha 不是 255，
            // 颜色就会被压暗到原来的 outA 倍 —— 而且**完全不报错**。
            //
            // 实测（探针，1 层 4×4）：
            //   图层位图  raw a=128 r=128   （= 反预乘后的纯红、alpha 128）
            //   合成之前  raw a=128 r=64    ← 红被减半了
            // 也就是"半透明的纯红"在画面上显示成"半透明的暗红"。
            // 目标不透明时 outA=255，这一步退化成乘 1，所以画在白底上
            // 看不出来 —— 只有透明画布（新建文档的默认底色）与柔边笔迹
            // 才会露出来，这也是它能潜伏这么久的原因。
            const int finalA = int(qRound(outA * 255.0));
            const int finalR = int(qRound(qBound(0.0, mixR, 1.0) * 255.0));
            const int finalG = int(qRound(qBound(0.0, mixG, 1.0) * 255.0));
            const int finalB = int(qRound(qBound(0.0, mixB, 1.0) * 255.0));

            dstLine[x] = qRgba(qBound(0, finalR, 255),
                               qBound(0, finalG, 255),
                               qBound(0, finalB, 255),
                               qBound(0, finalA, 255));
        }
    }

    return result;
}

QImage compositeLayers(const QList<QImage> &images,
                       const QList<BlendMode> &modes,
                       const QList<qreal> &opacities,
                       const QList<bool> &visible,
                       const QSize &bounds,
                       const QColor &background)
{
    if (!bounds.isValid() || bounds.isEmpty())
        return QImage();

    // 初始画布：填充指定底色（通常透明，但"拼合图像"时可能是白色）
    QImage canvas(bounds, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(background);

    // 从底到顶依次合成。QList 索引 0 是最底部，与 LayerList 的约定一致。
    const int count = qMin(qMin(images.size(), modes.size()),
                           qMin(opacities.size(), visible.size()));

    for (int i = 0; i < count; ++i) {
        // 不可见的图层直接跳过。这是最常见的优化 —— 图层多但大部分隐藏时，
        // 跳过它们能省下大量时间。
        if (!visible.at(i))
            continue;

        const QImage &img = images.at(i);
        if (img.isNull())
            continue;

        canvas = compositeLayer(canvas, img, modes.at(i), opacities.at(i));
    }

    return canvas;
}

} // namespace blend
} // namespace zl
