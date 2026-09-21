#include "engine/Resample.h"

#include <QtMath>
#include <QVector>
#include <cmath>

namespace zl {
namespace resample {

// ===========================================================================
//  核函数分派
// ===========================================================================

namespace {

// 按算法返回核函数的值。用 qreal 计算，保证精度。
qreal kernelValue(ResampleMethod method, qreal x)
{
    switch (method) {
    case ResampleMethod::Nearest:
        // 邻近取样实际不使用核函数加权，这里返回 1 只是占位
        return (qAbs(x) < 0.5) ? 1.0 : 0.0;
    case ResampleMethod::Bilinear:
        // 三角形核：W(x) = max(0, 1 - |x|)
        return qMax(0.0, 1.0 - qAbs(x));
    case ResampleMethod::Bicubic:
        return cubicKernel(x, -0.5);
    case ResampleMethod::Lanczos:
        return lanczosKernel(x, 3.0);
    }
    return cubicKernel(x, -0.5);
}

// 采样半径：决定每个输出像素需要参与运算的源像素范围
int radiusFor(ResampleMethod method)
{
    switch (method) {
    case ResampleMethod::Nearest:  return 0;
    case ResampleMethod::Bilinear: return 1;
    case ResampleMethod::Bicubic:  return 2;
    case ResampleMethod::Lanczos:  return 3;
    }
    return 2;
}

// 坐标钳制（clamp）。
// 这是边界处理的关键：越界时重复边缘像素，而不是当成透明。
// 若当成透明，图像四边会出现一圈暗边 —— 因为边缘像素与"透明黑"做了插值。
inline int clampIndex(int i, int size)
{
    if (i < 0)      return 0;
    if (i >= size)  return size - 1;
    return i;
}

} // namespace

int kernelRadius(ResampleMethod method)
{
    return radiusFor(method);
}

// ===========================================================================
//  构建重采样表
// ===========================================================================

ResampleTable buildTable(int srcSize, int dstSize, ResampleMethod method)
{
    ResampleTable table;
    table.srcSize = srcSize;
    table.dstSize = dstSize;

    if (srcSize <= 0 || dstSize <= 0)
        return table;

    // 邻近取样不需要表，直接按比例取整即可
    if (method == ResampleMethod::Nearest) {
        table.support = 1;
        table.start.resize(dstSize);
        table.weights.resize(dstSize);
        for (int i = 0; i < dstSize; ++i) {
            // 输出像素中心映射回源坐标
            const qreal srcPos = (i + 0.5) * qreal(srcSize) / qreal(dstSize);
            table.start[i] = clampIndex(int(std::floor(srcPos)), srcSize);
            table.weights[i] = 1.0;
        }
        return table;
    }

    const int radius = radiusFor(method);
    const int support = radius * 2;

    // ── 缩小时的精度补偿 ──
    //
    // 缩小（dstSize < srcSize）时，输出像素对应源图上的一大块区域，
    // 只用 4 个采样点会漏掉大量信息，产生锯齿（专业术语叫"欠采样"）。
    // 正确做法是按缩放比放大采样半径，让采样范围覆盖住对应的源区域，
    // 效果上相当于先做了一次低通滤波。
    //
    // 这个处理是"缩小照片看起来干净"和"缩小照片满是摩尔纹"的分水岭。
    const qreal scale = qreal(dstSize) / qreal(srcSize);
    const qreal filterScale = (scale < 1.0) ? 1.0 / scale : 1.0;

    // 实际参与运算的采样点数。缩小时会变多。
    const int actualSupport = int(std::ceil(support * filterScale));
    table.support = actualSupport;
    table.start.resize(dstSize);
    table.weights.resize(dstSize * actualSupport);

    for (int i = 0; i < dstSize; ++i) {
        // 输出像素中心在源图上的位置
        const qreal center = (i + 0.5) / scale - 0.5;

        // 采样范围的起始索引
        const int left = int(std::floor(center)) - actualSupport / 2 + 1;

        qreal weightSum = 0.0;

        // 第一遍：算权重
        for (int k = 0; k < actualSupport; ++k) {
            const int srcIndex = left + k;

            // 距离按 filterScale 缩放，这样缩小时核函数被"拉宽"
            const qreal dist = (srcIndex - center) / filterScale;
            const qreal w = kernelValue(method, dist);

            table.weights[i * actualSupport + k] = w;
            weightSum += w;
        }

        // ── 关键：权重归一化 ──
        //
        // 如果跳过这一步，图像整体亮度会发生偏移，而且偏移量随缩放比例变化。
        // 原因：边界附近以及 filterScale > 1 时，权重之和天然不等于 1。
        //
        // 归一化后保证 Σw == 1，即"常量图像缩放后仍是同一个常量" ——
        // 这是重采样正确性的基本要求。
        if (qAbs(weightSum) > 1e-12) {
            const qreal invSum = 1.0 / weightSum;
            for (int k = 0; k < actualSupport; ++k)
                table.weights[i * actualSupport + k] *= invSum;
        } else {
            // 权重全为 0 的极端情况（理论上不会发生）：退化为取最近邻，
            // 保证输出不会是全黑的。
            for (int k = 0; k < actualSupport; ++k)
                table.weights[i * actualSupport + k] = 0.0;
            table.weights[i * actualSupport + actualSupport / 2] = 1.0;
        }

        table.start[i] = left;
    }

    return table;
}

// ===========================================================================
//  主入口
// ===========================================================================

QImage scale(const QImage &src, const QSize &targetSize, ResampleMethod method)
{
    if (src.isNull() || !targetSize.isValid() || targetSize.isEmpty())
        return QImage();

    const int srcW = src.width();
    const int srcH = src.height();
    const int dstW = targetSize.width();
    const int dstH = targetSize.height();

    // 尺寸没变，直接返回副本。
    // 注意不能返回 src 本身 —— 调用方可能会修改返回值，而 QImage 是隐式
    // 共享的，直接返回会让两者共享同一份像素，修改一个另一个也变。
    if (srcW == dstW && srcH == dstH)
        return src.copy();

    // ── 邻近取样走 Qt 的实现 ──
    // Qt 的 FastTransformation 就是邻近，没有必要自己重写一遍。
    // 而且像素画的放大场景对性能敏感，用 Qt 的优化实现更好。
    if (method == ResampleMethod::Nearest) {
        return src.scaled(targetSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    }

    // ── 统一到预乘 ARGB32 ──
    // 重采样的加权求和在预乘空间做才是正确的（见头文件说明）。
    // 同时统一格式能让下面的逐像素代码免去格式判断。
    QImage work = src;
    if (work.format() != QImage::Format_ARGB32_Premultiplied)
        work = work.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // ── 构建重采样表 ──
    const ResampleTable tableX = buildTable(srcW, dstW, method);
    const ResampleTable tableY = buildTable(srcH, dstH, method);

    if (tableX.support <= 0 || tableY.support <= 0)
        return src.copy();

    // ── 第一遍：水平方向 ──
    //
    // 中间结果用 float 存储三通道 + alpha。
    // 为什么不用 8 位整数：两次卷积的舍入误差会累积，大比例缩小时
    // 在 8 位缓冲上会出现可见的色阶断层（原本平滑的渐变变成一条条带）。
    // float 缓冲的内存是 4 倍，但这是画质与内存的合理交换。
    // ★ 用 QVector：需要预分配固定长度并按下标随机访问 ★
    QVector<float> midBuf(size_t(srcH) * size_t(dstW) * 4, 0.0f);

    for (int y = 0; y < srcH; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        float *midLine = midBuf.data() + size_t(y) * size_t(dstW) * 4;

        for (int x = 0; x < dstW; ++x) {
            const int startX = tableX.start.at(x);
            const int support = tableX.support;

            float accR = 0.0f, accG = 0.0f, accB = 0.0f, accA = 0.0f;

            // 内层循环是 4 次乘加（缩小时更多，但通常仍是个位数）。
            // 用局部指针和预取权重减少间接寻址。
            for (int k = 0; k < support; ++k) {
                const int sx = clampIndex(startX + k, srcW);
                const qreal w = tableX.weight(x, k);
                const QRgb px = srcLine[sx];

                accA += float(w) * float(qAlpha(px));
                accR += float(w) * float(qRed(px));
                accG += float(w) * float(qGreen(px));
                accB += float(w) * float(qBlue(px));
            }

            float *out = midLine + size_t(x) * 4;
            out[0] = accR;
            out[1] = accG;
            out[2] = accB;
            out[3] = accA;
        }
    }

    // ── 第二遍：垂直方向 ──
    QImage result(dstW, dstH, QImage::Format_ARGB32_Premultiplied);

    for (int y = 0; y < dstH; ++y) {
        const int startY = tableY.start.at(y);
        const int support = tableY.support;

        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < dstW; ++x) {
            float accR = 0.0f, accG = 0.0f, accB = 0.0f, accA = 0.0f;

            for (int k = 0; k < support; ++k) {
                const int sy = clampIndex(startY + k, srcH);
                const qreal w = tableY.weight(y, k);

                // 从中间缓冲取水平方向已经处理过的结果
                const float *mid = midBuf.constData()
                                   + (size_t(sy) * size_t(dstW) + size_t(x)) * 4;

                accR += float(w) * mid[0];
                accG += float(w) * mid[1];
                accB += float(w) * mid[2];
                accA += float(w) * mid[3];
            }

            // 四舍五入到整数，并钳制。
            // 因为是在预乘空间，需要保证 R/G/B <= A（预乘格式的约束），
            // 否则某些渲染路径会出现过亮的杂点。
            const int a = qBound(0, int(accA + 0.5f), 255);
            const int r = qBound(0, int(accR + 0.5f), a);
            const int g = qBound(0, int(accG + 0.5f), a);
            const int b = qBound(0, int(accB + 0.5f), a);

            dstLine[x] = qRgba(r, g, b, a);
        }
    }

    // 若原图不是预乘格式，转回去，保持输入输出格式对称
    if (src.format() != QImage::Format_ARGB32_Premultiplied)
        return result.convertToFormat(src.format());

    return result;
}

QImage scale(const QImage &src, qreal factor, ResampleMethod method)
{
    if (src.isNull() || factor <= 0.0)
        return QImage();

    const int w = qMax(1, int(qRound(src.width()  * factor)));
    const int h = qMax(1, int(qRound(src.height() * factor)));
    return scale(src, QSize(w, h), method);
}

} // namespace resample
} // namespace zl
