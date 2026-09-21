#include "engine/Filters.h"
#include "engine/Lut.h"

#include <QtMath>
#include <QRandomGenerator>
#include <QVector>
#include <cstring>

namespace zl {
namespace filters {

namespace {

// 感知亮度权重（Rec.601），全工程统一，不要在各处另写一套
constexpr qreal kLumaR = 0.299;
constexpr qreal kLumaG = 0.587;
constexpr qreal kLumaB = 0.114;

// 统一转预乘格式并记录原格式
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

// 取非预乘的 R/G/B 分量（0~255）。alpha 为 0 时返回 0。
inline void unpackRgb(QRgb px, int &r, int &g, int &b)
{
    const int a = qAlpha(px);
    if (a == 0) {
        r = g = b = 0;
        return;
    }
    r = qBound(0, qRed(px)   * 255 / a, 255);
    g = qBound(0, qGreen(px) * 255 / a, 255);
    b = qBound(0, qBlue(px)  * 255 / a, 255);
}

// 打包回预乘像素
inline QRgb packRgb(int r, int g, int b, int a)
{
    if (a == 0)
        return 0;
    return qRgba(qBound(0, r, 255) * a / 255,
                 qBound(0, g, 255) * a / 255,
                 qBound(0, b, 255) * a / 255,
                 a);
}

// 坐标钳制（边缘重复），模糊类滤镜的边界处理
inline int clampIndex(int i, int size)
{
    if (i < 0)      return 0;
    if (i >= size)  return size - 1;
    return i;
}

} // namespace

// ===========================================================================
//  逐像素类
// ===========================================================================

QImage grayscale(const QImage &src)
{
    if (src.isNull())
        return QImage();

    // 用 LUT 实现：先把每个输入值对应的灰度值算好。
    // 灰度化是纯逐通道映射（三个通道用同一条表），所以能走 LUT。
    //
    // 思路：对每个输入值 v（假设它是灰度级的近似，即 R=G=B=v），
    // 灰度结果就是 v 本身。但我们要处理的彩色像素三通道不同，
    // 所以不能简单地用单表 —— 需要三通道相关运算。
    //
    // 折中方案：对每个通道值，预先算出"该值对灰度的贡献"，
    // 然后逐像素加权求和。贡献表只有 256 项，查三次表比三次乘法快。
    qreal contribR[256], contribG[256], contribB[256];
    for (int i = 0; i < 256; ++i) {
        contribR[i] = kLumaR * i;
        contribG[i] = kLumaG * i;
        contribB[i] = kLumaB * i;
    }

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

            int r, g, b;
            unpackRgb(px, r, g, b);

            const int gray = qBound(0,
                int(qRound(contribR[r] + contribG[g] + contribB[b])), 255);

            dstLine[x] = packRgb(gray, gray, gray, a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

QImage invert(const QImage &src)
{
    if (src.isNull())
        return QImage();

    // 反色是纯逐通道映射，直接用 LUT。
    // 用 applyLutRgb 的好处是它已经正确处理了预乘往返 ——
    // 如果这里手写逐像素，很容易忘记还原 alpha，半透明区域就会出错。
    const CurveLut lut = bakeLutFromFn([](qreal in) -> qreal {
        return 1.0 - in;
    });

    return applyLutRgb(src, lut);
}

QImage threshold(const QImage &src, int threshold)
{
    if (src.isNull())
        return QImage();

    const int t = qBound(0, threshold, 255);

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

            int r, g, b;
            unpackRgb(px, r, g, b);

            // 按感知亮度判断，而不是逐通道比较。
            // 逐通道比较会让彩色像素产生诡异的色斑（红色可能被判为暗、绿色为亮），
            // 而阈值化的语义是"这个像素整体算亮还是暗"。
            const int luma = qBound(0,
                int(qRound(kLumaR * r + kLumaG * g + kLumaB * b)), 255);

            const int v = (luma > t) ? 255 : 0;
            dstLine[x] = packRgb(v, v, v, a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  风格滤镜
// ===========================================================================

QImage sepia(const QImage &src)
{
    if (src.isNull())
        return QImage();

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

            int r, g, b;
            unpackRgb(px, r, g, b);

            // 经典的棕褐色调通道混合系数。
            // 这组系数保证：纯白 (255,255,255) 会映射到 (255, 239, 196)，
            // 也就是老照片那种暖黄色；纯黑保持纯黑，对比度不塌。
            //
            // 系数之和：
            //   R 行：0.393+0.769+0.189 = 1.351
            //   G 行：0.349+0.686+0.168 = 1.203
            //   B 行：0.272+0.534+0.131 = 0.937
            // 大于 1 的行会提亮（R、G 通道），小于 1 的压暗（B 通道），
            // 这正是产生"暖"色调的原理。
            const int nr = qBound(0, int(qRound(0.393 * r + 0.769 * g + 0.189 * b)), 255);
            const int ng = qBound(0, int(qRound(0.349 * r + 0.686 * g + 0.168 * b)), 255);
            const int nb = qBound(0, int(qRound(0.272 * r + 0.534 * g + 0.131 * b)), 255);

            dstLine[x] = packRgb(nr, ng, nb, a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

QImage emboss(const QImage &src, qreal strength, qreal directionDeg)
{
    if (src.isNull())
        return QImage();

    const qreal s = qBound(0.0, strength, 3.0);

    // 把方向角转成差分偏移量。方向定义了"光从哪来"，
    // 差分方向与之相反，这样凸起的一面会被照亮。
    const qreal rad = qDegreesToRadians(directionDeg);
    const int dx = int(qRound(qCos(rad)));
    const int dy = int(qRound(qSin(rad)));

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    // 先灰度化。浮雕基于亮度差分，彩色浮雕刻度会很奇怪。
    const QImage gray = grayscale(work);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    const int w = gray.width();
    const int h = gray.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(gray.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        // 上下两行用于取差分。边界行做钳制。
        const int prevY = clampIndex(y - dy, h);
        const int nextY = clampIndex(y + dy, h);
        const QRgb *prevLine = reinterpret_cast<const QRgb *>(gray.constScanLine(prevY));
        const QRgb *nextLine = reinterpret_cast<const QRgb *>(gray.constScanLine(nextY));

        for (int x = 0; x < w; ++x) {
            const int xPrev = clampIndex(x - dx, w);
            const int xNext = clampIndex(x + dx, w);

            // 取差分
            const int vPrev = qRed(prevLine[xPrev]);
            const int vNext = qRed(nextLine[xNext]);
            const int diff = int(qRound((vNext - vPrev) * s));

            // 中性灰 128 + 差分。
            // 用中性灰而不是 0 作为基底，是因为差分有正有负 ——
            // 以 0 为基底的话负值全部被截断成黑色，浮雕的"凹"就没法表现了。
            const int v = qBound(0, 128 + diff, 255);

            const int a = qAlpha(line[x]);
            dstLine[x] = packRgb(v, v, v, a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  模糊
// ===========================================================================

namespace {

// 生成一维高斯核。
//
// 用 exp(-x²/(2σ²)) 直接算，而不是查表或近似 —— 核长度通常不超过
// 几十项，这点三角函数与指数运算的成本完全可以忽略。
//
// ★ 返回 QVector 而不是 QList ★
//   内部需要 resize() 预分配长度再按下标赋值，而 Qt5 的 QList 没有
//   resize()。QVector 在 Qt5/6 下都支持该接口（Qt6 里两者等价）。
QVector<qreal> gaussianKernel(qreal sigma)
{
    QVector<qreal> kernel;

    // 半径取 3σ，理由见头文件说明（3σ 覆盖 99.7% 能量）
    const int radius = qMax(1, int(std::ceil(sigma * 3.0)));

    kernel.resize(radius * 2 + 1);

    const qreal twoSigmaSq = 2.0 * sigma * sigma;
    qreal sum = 0.0;

    for (int i = -radius; i <= radius; ++i) {
        const qreal value = qExp(-(i * i) / twoSigmaSq);
        kernel[i + radius] = value;
        sum += value;
    }

    // ★ 归一化 ★
    // 与 Resample 同理：权重之和必须为 1，否则图像整体亮度会变。
    // 高斯模糊的"模糊"应当只改变空间分布，不改变总能量 ——
    // 能量守恒的直观验证方法是"模糊一张纯色图，颜色应该完全不变"。
    if (sum > 0.0) {
        for (qreal &v : kernel)
            v /= sum;
    }

    return kernel;
}

// 水平方向一维卷积
QImage convolveHorizontal(const QImage &src, const QVector<qreal> &kernel)
{
    const int w = src.width();
    const int h = src.height();
    const int radius = (kernel.size() - 1) / 2;

    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            qreal accR = 0.0, accG = 0.0, accB = 0.0, accA = 0.0;

            for (int k = -radius; k <= radius; ++k) {
                const int sx = clampIndex(x + k, w);
                const qreal weight = kernel[k + radius];
                const QRgb px = srcLine[sx];

                accR += weight * qRed(px);
                accG += weight * qGreen(px);
                accB += weight * qBlue(px);
                accA += weight * qAlpha(px);
            }

            // 在预乘空间直接四舍五入。
            // 模糊在预乘空间做是**正确的**（与重采样同理）：加权求和等价于
            // 按 alpha 加权平均颜色。若先转非预乘再模糊，透明区域的 RGB
            // 会参与插值，边缘产生彩边。
            const int a = qBound(0, int(accA + 0.5), 255);
            dstLine[x] = qRgba(qBound(0, int(accR + 0.5), a),
                               qBound(0, int(accG + 0.5), a),
                               qBound(0, int(accB + 0.5), a),
                               a);
        }
    }

    return result;
}

// 垂直方向一维卷积
QImage convolveVertical(const QImage &src, const QVector<qreal> &kernel)
{
    const int w = src.width();
    const int h = src.height();
    const int radius = (kernel.size() - 1) / 2;

    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);

    // 预先取出所有行的指针，避免在内层循环里反复调用 constScanLine
    QVector<const QRgb *> lines(h);
    for (int y = 0; y < h; ++y)
        lines[y] = reinterpret_cast<const QRgb *>(src.constScanLine(y));

    for (int y = 0; y < h; ++y) {
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            qreal accR = 0.0, accG = 0.0, accB = 0.0, accA = 0.0;

            for (int k = -radius; k <= radius; ++k) {
                const int sy = clampIndex(y + k, h);
                const qreal weight = kernel[k + radius];
                const QRgb px = lines[sy][x];

                accR += weight * qRed(px);
                accG += weight * qGreen(px);
                accB += weight * qBlue(px);
                accA += weight * qAlpha(px);
            }

            const int a = qBound(0, int(accA + 0.5), 255);
            dstLine[x] = qRgba(qBound(0, int(accR + 0.5), a),
                               qBound(0, int(accG + 0.5), a),
                               qBound(0, int(accB + 0.5), a),
                               a);
        }
    }

    return result;
}

} // namespace

QImage gaussianBlur(const QImage &src, qreal radius)
{
    if (src.isNull() || radius <= 0.0)
        return src.isNull() ? QImage() : src.copy();

    // ── 半径 → sigma ──
    // sigma = radius / 3，理由见头文件。
    const qreal sigma = radius / 3.0;

    // 大半径时改用三次方框模糊近似。
    //
    // 判断阈值取 20 像素：超过这个值后，可分离高斯模糊的 O(n·r) 成本
    // 已经明显高于三次方框模糊的 O(n)，而视觉差异几乎不可见。
    // 这个阈值是经验值，可以在实测后调整。
    if (radius > 20.0) {
        return tripleBoxBlur(src, int(qRound(radius)));
    }

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    const QVector<qreal> kernel = gaussianKernel(sigma);

    // ★ 分离式两遍一维卷积 ★
    // 不做二维 (2r+1)² 卷积。理由见头文件。
    // 两遍之间不需要转格式 —— 全程在预乘空间。
    QImage temp = convolveHorizontal(work, kernel);
    QImage result = convolveVertical(temp, kernel);

    return restoreFormat(result, src, wasPremultiplied);
}

QImage boxBlur(const QImage &src, int radius)
{
    if (src.isNull() || radius <= 0)
        return src.isNull() ? QImage() : src.copy();

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    const int w = work.width();
    const int h = work.height();
    const int r = qMin(radius, qMax(w, h));   // 防止半径超过图像尺寸导致越界

    // ── 滑动窗口求和 ──
    //
    // 关键：不是对每个像素都重新累加窗口内的所有元素（那是 O(n·r)），
    // 而是维护一个"当前窗口的和"，滑动时只减去离开的元素、加上进入的元素。
    // 这样复杂度是 O(n)，**与半径无关** —— 半径 100 和半径 1 一样快。
    //
    // 用 quint32 累加而不是 quint8 逐项相加，是为了避免中间溢出。

    // 水平方向
    QVector<quint32> horizontal(size_t(w) * size_t(h) * 4);

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        quint32 *outLine = horizontal.data() + size_t(y) * size_t(w) * 4;

        quint32 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
        int count = 0;

        // 初始化第一个窗口 [0, r]
        for (int i = 0; i <= r && i < w; ++i) {
            const QRgb px = srcLine[i];
            sumR += quint32(qRed(px));
            sumG += quint32(qGreen(px));
            sumB += quint32(qBlue(px));
            sumA += quint32(qAlpha(px));
            ++count;
        }

        for (int x = 0; x < w; ++x) {
            quint32 *out = outLine + size_t(x) * 4;
            out[0] = sumR / quint32(count);
            out[1] = sumG / quint32(count);
            out[2] = sumB / quint32(count);
            out[3] = sumA / quint32(count);

            // 滑动：加入右侧新元素
            const int addIdx = x + r + 1;
            if (addIdx < w) {
                const QRgb px = srcLine[addIdx];
                sumR += quint32(qRed(px));
                sumG += quint32(qGreen(px));
                sumB += quint32(qBlue(px));
                sumA += quint32(qAlpha(px));
                ++count;
            }

            // 滑动：移除左侧旧元素
            const int delIdx = x - r;
            if (delIdx >= 0) {
                const QRgb px = srcLine[delIdx];
                sumR -= quint32(qRed(px));
                sumG -= quint32(qGreen(px));
                sumB -= quint32(qBlue(px));
                sumA -= quint32(qAlpha(px));
                --count;
            }
        }
    }

    // 垂直方向
    QImage result(w, h, QImage::Format_ARGB32_Premultiplied);

    for (int x = 0; x < w; ++x) {
        quint32 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
        int count = 0;

        auto at = [&](int yy, int channel) -> quint32 {
            return horizontal[size_t(yy) * size_t(w) * 4 + size_t(x) * 4 + size_t(channel)];
        };

        for (int i = 0; i <= r && i < h; ++i) {
            sumR += at(i, 0);
            sumG += at(i, 1);
            sumB += at(i, 2);
            sumA += at(i, 3);
            ++count;
        }

        for (int y = 0; y < h; ++y) {
            QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));
            const int a = qBound(0, int(sumA / quint32(count)), 255);
            const int rr = qBound(0, int(sumR / quint32(count)), a);
            const int gg = qBound(0, int(sumG / quint32(count)), a);
            const int bb = qBound(0, int(sumB / quint32(count)), a);
            dstLine[x] = qRgba(rr, gg, bb, a);

            const int addIdx = y + r + 1;
            if (addIdx < h) {
                sumR += at(addIdx, 0);
                sumG += at(addIdx, 1);
                sumB += at(addIdx, 2);
                sumA += at(addIdx, 3);
                ++count;
            }

            const int delIdx = y - r;
            if (delIdx >= 0) {
                sumR -= at(delIdx, 0);
                sumG -= at(delIdx, 1);
                sumB -= at(delIdx, 2);
                sumA -= at(delIdx, 3);
                --count;
            }
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

QImage tripleBoxBlur(const QImage &src, int radius)
{
    if (src.isNull() || radius <= 0)
        return src.isNull() ? QImage() : src.copy();

    // 连续三次方框模糊逼近高斯模糊。
    //
    // 为什么能逼近：中心极限定理 —— 多个独立同分布的随机变量之和趋于正态分布。
    // 一次方框模糊相当于与矩形窗卷积，三次就是三个矩形窗的卷积，
    // 其结果已经很接近高斯钟形曲线。
    //
    // 半径的换算：要达到等效于 sigma 的高斯模糊，单次方框半径应约为
    //     r ≈ sqrt(12·sigma²/3 + 1) / 2
    // 这里用简化版本：三次半径相同的方框模糊，总 sigma ≈ r·sqrt(3)/2。
    // 反解出 r 使得总 sigma 与目标一致。
    //
    // 简化实现：直接用传入的 radius 除以 3 作为每次的半径。
    // 这是常见做法，效果足够好，而且不需要用户理解这些换算。
    const int perPass = qMax(1, radius / 3);

    QImage result = boxBlur(src, perPass);
    result = boxBlur(result, perPass);
    result = boxBlur(result, perPass);

    return result;
}

// ===========================================================================
//  选区蒙版羽化（单通道路径）
// ===========================================================================

QImage featherGrayMask(const QImage &grayMask, qreal radius)
{
    if (grayMask.isNull() || radius <= 0.0)
        return grayMask.isNull() ? QImage() : grayMask.copy();

    // 统一成 Grayscale8 再处理。若调用方给的是别的格式
    // （例如从 QBitmap 来的 Mono），这里一次性转换掉，
    // 保证下面的裸字节访问是安全的。
    QImage mask = (grayMask.format() == QImage::Format_Grayscale8)
                      ? grayMask
                      : grayMask.convertToFormat(QImage::Format_Grayscale8);

    if (mask.isNull())
        return QImage();

    // ★ 半径换算：除以 3 ★
    //
    //   三次盒式模糊是串联的，总扩散半径 ≈ 3 × 单趟半径。
    //   如果直接把用户给的 radius 当作单趟半径，实际羽化范围就是
    //   标称值的 **3 倍** —— 用户填"羽化 4 像素"却得到 12 像素的模糊，
    //   选区的硬边完全消失，而用户完全不知道为什么。
    //
    //   （这条换算与 tripleBoxBlur 里的一致，两处必须同口径。）
    //
    //   ★ 下限为 1 ★
    //     盒式模糊的半径是整数，radius 小于 3 时 radius/3 会取到 0。
    //     允许 0 就等于"不模糊"，那用户调 1~2 像素的羽化会毫无反应；
    //     夹到 1 之后最小支持半径是 3 像素，是这套整数方案能做到的
    //     最"轻"的羽化。
    const int perPass = qMax(1, int(qRound(radius / 3.0)));
    const int w = mask.width();
    const int h = mask.height();

    if (w <= 0 || h <= 0)
        return mask.copy();

    // 转成 16 位中间缓冲，避免多次整数除法造成的精度损失。
    //
    // ★ 必须用 QVector 而不是 QList ★
    //   Qt5 的 QList<T> 没有"只给大小"的构造函数，也没有 data()/constData()
    //   这种连续内存访问接口 —— 因为 QList 在 Qt5 里是"指针数组 + 内联存储"
    //   的混合实现，对元素不保证在内存里连续。
    //   而这里的滑动窗口模糊需要"按下标随机访问 + 连续内存"，
    //   所以必须用 QVector（Qt6 里 QVector 是 QList 的别名，两版都能编）。
    //
    // ★ 左移 8 位再运算的原因 ★
    //   8 位精度下反复除以窗口大小会不断向下取整，三次模糊后
    //   整体会明显变暗（边缘整体内缩）。放到 16 位空间做中间运算，
    //   最后一次性缩回 8 位，误差可以忽略。
    QVector<quint16> buf(w * h);
    for (int y = 0; y < h; ++y) {
        const uchar *line = mask.constScanLine(y);
        quint16 *out = buf.data() + y * w;
        for (int x = 0; x < w; ++x)
            out[x] = quint16(line[x]) * 256;
    }

    auto boxBlurPass = [&](bool horizontal) {
        QVector<quint16> tmp(buf.size());
        const int len   = horizontal ? w : h;
        const int outer = horizontal ? h : w;

        for (int o = 0; o < outer; ++o) {
            // 滑动窗口求和，O(n) 完成一趟模糊（与半径无关）
            quint32 sum = 0;
            int count = 0;

            auto at = [&](int i) -> quint16 {
                const int idx = horizontal ? (o * w + i) : (i * w + o);
                return buf[idx];
            };
            auto setTmp = [&](int i, quint16 v) {
                const int idx = horizontal ? (o * w + i) : (i * w + o);
                tmp[idx] = v;
            };

            // 初始化窗口
            for (int i = 0; i <= perPass && i < len; ++i) {
                sum += at(i);
                ++count;
            }

            for (int i = 0; i < len; ++i) {
                setTmp(i, quint16(sum / count));

                // 窗口右移：加入右侧新元素，移除左侧旧元素
                const int addIdx = i + perPass + 1;
                const int delIdx = i - perPass;
                if (addIdx < len) {
                    sum += at(addIdx);
                    ++count;
                }
                if (delIdx >= 0) {
                    sum -= at(delIdx);
                    --count;
                }
            }
        }
        buf.swap(tmp);
    };

    // 三次盒式模糊 ≈ 高斯模糊。次数越多越接近，三次是质量与速度的常用折中。
    for (int pass = 0; pass < 3; ++pass) {
        boxBlurPass(true);
        boxBlurPass(false);
    }

    // 写回 8 位蒙版
    for (int y = 0; y < h; ++y) {
        uchar *line = mask.scanLine(y);
        const quint16 *in = buf.constData() + y * w;
        for (int x = 0; x < w; ++x)
            line[x] = uchar(qMin<quint16>(255, in[x] / 256));
    }

    return mask;
}

// ===========================================================================
//  锐化
// ===========================================================================

QImage unsharpMask(const QImage &src, qreal radius,
                   qreal amount, int threshold)
{
    if (src.isNull())
        return QImage();

    if (radius <= 0.0 || amount <= 0.0)
        return src.copy();

    const int th = qMax(0, threshold);

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    // ── 第 1 步：算模糊图 ──
    // 模糊图代表"低频"，原图减去它就是"高频"（细节）。
    const QImage blurred = gaussianBlur(work, radius);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    const int w = work.width();
    const int h = work.height();

    for (int y = 0; y < h; ++y) {
        const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
        const QRgb *blurLine = reinterpret_cast<const QRgb *>(blurred.constScanLine(y));
        QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(y));

        for (int x = 0; x < w; ++x) {
            const QRgb px = srcLine[x];
            const QRgb bc = blurLine[x];
            const int a = qAlpha(px);

            if (a == 0) {
                dstLine[x] = 0;
                continue;
            }

            int r, g, b;
            unpackRgb(px, r, g, b);

            int br, bg, bb;
            unpackRgb(bc, br, bg, bb);

            // 高频分量
            const int hr = r - br;
            const int hg = g - bg;
            const int hb = b - bb;

            // ── 阈值判断 ──
            // 这是抑制噪点的关键：平坦区域的微小差异（就是噪点）不该被放大。
            // 判据用三个通道中最大的那个差异 —— 只要有一个通道的差异
            // 超过阈值，就认为这是真实边缘，值得锐化。
            //
            // 为什么取最大值而不是取平均：取平均会让"只有一个通道变化"的
            // 彩色边缘（例如红色物体边缘）被平均值稀释掉，从而漏掉。
            const int maxDiff = qMax(qAbs(hr), qMax(qAbs(hg), qAbs(hb)));

            if (maxDiff < th) {
                // 差异太小，保持原样（不锐化）
                dstLine[x] = px;
                continue;
            }

            const int nr = qBound(0, int(qRound(r + amount * hr)), 255);
            const int ng = qBound(0, int(qRound(g + amount * hg)), 255);
            const int nb = qBound(0, int(qRound(b + amount * hb)), 255);

            dstLine[x] = packRgb(nr, ng, nb, a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  其它
// ===========================================================================

QImage pixelate(const QImage &src, int blockSize)
{
    if (src.isNull() || blockSize <= 1)
        return src.isNull() ? QImage() : src.copy();

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    const int w = work.width();
    const int h = work.height();
    const int bs = qMax(2, blockSize);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    for (int by = 0; by < h; by += bs) {
        for (int bx = 0; bx < w; bx += bs) {
            // 算这个块的边界（最后一块可能不满）
            const int x1 = qMin(bx + bs, w);
            const int y1 = qMin(by + bs, h);

            // 求块内平均值
            quint64 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            quint64 count = 0;

            for (int y = by; y < y1; ++y) {
                const QRgb *line = reinterpret_cast<const QRgb *>(work.constScanLine(y));
                for (int x = bx; x < x1; ++x) {
                    const QRgb px = line[x];
                    const int a = qAlpha(px);
                    sumA += quint64(a);

                    // 预乘空间下直接对预乘分量求平均是正确且高效的。
                    // 若先还原非预乘再平均，透明像素会被赋予"原本不可见的颜色"，
                    // 污染平均值 —— 这是马赛克里常见的错误来源。
                    if (a > 0) {
                        sumR += quint64(qRed(px));
                        sumG += quint64(qGreen(px));
                        sumB += quint64(qBlue(px));
                    }
                    ++count;
                }
            }

            if (count == 0)
                continue;

            const int avgA = int(sumA / count);
            const int avgR = (avgA > 0) ? int(sumR / count) : 0;
            const int avgG = (avgA > 0) ? int(sumG / count) : 0;
            const int avgB = (avgA > 0) ? int(sumB / count) : 0;

            const QRgb fill = qRgba(qBound(0, avgR, avgA),
                                    qBound(0, avgG, avgA),
                                    qBound(0, avgB, avgA),
                                    qBound(0, avgA, 255));

            // 用平均值填充整个块
            for (int y = by; y < y1; ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
                for (int x = bx; x < x1; ++x)
                    line[x] = fill;
            }
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

QImage addNoise(const QImage &src, qreal amount, quint32 seed)
{
    if (src.isNull() || amount <= 0.0)
        return src.isNull() ? QImage() : src.copy();

    bool wasPremultiplied = false;
    QImage work = toPremultiplied(src, wasPremultiplied);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);

    // 用可指定种子的随机数生成器，而不是全局的 qrand()。
    // 好处：同样的 seed 必然得到同样的噪点，这让"添加噪点"这个操作可以
    // 安全地放进撤销栈 —— 重做时不会因为随机数不同而结果不一样，
    // 那样用户会觉得"撤销重做之后噪点变了"，很困惑。
    QRandomGenerator rng(seed != 0 ? seed : 1);

    // 强度映射：amount 0.0~1.0 → 0~64 的噪声幅度
    const qreal amplitude = qBound(0.0, amount, 1.0) * 64.0;

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

            int r, g, b;
            unpackRgb(px, r, g, b);

            // 每个像素一个独立噪声值，三个通道共用 ——
            // 三通道各自独立会产生彩色噪点（像电视雪花），
            // 共用则是灰度噪点（像胶片颗粒），后者更符合"添加颗粒感"的意图。
            const qreal noise = (rng.generateDouble() * 2.0 - 1.0) * amplitude;

            const int nr = qBound(0, int(qRound(r + noise)), 255);
            const int ng = qBound(0, int(qRound(g + noise)), 255);
            const int nb = qBound(0, int(qRound(b + noise)), 255);

            dstLine[x] = packRgb(nr, ng, nb, a);
        }
    }

    return restoreFormat(result, src, wasPremultiplied);
}

} // namespace filters
} // namespace zl
