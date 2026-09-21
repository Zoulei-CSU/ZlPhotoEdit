// ============================================================================
//  Histogram.cpp —— 直方图统计的实现
// ----------------------------------------------------------------------------
//  全部是"输入 QImage → 输出统计数组"的无副作用纯函数，
//  不依赖任何 QtWidgets —— 因此可以放进离线自检（tests/adjust_test.cpp）。
//
//  耗时参考：4000×3000（1200 万像素）单线程约 30ms。这个量级决定了两件事：
//    · 只在"打开对话框时"算一次，绝不在滑块拖动时重算
//    · 预览用降采样副本时，直方图也用降采样副本算（约 1/12 的量）
//  见 docs/曲线调整设计.md 关于背景直方图的成本说明。
// ============================================================================

#include "engine/Histogram.h"

#include <QtGlobal>
#include <cmath>

namespace zl {
namespace histogram {

namespace {

// 内部共用：把图像转成非预乘 ARGB32 后逐像素回调整理。
//
// 用函数模板把"取哪个值"抽出来，避免三段几乎相同的遍历代码 ——
// 这类复制粘贴的遍历最容易出现"改了亮度通道忘了改绿通道"的错。
template <typename PickValue>
QVector<int> compute(const QImage &image, bool skipTransparent, PickValue pick)
{
    QVector<int> hist(256, 0);

    if (image.isNull())
        return hist;

    // ★ 必须转非预乘 ★ 见头文件里的说明。
    //   Format_ARGB32 与 ARGB32_Premultiplied 是不同格式，这里的转换
    //   会真的做一次逐像素反预乘；若原图本来就是 ARGB32 则是浅拷贝。
    const QImage src = image.convertToFormat(QImage::Format_ARGB32);
    if (src.isNull())
        return hist;

    const int w = src.width();
    const int h = src.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb px = line[x];

            if (skipTransparent && qAlpha(px) == 0)
                continue;

            const int v = pick(px);
            // 保险起见再夹一次：pick 的实现里有浮点运算，极端输入下
            // 可能算出 256（例如全通道 255 时 0.299+0.587+0.114 == 1.0
            // 在浮点误差下变成 1.0000001）。越界写会把相邻内存改坏，
            // 这种崩溃极难定位，所以宁可多一次判断。
            hist[qBound(0, v, 255)]++;
        }
    }

    return hist;
}

} // namespace

QVector<int> luminance(const QImage &image, bool skipTransparent)
{
    return compute(image, skipTransparent, [](QRgb px) {
        // 与 filters::grayscale 相同的权重。用 lround 而不是隐式截断，
        // 否则同一个像素在直方图与去色结果里会落在相邻的桶中。
        return int(std::lround(0.299 * qRed(px) + 0.587 * qGreen(px) + 0.114 * qBlue(px)));
    });
}

QVector<int> singleChannel(const QImage &image, int channel, bool skipTransparent)
{
    switch (channel) {
    case 1:  return compute(image, skipTransparent, [](QRgb px) { return qGreen(px); });
    case 2:  return compute(image, skipTransparent, [](QRgb px) { return qBlue(px); });
    case 0:
    default: return compute(image, skipTransparent, [](QRgb px) { return qRed(px); });
    }
}

QVector<qreal> normalized(const QVector<int> &hist, bool logScale)
{
    QVector<qreal> out(hist.size(), 0.0);
    if (hist.isEmpty())
        return out;

    int maxCount = 0;
    for (int v : hist)
        maxCount = qMax(maxCount, v);

    if (maxCount <= 0)
        return out;   // 空直方图：全 0，调用方据此跳过绘制

    // log 的底数用同一个值，比值才与底数无关 —— 所以直接写成
    // log(1+n)/log(1+max) 而不是 log10 之类。
    const qreal denom = logScale ? std::log(1.0 + maxCount) : qreal(maxCount);

    for (int i = 0; i < hist.size(); ++i) {
        out[i] = logScale ? std::log(1.0 + hist[i]) / denom
                          : qreal(hist[i]) / denom;
    }
    return out;
}

} // namespace histogram
} // namespace zl
