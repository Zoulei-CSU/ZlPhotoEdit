#include "engine/Lut.h"

#include <QtMath>

namespace zl {

CurveLut identityLut()
{
    CurveLut lut{};
    for (int i = 0; i < 256; ++i)
        lut[size_t(i)] = quint8(i);
    return lut;
}

CurveLut composeLut(const CurveLut &inner, const CurveLut &outer)
{
    // 先穿过 inner，再把结果喂给 outer。
    // 例：inner 把 128 映射到 200，outer 把 200 映射到 180，
    //     则合成表在 128 处应得到 180。
    CurveLut result{};
    for (int i = 0; i < 256; ++i)
        result[size_t(i)] = outer[size_t(inner[size_t(i)])];
    return result;
}

// ===========================================================================
//  应用 LUT
// ===========================================================================

QImage applyLuts(const QImage &src, const ChannelLuts &luts)
{
    if (src.isNull())
        return QImage();

    // ── 第一步：统一成 32 位 ARGB 格式 ──
    // 需要保留 alpha 通道，所以不能转成 RGB32。
    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = src;
    if (src.format() != QImage::Format_ARGB32
        && src.format() != QImage::Format_ARGB32_Premultiplied) {
        work = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }

    // 输出也用预乘格式（图层位图的铁律），最后统一转换
    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    const int w = work.width();
    const int h = work.height();

    // 把 LUT 的数据指针取出来，避免在循环里通过 std::array 索引访问
    // （带上迭代器调试检查后会有额外开销）
    //
    // ★ 三表相同的"单表"优化，优化的是**表**，不是**查表次数** ★
    //
    // isMonochrome() 的含义是"三条曲线长得一样"，即
    //     lutR(x) == lutG(x) == lutB(x)   对任意 x
    // 它**不**意味着"像素的 R/G/B 三个分量相等"。这两件事毫无关系。
    //
    // 所以这里只做一件事：让 lutG/lutB 指向同一份 256 字节的表，
    // 使三个通道的查找命中同一片 cache。逐像素仍必须用**各自的输入值**
    // 去查三次表。
    //
    // 曾经这里写成 `r = g = b = lutR[ir]`（只用红通道的值查一次再广播），
    // 在灰度图上完全看不出问题，但任何彩色像素的 G/B 都会被红通道的结果
    // 覆盖 —— 表现为"调个亮度，彩色照片变成单色版"。而且因为
    // applyLutRgb 构造出的 ChannelLuts 天然是 monochrome，
    // 这个错误路径覆盖了亮度/对比度/RGB 主曲线的**全部**调用。
    //
    // 判断放在循环外，逐像素分支省掉。
    const bool mono = luts.isMonochrome();
    const quint8 *lutR = luts.r.data();
    const quint8 *lutG = mono ? lutR : luts.g.data();
    const quint8 *lutB = mono ? lutR : luts.b.data();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const int a = qAlpha(px);

            // 完全透明的像素：什么也看不到，不必运算，直接跳过。
            // 这是很实在的优化 —— 新建图层大部分区域是透明的。
            if (a == 0) {
                dstLine[x] = 0;
                continue;
            }

            int r, g, b;

            if (wasPremultiplied || work.format() == QImage::Format_ARGB32_Premultiplied) {
                // ── 预乘 → 非预乘 → 查表 → 回预乘 ──
                //
                // 这一步是本文件存在的全部理由。见头文件里的详细说明。
                // 如果不做这个还原，半透明像素会查到一个"被 alpha 压暗"的值，
                // 结果就是半透明区域颜色明显偏差。
                //
                // 还原公式：original = premultiplied * 255 / alpha
                const int r0 = qRed(px)   * 255 / a;
                const int g0 = qGreen(px) * 255 / a;
                const int b0 = qBlue(px)  * 255 / a;

                // 除以 alpha 后可能略微超过 255（预乘时的舍入误差），需要钳制
                const int ir = qBound(0, r0, 255);
                const int ig = qBound(0, g0, 255);
                const int ib = qBound(0, b0, 255);

                // 三个通道各查各的输入值。mono 时三个指针指向同一张表，
                // 所以这三行在 mono 与非 mono 下是**同一段代码** ——
                // 不必也不能写成"查一次红通道再广播"。
                r = lutR[ir];
                g = lutG[ig];
                b = lutB[ib];

                // 乘回 alpha 存进预乘格式。
                // 用 +127 做四舍五入，否则大量像素会系统性偏暗（截断误差）。
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            } else {
                // ── 非预乘格式：直接查表 ──
                // 同上：必须用各自的输入值查表
                r = lutR[qRed(px)];
                g = lutG[qGreen(px)];
                b = lutB[qBlue(px)];
            }

            dstLine[x] = qRgba(qBound(0, r, 255),
                               qBound(0, g, 255),
                               qBound(0, b, 255),
                               a);
        }
    }

    // 若调用方原本给的就是非预乘格式，这里转回去，保持"输入什么格式输出什么格式"
    // 的对称性。多数情况下调用方给的是预乘格式，不会走到这一步。
    if (!wasPremultiplied && src.format() == QImage::Format_ARGB32)
        return result.convertToFormat(QImage::Format_ARGB32);

    return result;
}

QImage applyLutRgb(const QImage &src, const CurveLut &lut)
{
    ChannelLuts luts;
    luts.r = lut;
    luts.g = lut;
    luts.b = lut;
    return applyLuts(src, luts);
}

} // namespace zl
