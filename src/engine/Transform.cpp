#include "engine/Transform.h"
#include "engine/Resample.h"
#include "compat/QtCompat.h"

#include <QPainter>
#include <QtMath>
#include <cstring>

namespace zl {
namespace transform {

namespace {

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

} // namespace

// ===========================================================================
//  移动
// ===========================================================================

QImage translate(const QImage &src, const QPoint &delta)
{
    if (src.isNull())
        return QImage();
    if (delta.isNull())
        return src.copy();

    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // 目标尺寸不变，空白处透明。
    // 注意必须显式 fill —— QImage 构造时不会清零内存。
    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    const int w = work.width();
    const int h = work.height();

    // 逐行 memcpy 而不是逐像素拷贝。
    // 一行 4000 像素用 memcpy 比逐像素循环快 5~10 倍，
    // 而且这只是平移，不需要任何插值运算，没必要逐像素处理。
    for (int y = 0; y < h; ++y) {
        const int srcY = y - delta.y();
        if (srcY < 0 || srcY >= h)
            continue;

        const uchar *srcLine = work.constScanLine(srcY);
        uchar *dstLine = result.scanLine(y);

        // 计算实际可以拷贝的水平区间（矩形求交）
        const int copyLeft  = qMax(0, delta.x());
        const int copyRight = qMin(w, w + delta.x());
        const int copyWidth = copyRight - copyLeft;

        if (copyWidth <= 0)
            continue;

        const int srcX = copyLeft - delta.x();

        std::memcpy(dstLine + size_t(copyLeft) * 4,
                    srcLine + size_t(srcX) * 4,
                    size_t(copyWidth) * 4);
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  缩放
// ===========================================================================

QImage scaleAbout(const QImage &src, qreal factor,
                  const QPointF &anchor, ResampleMethod method)
{
    return scaleAbout(src, factor, factor, anchor, method);
}

QImage scaleAbout(const QImage &src, qreal factorX, qreal factorY,
                  const QPointF &anchor, ResampleMethod method)
{
    if (src.isNull())
        return QImage();

    // 退化情况：倍率为 1 或非正数时直接返回。
    // 非正数会导致尺寸算出 0 或负数，是必须防的边界。
    if (factorX <= 0.0 || factorY <= 0.0)
        return src.copy();
    if (qFuzzyCompare(factorX, 1.0) && qFuzzyCompare(factorY, 1.0))
        return src.copy();

    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // ── 第 1 步：用高质量重采样缩放 ──
    const int newW = qMax(1, int(qRound(work.width()  * factorX)));
    const int newH = qMax(1, int(qRound(work.height() * factorY)));

    QImage scaled = resample::scale(work, QSize(newW, newH), method);

    if (scaled.isNull())
        return src.copy();

    // ── 第 2 步：把缩放结果按锚点摆回原尺寸画布 ──
    //
    // 锚点的语义：缩放前后，锚点位置对应的图像点保持不动。
    // 对归一化锚点 (ax, ay)：
    //   缩放前的锚点像素位置 = (ax * oldW, ay * oldH)
    //   缩放后的锚点像素位置 = (ax * newW, ay * newH)
    //   要让它们在新画布中对齐，偏移量为
    //     dx = ax*oldW - ax*newW
    const qreal ax = anchor.x();
    const qreal ay = anchor.y();

    const int dx = int(qRound(ax * work.width()  - ax * newW));
    const int dy = int(qRound(ay * work.height() - ay * newH));

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(dx, dy, scaled);
        painter.end();
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  旋转
// ===========================================================================

QImage rotateAboutCenter(const QImage &src, qreal angleDeg, ResampleMethod method)
{
    return rotateAbout(src, angleDeg, QPointF(0.5, 0.5), method);
}

QImage rotateAbout(const QImage &src, qreal angleDeg, const QPointF &anchor,
                   ResampleMethod method)
{
    // ★ method 目前故意未使用，原因见下面"任意角度"那一段的说明 ★
    //
    //   任意角度旋转的"源→目标"映射不能沿坐标轴分离，所以走不了本工程
    //   自实现的 Resample（它靠两遍一维卷积实现），只能用 QPainter 的
    //   平滑变换。参数保留是为了统一 rotateAboutCenter / rotateAbout 的
    //   签名 —— 将来若实现了真正的任意角度重采样，这个参数就直接生效。
    //
    //   显式标记而不是删掉参数：删参数会改变公开接口，且会让调用方
    //   （Document::rotateImageArbitrary）也跟着改一遍。
    Q_UNUSED(method);

    if (src.isNull())
        return QImage();

    // 角度归零时直接返回，避免无谓的重采样（旋转 0 度也会因插值而模糊）
    const qreal normalized = normalizeAngle(angleDeg);
    if (qFuzzyIsNull(normalized) || qFuzzyCompare(normalized, 360.0))
        return src.copy();

    // 90° 倍数走无损路径。
    // 注意这里用的是"旋转角度是否接近 90 的倍数"判断（带容差），
    // 因为用户可能在对话框里输入 89.9999，不该因此得到一张模糊的图。
    int quarterTurns = 0;
    if (isRightAngle(angleDeg, &quarterTurns)) {
        if (quarterTurns == 0)
            return src.copy();

        // 无损旋转后再按锚点摆回（90° 旋转会改变长宽，需要重新定位）
        const QImage rotated = rotateQuarterTurns(src, quarterTurns);
        if (rotated.isNull())
            return src.copy();

        // 旋转后尺寸不同，需要按锚点对齐
        // 锚点语义同上：旋转前后锚点对应的图像点保持不动
        const int dx = int(qRound(anchor.x() * src.width()
                                  - anchor.x() * rotated.width()));
        const int dy = int(qRound(anchor.y() * src.height()
                                  - anchor.y() * rotated.height()));

        QImage result(src.size(), QImage::Format_ARGB32_Premultiplied);
        result.fill(Qt::transparent);
        {
            QPainter painter(&result);
            painter.drawImage(dx, dy, rotated);
            painter.end();
        }

        if (src.format() == QImage::Format_ARGB32_Premultiplied)
            return result;

        QImage restored = result.convertToFormat(src.format());
        return restored;
    }

    // ── 任意角度：用 QPainter 的平滑变换 ──
    //
    // 这里没有走 Resample 的原因：任意角度旋转的"源→目标"映射不是沿坐标轴
    // 可分离的，无法用两遍一维卷积实现。QPainter 的 SmoothPixmapTransform
    // 内部用的是双线性，质量略低于双三次，但旋转的插值误差本身就被角度
    // 摊薄了（45° 时最差），实际观感可以接受。
    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    {
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        // 围绕锚点旋转：先把锚点移到原点，旋转，再移回去。
        // QTransform 的链式乘法是从左到右应用的。
        const qreal px = anchor.x() * work.width();
        const qreal py = anchor.y() * work.height();

        QTransform tf;
        tf.translate(px, py);
        tf.rotate(angleDeg);
        tf.translate(-px, -py);

        painter.setTransform(tf);
        painter.drawImage(0, 0, work);
        painter.end();
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  翻转
// ===========================================================================

QImage flip(const QImage &src, FlipDirection dir)
{
    if (src.isNull())
        return QImage();

    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // 走 compat 层，屏蔽 Qt5 的 mirrored() 与 Qt6 的 flipped() 差异
    const bool horizontal = (dir == FlipDirection::Horizontal);
    const bool vertical   = (dir == FlipDirection::Vertical);

    QImage result = zl::mirrorImage(work, horizontal, vertical);

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  任意仿射变换
// ===========================================================================

QImage applyTransform(const QImage &src, const QTransform &m, ResampleMethod method)
{
    if (src.isNull())
        return QImage();

    // 判断是否需要高质量重采样：
    // 如果矩阵不含旋转/错切（m12 与 m21 为 0），可以用 Resample 的两遍卷积。
    // 否则退回 QPainter。
    const bool axisAligned = qFuzzyIsNull(m.m12()) && qFuzzyIsNull(m.m21());

    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage result(work.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);

    if (axisAligned && method != ResampleMethod::Nearest) {
        // 轴向对齐：先按矩阵的缩放分量重采样，再按平移分量摆放。
        // 这样能获得 Resample 的高质量插值。
        const qreal sx = qAbs(m.m11());
        const qreal sy = qAbs(m.m22());

        QImage staged = work;
        if (!qFuzzyCompare(sx, 1.0) || !qFuzzyCompare(sy, 1.0)) {
            const QSize sz(qMax(1, int(qRound(work.width()  * sx))),
                           qMax(1, int(qRound(work.height() * sy))));
            staged = resample::scale(work, sz, method);
        }

        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(int(std::floor(m.dx())), int(std::floor(m.dy())), staged);
        painter.end();
    } else {
        // 含旋转/错切：交给 QPainter
        QPainter painter(&result);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              method != ResampleMethod::Nearest);
        painter.setTransform(m);
        painter.drawImage(0, 0, work);
        painter.end();
    }

    return restoreFormat(result, src, wasPremultiplied);
}

// ===========================================================================
//  内容包围盒
// ===========================================================================

QRect contentBounds(const QImage &src)
{
    if (src.isNull())
        return QRect();

    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    const int w = work.width();
    const int h = work.height();

    int minX = w, minY = h, maxX = -1, maxY = -1;

    // ── 逐行扫描 ──
    //
    // 优化思路：对每一行先用 32 位对齐的方式快速判断"这一行是否全透明"。
    // 全透明的行（alpha 全为 0）可以整行跳过，实际照片的上下边缘经常
    // 有大量空行（例如竖图放在横画布里），跳过它们能省下可观的时间。
    //
    // 这里用 QRgb 逐元素判断而不是逐字节，因为预乘格式下"透明"等价于
    // 整个 32 位字为 0（alpha=0 时预乘的 RGB 也必然为 0），
    // 因此可以用一次 32 位比较代替四次字节比较。
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(work.constScanLine(y));

        int lineMinX = -1;
        int lineMaxX = -1;

        for (int x = 0; x < w; ++x) {
            if (qAlpha(line[x]) != 0) {
                if (lineMinX < 0)
                    lineMinX = x;
                lineMaxX = x;
            }
        }

        if (lineMinX < 0)
            continue;   // 整行全透明，跳过

        if (lineMinX < minX) minX = lineMinX;
        if (lineMaxX > maxX) maxX = lineMaxX;

        if (y < minY) minY = y;
        maxY = y;
    }

    if (maxX < 0 || maxY < 0)
        return QRect();

    // QRect 的 right()/bottom() 是**闭区间**，所以宽高用 (max - min + 1)。
    // 这是 Qt 的经典陷阱：用 QRect(minX, minY, maxX - minX, maxY - minY)
    // 会少算一列一行，导致内容边缘被切掉 1 像素。
    return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

QImage trimToContent(const QImage &src, int padding)
{
    if (src.isNull())
        return QImage();

    const QRect bounds = contentBounds(src);

    // 全透明图像：没有内容可裁，返回原图（而不是空图）。
    // 调用方拿到空图会以为是失败，而实际上"全透明"是一个合法状态。
    if (bounds.isEmpty())
        return src.copy();

    const int pad = qMax(0, padding);

    // 加上 padding 后再与图像范围求交，防止越界
    QRect trimmed = bounds.adjusted(-pad, -pad, pad, pad)
                          .intersected(src.rect());

    if (trimmed.isEmpty())
        return src.copy();

    // QImage::copy(rect) 返回独立副本，是安全的
    return src.copy(trimmed);
}

} // namespace transform
} // namespace zl
