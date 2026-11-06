#include "engine/DocumentScan.h"

#include <QPainter>
#include <QTransform>
#include <QVector>

#include <algorithm>
#include <cmath>

namespace zl {
namespace scan {

namespace {

// 亮度权重与 Adjust / Filters 保持同一套（0.299 / 0.587 / 0.114 的定点版）。
// 用同一个公式很重要：用户在"去色"与"文档增强"之间切换时，
// 灰度的深浅应当一致，否则会觉得其中一个"偏"。
inline int lumaOf(int r, int g, int b)
{
    return (r * 77 + g * 150 + b * 29) >> 8;
}

// 一维盒式模糊（水平）。窗口越界时按边缘像素重复（clamp），
// 否则图像四周会出现"越糊越黑"的暗边 —— 背景估计一旦有暗边，
// 归一化就会把边缘的纸算成灰的。
void boxBlurRow(const quint8 *src, quint8 *dst, int w, int h, int r)
{
    const int win = 2 * r + 1;
    for (int y = 0; y < h; ++y) {
        const quint8 *row = src + ptrdiff_t(y) * w;
        quint8 *out = dst + ptrdiff_t(y) * w;

        int sum = 0;
        for (int i = -r; i <= r; ++i)
            sum += row[std::clamp(i, 0, w - 1)];
        out[0] = quint8(sum / win);

        for (int x = 1; x < w; ++x) {
            sum -= row[std::clamp(x - 1 - r, 0, w - 1)];
            sum += row[std::clamp(x + r, 0, w - 1)];
            out[x] = quint8(sum / win);
        }
    }
}

// 一维盒式模糊（垂直）。src 与 dst 布局相同，按行跨步取。
void boxBlurCol(const quint8 *src, quint8 *dst, int w, int h, int r)
{
    const int win = 2 * r + 1;
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int i = -r; i <= r; ++i)
            sum += src[ptrdiff_t(std::clamp(i, 0, h - 1)) * w + x];
        dst[x] = quint8(sum / win);

        for (int y = 1; y < h; ++y) {
            sum -= src[ptrdiff_t(std::clamp(y - 1 - r, 0, h - 1)) * w + x];
            sum += src[ptrdiff_t(std::clamp(y + r, 0, h - 1)) * w + x];
            dst[ptrdiff_t(y) * w + x] = quint8(sum / win);
        }
    }
}

// 两遍可分离盒式模糊（≈ 三角核，比单遍方窗平滑，且比高斯便宜）。
void blurGray(const QVector<quint8> &src, QVector<quint8> &dst,
              QVector<quint8> &tmp, int w, int h, int radius)
{
    boxBlurRow(src.constData(), tmp.data(), w, h, radius);
    boxBlurCol(tmp.constData(), dst.data(), w, h, radius);
    // 第二遍：把结果再抹一次，抹掉方窗带来的"方块感"
    boxBlurRow(dst.constData(), tmp.data(), w, h, radius);
    boxBlurCol(tmp.constData(), dst.data(), w, h, radius);
}

// 目标尺寸至少 1×1，且不超过某个上限 —— 四角被拖成极端形状时
// （例如三个点挤在一角）边长会算出一个巨大的值，直接建图会 OOM。
QSize clampOutputSize(const QSize &s)
{
    constexpr int kMaxEdge = 20000;   // 20000² × 4B ≈ 1.6GB，已经是失控范围
    return QSize(std::clamp(s.width(), 1, kMaxEdge),
                 std::clamp(s.height(), 1, kMaxEdge));
}

} // namespace

// ===========================================================================
//  透视展平
// ===========================================================================

QImage unwarp(const QImage &src, const QPolygonF &quad, const QSize &outputSize,
              ResampleMethod method)
{
    if (src.isNull() || quad.size() != 4 || !outputSize.isValid())
        return QImage();

    const QSize out = clampOutputSize(outputSize);
    if (out.isEmpty())
        return QImage();

    // 目标四角：与源同序（左上 → 右上 → 右下 → 左下）
    QPolygonF dstQuad;
    dstQuad << QPointF(0, 0)
            << QPointF(out.width(), 0)
            << QPointF(out.width(), out.height())
            << QPointF(0, out.height());

    QTransform t;
    if (!QTransform::quadToQuad(quad, dstQuad, t))
        return QImage();   // 奇异（三点共线等）

    QImage result(out, QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    QPainter painter(&result);
    // Nearest 关掉平滑（用于调试/像素画），其余一律开。
    painter.setRenderHint(QPainter::SmoothPixmapTransform,
                          method != ResampleMethod::Nearest);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setTransform(t);
    painter.drawImage(0, 0, src);
    painter.end();

    return result;
}

QImage unwarpMask(const QImage &grayMask, const QPolygonF &quad,
                  const QSize &outputSize)
{
    if (grayMask.isNull() || quad.size() != 4 || !outputSize.isValid())
        return QImage();

    // ★ 把蒙版当"白色 + alpha"的图去变换，回来取 alpha ★
    //   直接把灰度当 RGB 画在透明底上是不行的：SourceOver 会把
    //   "灰度值"当颜色、alpha 仍是 0，插值完 alpha 恒为 0，蒙版全空。
    //   alpha 必须与灰度值同步，才等于"把蒙版当成一张形状图"在变形。
    const QImage gray = (grayMask.format() == QImage::Format_Grayscale8)
                            ? grayMask
                            : grayMask.convertToFormat(QImage::Format_Grayscale8);

    QImage asAlpha(gray.size(), QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < gray.height(); ++y) {
        const uchar *srcLine = gray.constScanLine(y);
        QRgb *dstLine = reinterpret_cast<QRgb *>(asAlpha.scanLine(y));
        for (int x = 0; x < gray.width(); ++x) {
            const int v = srcLine[x];
            dstLine[x] = qRgba(255, 255, 255, v);
        }
    }

    const QImage warped = unwarp(asAlpha, quad, outputSize,
                                 ResampleMethod::Bilinear);
    if (warped.isNull())
        return QImage();

    QImage mask(warped.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < warped.height(); ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(warped.constScanLine(y));
        uchar *dstLine = mask.scanLine(y);
        for (int x = 0; x < warped.width(); ++x)
            dstLine[x] = uchar(qAlpha(srcLine[x]));
    }
    return mask;
}

QSize suggestOutputSize(const QPolygonF &quad)
{
    if (quad.size() != 4)
        return QSize(1, 1);

    const auto dist = [](const QPointF &a, const QPointF &b) {
        return std::hypot(b.x() - a.x(), b.y() - a.y());
    };

    // 上边 0-1，下边 3-2，左边 0-3，右边 1-2
    const qreal wTop = dist(quad[0], quad[1]);
    const qreal wBottom = dist(quad[3], quad[2]);
    const qreal hLeft = dist(quad[0], quad[3]);
    const qreal hRight = dist(quad[1], quad[2]);

    const int w = int(std::lround(std::max(wTop, wBottom)));
    const int h = int(std::lround(std::max(hLeft, hRight)));
    return clampOutputSize(QSize(w, h));
}

bool isQuadUsable(const QPolygonF &quad)
{
    if (quad.size() != 4)
        return false;

    // ① 相邻点不能重合（重合会让映射退化，quadToQuad 直接失败）
    for (int i = 0; i < 4; ++i) {
        const QPointF d = quad[(i + 1) % 4] - quad[i];
        if (std::hypot(d.x(), d.y()) < 2.0)
            return false;
    }

    // ② 四个顶点的转向必须同号（凸四边形）；同号同时排除了自交的蝴蝶形
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = quad[i];
        const QPointF b = quad[(i + 1) % 4];
        const QPointF c = quad[(i + 2) % 4];
        const qreal cross = (b.x() - a.x()) * (c.y() - b.y())
                          - (b.y() - a.y()) * (c.x() - b.x());
        if (std::abs(cross) < 1e-9)
            return false;          // 三点共线 → 矩阵奇异
        const int s = cross > 0 ? 1 : -1;
        if (sign == 0)
            sign = s;
        else if (s != sign)
            return false;          // 凹形 / 自交
    }

    // ③ 面积不能太小（拖成一条缝时虽然凸，但展平出来是噪声）
    qreal area2 = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF a = quad[i];
        const QPointF b = quad[(i + 1) % 4];
        area2 += a.x() * b.y() - b.x() * a.y();
    }
    return std::abs(area2) * 0.5 >= 16.0;
}

// ===========================================================================
//  白底黑字增强
// ===========================================================================

QImage enhance(const QImage &src, const EnhanceParams &params)
{
    if (src.isNull())
        return QImage();

    // ★ 统一到非预乘 ARGB32 ★
    //   下面要按"原始 RGB 值"做除法与查表，预乘空间里的 R/G/B 已经被
    //   alpha 乘过，半透明像素会被当成暗部（见 Lut.h 顶部那段）。
    //   最后 return 时 Qt 会按需转换回图层用的预乘格式。
    QImage work = (src.format() == QImage::Format_ARGB32)
                      ? src.copy()
                      : src.convertToFormat(QImage::Format_ARGB32);
    if (work.isNull())
        return QImage();

    const int w = work.width();
    const int h = work.height();
    const int minDim = std::max(1, std::min(w, h));
    const int pixels = w * h;

    // ── ① 亮度图 ──
    QVector<quint8> luma(pixels);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        quint8 *out = luma.data() + ptrdiff_t(y) * w;
        for (int x = 0; x < w; ++x)
            out[x] = quint8(lumaOf(qRed(line[x]), qGreen(line[x]), qBlue(line[x])));
    }

    // ── ② 局部背景估计 ──
    //
    // ★ 半径是"图像短边的百分比"而不是固定像素 ★
    //   固定半径在 800×600 的图上会糊成一整片（背景全等于平均亮度，
    //   等于没做匀光），在 6000×4000 的图上又只抹掉极小一块。
    //   按短边取百分比，两种尺寸下得到的是同一种"尺度感"。
    //   下限 4 是模糊核的物理下限，上限 1/3 短边是为了不让背景估计
    //   退化成"整幅平均"。
    const qreal shadowRatio = 0.02 + 0.06 * std::clamp(params.shadow, 0, 100) / 100.0;
    const int radius = std::clamp(int(std::lround(minDim * shadowRatio)),
                                  4, std::max(4, minDim / 3));

    QVector<quint8> background(pixels);
    QVector<quint8> tmp(pixels);
    blurGray(luma, background, tmp, w, h, radius);

    // ── ③④ 合成 256×256 二维表 ──
    //
    // 行 = 背景亮度，列 = 像素值；内容 = 归一化后再过曲线的结果。
    // 一次烘表，逐像素就只剩查表 —— 没有除法、没有 pow（见头文件的性能说明）。
    const int blackPoint = std::clamp(params.contrast * 55 / 100, 0, 120);
    const int whitePoint = std::max(
        blackPoint + 8,
        255 - std::clamp(params.whiten * 85 / 100, 0, 200));
    // ★ 系数 0.016 是"默认档就要看得出效果"调出来的 ★
    //   默认 contrast=50 时 gamma≈1.8：归一化后落在 0.4 左右的灰字
    //   会压到 50/255（真黑），而纸（n≈255 → t=1）不受 gamma 影响，仍是纯白。
    //   取 0.012 时默认只能压到 80 上下 —— 屏幕上仍是一片灰，用户会觉得
    //   "这个功能没起作用"。
    const qreal gamma = 1.0 + std::clamp(params.contrast, 0, 100) * 0.016;

    QVector<quint8> curve(256);
    for (int i = 0; i < 256; ++i) {
        qreal t = (i - blackPoint) / qreal(whitePoint - blackPoint);
        t = std::clamp(t, 0.0, 1.0);
        curve[i] = quint8(std::clamp(int(std::lround(std::pow(t, gamma) * 255.0)), 0, 255));
    }

    QVector<quint8> table(256 * 256);
    for (int b = 0; b < 256; ++b) {
        quint8 *row = table.data() + b * 256;
        if (b <= 24) {
            // 背景本身极暗（画外黑边、镜头暗角）：做除法只会把噪点放大成麻点。
            // 这种区域本来就没有纸，直接原样过曲线。
            for (int v = 0; v < 256; ++v)
                row[v] = curve[v];
        } else {
            for (int v = 0; v < 256; ++v) {
                const int n = std::min(255, (v * 255 + b / 2) / b);
                row[v] = curve[n];
            }
        }
    }

    // ── 逐像素 ──
    //
    // 原地改：背景表已经烘好，每个像素只依赖自己与它所在位置的背景，
    // 与邻居的**新值**无关，所以不需要额外的输出缓冲（省一整张图的内存）。
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(work.scanLine(y));
        const quint8 *bgLine = background.constData() + ptrdiff_t(y) * w;
        const quint8 *lumaLine = luma.constData() + ptrdiff_t(y) * w;

        for (int x = 0; x < w; ++x) {
            const QRgb c = line[x];
            const quint8 *row = table.constData() + int(bgLine[x]) * 256;

            if (params.grayscale) {
                const quint8 v = row[lumaLine[x]];
                line[x] = qRgba(v, v, v, qAlpha(c));
            } else {
                line[x] = qRgba(row[qRed(c)], row[qGreen(c)], row[qBlue(c)],
                                qAlpha(c));
            }
        }
    }

    return work;
}

} // namespace scan
} // namespace zl
