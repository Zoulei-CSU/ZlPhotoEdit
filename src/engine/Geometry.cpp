#include "engine/Geometry.h"
#include "engine/Resample.h"
#include "compat/QtCompat.h"

#include <QPainter>
#include <QtMath>
#include <cmath>

namespace zl {

// ===========================================================================
//  锚点
// ===========================================================================

QPoint anchorOffset(Anchor anchor, const QSize &oldSize, const QSize &newSize)
{
    // 尺寸差（新 - 旧）。为正表示画布变大了。
    const int dw = newSize.width()  - oldSize.width();
    const int dh = newSize.height() - oldSize.height();

    switch (anchor) {
    case Anchor::TopLeft:      return QPoint(0,   0);
    case Anchor::TopCenter:    return QPoint(dw / 2, 0);
    case Anchor::TopRight:     return QPoint(dw, 0);

    case Anchor::MiddleLeft:   return QPoint(0,   dh / 2);
    case Anchor::Center:       return QPoint(dw / 2, dh / 2);
    case Anchor::MiddleRight:  return QPoint(dw, dh / 2);

    case Anchor::BottomLeft:   return QPoint(0,   dh);
    case Anchor::BottomCenter: return QPoint(dw / 2, dh);
    case Anchor::BottomRight:  return QPoint(dw, dh);
    }
    return QPoint(0, 0);
}

QPoint canvasResizeDelta(const QSize &oldSize, const QSize &newSize, Anchor anchor)
{
    return anchorOffset(anchor, oldSize, newSize);
}

// ===========================================================================
//  百分比 ↔ 像素
// ===========================================================================
//
//  "尺寸类对话框的单位换算"与"锚点偏移"是同一类东西：一段被多处依赖的
//  纯算术。放在引擎层，就能被离线自检直接覆盖（geometry_test 第 12 组），
//  而不必去启动一个 QApplication 才能验证 qRound 有没有写错。

namespace {

// 四舍五入的整数除法 x / d，.5 远离 0。
//   C++ 的整数除法是向零截断，负数要先取绝对值再补符号，否则
//   (-231 + 50) / 100 会得到 -1，而正确答案是 -2。
int roundedDiv(qint64 x, qint64 d)
{
    if (x >= 0)
        return int((x + d / 2) / d);
    return -int((-x + d / 2) / d);
}

} // namespace

int percentToPixels(int percent, int original)
{
    if (original <= 0)
        return 0;

    // d = 100：percent 本身就是百分数，直接除以 100 即可收工。
    return roundedDiv(qint64(percent) * qint64(original), 100);
}

int pixelsToPercent(int pixels, int original)
{
    if (original <= 0)
        return 0;

    return roundedDiv(qint64(pixels) * 100, original);
}

// ===========================================================================
//  智能适配
// ===========================================================================

QPoint computeSmartOffset(const QRect &contentRect,
                          const QSize &oldSize,
                          const QSize &newSize,
                          Anchor anchor)
{
    // 基础偏移：按用户选的锚点
    const QPoint baseOffset = anchorOffset(anchor, oldSize, newSize);

    // 内容为空（全透明图像）时没什么可保护的，直接用锚点结果
    if (contentRect.isEmpty())
        return baseOffset;

    // 内容在新画布中的包围盒（相对旧画布的坐标 + 基础偏移）
    const QRect contentInNew = contentRect.translated(baseOffset);

    const bool fitsHorizontally = contentInNew.left() >= 0
                               && contentInNew.right() < newSize.width();
    const bool fitsVertically   = contentInNew.top() >= 0
                               && contentInNew.bottom() < newSize.height();

    // ① 内容完整可见 → 保持锚点的语义，不做任何调整。
    //    这是最理想的情况，也是画布放大时的必然结果。
    if (fitsHorizontally && fitsVertically)
        return baseOffset;

    // ② 内容比新画布还大 → 无论怎么移都装不下，退化为按锚点裁切。
    //    此时再"智能"也没有意义，不如遵循用户明确选择的锚点。
    const bool tooBigHorizontally = contentRect.width()  > newSize.width();
    const bool tooBigVertically   = contentRect.height() > newSize.height();

    if (tooBigHorizontally && !fitsHorizontally)
        return baseOffset;
    if (tooBigVertically && !fitsVertically)
        return baseOffset;

    // ③ 装得下但被裁到了 → 在合法范围内求最接近 baseOffset 的偏移。
    //
    //    合法的水平偏移范围：让内容完整落在 [0, newWidth) 内
    //      最小偏移：-contentRect.left()        （内容左边贴到 0）
    //      最大偏移：newWidth - 1 - contentRect.right()  （内容右边贴到 newWidth-1）
    //
    //    然后把 baseOffset 钳制进这个范围。因为是取"最接近"的值，
    //    所以钳制（clamp）就是正解 —— 不需要更复杂的搜索。
    QPoint offset = baseOffset;

    if (!fitsHorizontally) {
        const int minOffsetX = -contentRect.left();
        const int maxOffsetX = newSize.width() - 1 - contentRect.right();
        // 确保 min <= max（前面已经排除了"内容比画布大"的情况）
        if (minOffsetX <= maxOffsetX)
            offset.setX(qBound(minOffsetX, baseOffset.x(), maxOffsetX));
    }

    if (!fitsVertically) {
        const int minOffsetY = -contentRect.top();
        const int maxOffsetY = newSize.height() - 1 - contentRect.bottom();
        if (minOffsetY <= maxOffsetY)
            offset.setY(qBound(minOffsetY, baseOffset.y(), maxOffsetY));
    }

    return offset;
}

QPoint canvasResizeOffset(const QRect &contentRect,
                          const QSize &oldSize,
                          const QSize &newSize,
                          Anchor anchor,
                          bool smart)
{
    // ★ 智能适配只在"缩小时"启用 ★
    //   放大时按锚点放置本来就是正确的（内容一定完整可见），
    //   再跑一遍智能适配纯属浪费，而且会让"锚点"这个用户明确的选择
    //   在某些边角情形下被悄悄改写。
    //
    //   注意条件是"任一方向变小"而不是"两个方向都变小"：
    //   宽变小、高变大（例如 4000×3000 → 2000×4000）同样会横裁，
    //   这种情况必须走智能适配。
    const bool shrinking = (newSize.width()  < oldSize.width())
                        || (newSize.height() < oldSize.height());

    if (!smart || !shrinking)
        return anchorOffset(anchor, oldSize, newSize);

    // ★ 内容包围盒为空（全透明图像）时退化为"整个旧画布" ★
    //
    //   这与改动前的 Document::resizeCanvas 行为完全一致 —— 收敛策略时
    //   必须保证既有行为不变，否则"顺手统一一下"就会变成一个静默的
    //   行为回归，而且是那种几个月后才有人发现的回归。
    //
    //   语义上也说得通：一幅全透明的图没有"内容"可保护，但用户对
    //   画布边界的心理模型仍在，按旧画布范围保持可见更符合直觉。
    //   （实际上两种情况都没有可见像素，差别只落在选区的同步平移上。）
    const QRect content = contentRect.isEmpty()
                        ? QRect(QPoint(0, 0), oldSize)
                        : contentRect;

    return computeSmartOffset(content, oldSize, newSize, anchor);
}

// ===========================================================================
//  「打开为图层」的尺寸适配
// ===========================================================================
//
//  两个函数都很短，但没有合并成"给两个尺寸返回 (新画布, 放置点)"的一个函数。
//  理由：调用方需要分开用它们 —— 对话框要拿"扩大后的尺寸"给用户看，
//  而放置点要等用户真的选了"扩大"才算。合成一个会让调用方拿到一堆
//  用不上的中间值，也说不清"用户还没决定"这个状态。
//  详细理由见头文件。

QSize unionSize(const QSize &a, const QSize &b)
{
    // ★ 刻意**不做**"空尺寸"的特判 ★
    //
    //  起先这里有 `if (!a.isValid() || a.isEmpty()) return b;` 两行。
    //  逐种输入推演之后发现：它在**任何**输入下都算不出与纯逐维取大
    //  不同的结果 —— QSize() 的默认值是 (-1, -1)（不是 (0,0)），
    //  参与 qMax 既不会把结果拉成 0 也不会拉成负数；而 (0,50) 与 (50,0)
    //  这种退化输入在真实调用点根本不会出现（画布 ≥ 1×1、图片来自
    //  已成功解码的 QImage）。
    //  换句话说那两行是**死代码**。死代码最坏的地方不是浪费，
    //  而是它会让人以为"空尺寸已经处理过了"，于是在它上面加断言、
    //  写注释 —— 全是空的（这一版的自检就差点这么干）。
    return QSize(qMax(a.width(),  b.width()),
                 qMax(a.height(), b.height()));
}

QPoint centeredOffset(const QSize &canvasSize, const QSize &itemSize)
{
    // 整数除法会向下取整 —— 差值为奇数时内容偏左上 0.5px。
    // 这是刻意的：两个方向用同一条规则，不会出现"横向偏右、纵向偏上"
    // 这种看着歪的结果。
    return QPoint((canvasSize.width()  - itemSize.width())  / 2,
                  (canvasSize.height() - itemSize.height()) / 2);
}

// ===========================================================================
//  旋转
// ===========================================================================

bool isRightAngle(qreal angleDeg, int *quarterTurnsOut)
{
    // 先归一化到 [0, 360)
    const qreal normalized = normalizeAngle(angleDeg);

    // 容差 0.01 度。理由见头文件说明：
    // 太小会被浮点误差骗到，太大可能把用户真的想转 89.9 度当成 90 度处理。
    const qreal tolerance = 0.01;

    const qreal candidates[4] = { 0.0, 90.0, 180.0, 270.0 };
    for (int i = 0; i < 4; ++i) {
        qreal diff = qAbs(normalized - candidates[i]);
        // 处理环绕：359.995 与 0 的差应该算 0.005 而不是 359.995
        if (diff > 180.0)
            diff = 360.0 - diff;

        if (diff <= tolerance) {
            if (quarterTurnsOut)
                *quarterTurnsOut = i;
            return true;
        }
    }

    return false;
}

qreal normalizeAngle(qreal angleDeg)
{
    qreal a = std::fmod(angleDeg, 360.0);
    if (a < 0.0)
        a += 360.0;
    return a;
}

// ===========================================================================
//  变换应用
// ===========================================================================

QImage applyGeometry(const QImage &src, const GeometryParams &params)
{
    if (src.isNull())
        return QImage();
    if (!params.newCanvasSize.isValid() || params.newCanvasSize.isEmpty())
        return QImage();

    // 统一到预乘格式。几何变换会在预乘空间做插值（对重采样是正确的），
    // 若原图不是预乘格式，最后再转回去。
    const bool wasPremultiplied =
        (src.format() == QImage::Format_ARGB32_Premultiplied);

    QImage work = wasPremultiplied
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // ── 准备目标画布 ──
    QImage canvas(params.newCanvasSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(params.fillColor);

    // ── 构造绘制变换 ──
    // 顺序很重要：先应用内容变换，再平移到目标位置。
    // QTransform 的乘法是"后乘作用于先"，也就是说
    //   final = params.transform * translate
    // 表示"先做 params.transform，再做 translate"。
    QTransform full = params.transform;
    full *= QTransform::fromTranslate(params.contentOffset.x(),
                                      params.contentOffset.y());

    // ── 判断是否需要插值 ──
    // 如果变换是纯粹的整数平移 + 90° 倍数旋转，理论上不插值也正确。
    // 但这里无法在不解析矩阵的情况下可靠判断，所以由调用方通过
    // interpolation 参数控制。90° 旋转应当走 rotateQuarterTurns()。
    const bool needsInterpolation =
        (params.interpolation != ResampleMethod::Nearest);

    QImage result;

    if (!needsInterpolation) {
        // 邻近取样：用 QPainter 的最近邻变换，速度快且保证像素精确
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setTransform(full);
        painter.drawImage(0, 0, work);
        painter.end();
        result = canvas;
    } else {
        // ── 需要高质量插值 ──
        //
        // 关键决策：不用 QPainter 的 SmoothPixmapTransform 做 bicubic。
        //
        // QPainter 的平滑变换在 Qt5 里通常退化为双线性，无法保证是双三次。
        // 既然我们已经实现了 Resample，就应该用它 —— 否则"自写 bicubic"
        // 这个需求就落空了，用户看到的质量和 Qt 自带的一模一样。
        //
        // 做法：先用 QPainter 找出变换后内容的包围盒，把图像变换到紧凑的
        // 临时画布上（这一步允许用 QPainter，因为它只负责"摆位置"），
        // 然后…… 其实更简单可靠的做法是：把纯平移和纯旋转/缩放分开处理。

        // 简化路径：如果变换矩阵只有平移成分（a=1,b=0,c=0,d=1），
        // 直接 blit，无需插值 —— 这覆盖了"画布大小调整"的全部场景，
        // 而画布大小调整恰恰是最高频的几何操作。
        const bool pureTranslate = qFuzzyCompare(full.m11(), 1.0)
                                && qFuzzyIsNull(full.m12())
                                && qFuzzyIsNull(full.m21())
                                && qFuzzyCompare(full.m22(), 1.0);

        if (pureTranslate) {
            QPainter painter(&canvas);
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.drawImage(int(std::floor(full.dx())),
                              int(std::floor(full.dy())),
                              work);
            painter.end();
            result = canvas;
        } else {
            // 一般情况：先按缩放比做一次高质量重采样，再旋转。
            //
            // 这个分解的正确性依赖于"缩放与旋转可交换（在各自轴向独立时）"。
            // 对均匀缩放（sx == sy）这是严格成立的；对非均匀缩放会有
            // 轻微误差，但几何变换对话框里的缩放始终是均匀的。
            const qreal sx = qSqrt(full.m11() * full.m11() + full.m12() * full.m12());
            const qreal sy = qSqrt(full.m21() * full.m21() + full.m22() * full.m22());

            QImage staged = work;

            const bool needsScale = !qFuzzyCompare(sx, 1.0) || !qFuzzyCompare(sy, 1.0);
            if (needsScale) {
                const QSize scaledSize(qMax(1, int(qRound(work.width()  * sx))),
                                       qMax(1, int(qRound(work.height() * sy))));
                staged = resample::scale(staged, scaledSize, params.interpolation);
            }

            // 提取旋转角度。atan2 在矩阵为纯缩放时返回 0。
            const qreal angleDeg = qRadiansToDegrees(qAtan2(full.m12(), full.m11()));

            // ★★ 平移只能取 full 的，绝不能再加一次 params.contentOffset ★★
            //
            //  full = params.transform * fromTranslate(contentOffset)，
            //  也就是说 full **已经包含了** contentOffset 那次平移
            //  （它的 dx/dy 是"旋转耦合之后"的最终平移量）。
            //
            //  曾经这里写的是 contentOffset + full.dx()，等于把平移量加了
            //  两遍：内容会朝偏移方向的斜下方多跑约一倍的距离，然后被画布
            //  裁掉一角，而四角的填充区一大一小。
            //
            //  ★ 为什么它能潜伏那么久 ★
            //    · 90° 倍数根本不走这里（走 rotateQuarterTurns 的无损路径）
            //    · 纯平移（画布大小调整）走的是上面的 pureTranslate 分支
            //    · 所以只有"任意角度旋转"会踩到 —— 而它直到本轮才有 UI
            //  这也正是"新功能第一次把一个老 bug 暴露出来"的典型样子。
            QTransform rotation;
            rotation.rotate(angleDeg);
            rotation *= QTransform::fromTranslate(full.dx(), full.dy());

            QPainter painter(&canvas);
            // 让 QPainter 处理旋转。旋转的插值误差难以完全避免，
            // 但前面的缩放已经用高质量算法做过了，这一步只是转动，
            // 对质量的损耗可以接受。
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setTransform(rotation);
            painter.drawImage(0, 0, staged);
            painter.end();
            result = canvas;
        }
    }

    if (!wasPremultiplied && src.format() == QImage::Format_ARGB32)
        return result.convertToFormat(QImage::Format_ARGB32);

    return result;
}

QImage rotateImage(const QImage &src, qreal angleDeg, ResampleMethod method,
                   const QColor &fillColor)
{
    if (src.isNull())
        return QImage();

    // 先检查是不是 90° 倍数 —— 若是则走无损路径。
    // 这个检查很关键：用户输入 90 时（哪怕因为浮点误差变成 89.99999），
    // 都应该得到逐像素相同的无损结果，而不是被模糊一次。
    //
    // ★ 这条路径下 fillColor 无处安放 ★
    //   90° 倍数的旋转结果与画布**完全等大**（宽高互换或不变），
    //   包围盒里没有空出来的像素，填充色自然无从体现。
    //   这不是遗漏，而是"无损"这个性质本身的推论。
    int quarterTurns = 0;
    if (isRightAngle(angleDeg, &quarterTurns)) {
        if (quarterTurns == 0)
            return src.copy();
        return rotateQuarterTurns(src, quarterTurns);
    }

    // 任意角度：计算旋转后的包围盒尺寸（不裁切）
    const QSize newSize = rotatedBoundsSize(src.size(), angleDeg);

    // ── 让旋转后的内容居中（这一步有个极易写错的细节）──
    //
    //  ★ QTransform::rotate() 是**围绕原点**旋转的 ★
    //    所以内容的中心 (w/2, h/2) 旋转之后自己也跑到了别处。想让内容
    //    落回新画布中心，必须用**旋转之后的内容中心**去算偏移：
    //
    //        正确：offset = newCenter − rotate(oldCenter)
    //        错误：offset = newCenter − oldCenter
    //
    //    写成后者时，90° 倍数恰好看不出问题（中心绕原点转 90° 后仍与
    //    画布中心保持对称），所以这条错误能长期潜伏；一旦角度是别的值，
    //    内容就会整体偏出画布被裁掉一角，且四个角的填充区大小不均 ——
    //    看起来像"旋转轴心不对"，光读代码很难定位。
    //    （geometry_test 的 [13] 组用"内容包围盒必须铺满整张画布"钉住了它。）
    QTransform rotation;
    rotation.rotate(angleDeg);

    const QPointF oldCenter(src.width()  / 2.0, src.height() / 2.0);
    const QPointF newCenter(newSize.width() / 2.0, newSize.height() / 2.0);
    const QPointF offset = newCenter - rotation.map(oldCenter);

    GeometryParams params;
    params.transform = rotation;
    params.newCanvasSize = newSize;
    params.interpolation = method;
    params.fillColor = fillColor;
    params.contentOffset = QPoint(int(qRound(offset.x())), int(qRound(offset.y())));

    // 注意：这里最终走的是 applyGeometry 的"一般情况"分支，
    // 即先用 Resample 做缩放（此处 sx=sy=1，会跳过），再让 QPainter 旋转。
    return applyGeometry(src, params);
}

QImage rotateQuarterTurns(const QImage &src, int quarterTurns)
{
    if (src.isNull())
        return QImage();

    // 归一化到 [0, 3]
    quarterTurns = ((quarterTurns % 4) + 4) % 4;

    if (quarterTurns == 0)
        return src.copy();

    // ── 无损 90° 倍数旋转 ──
    //
    // QImage 没有直接提供 90° 旋转，但 QTransform 的 rotate(90) 在
    // Qt 内部会识别为"正交变换"，走的是逐像素搬运而不是插值。
    // 不过为了百分之百保证无损，这里手写一遍更可靠 ——
    // 而且手写版本能明确表达"这是无损的"这个意图，
    // 将来有人改动 QTransform 的用法时不会无意中破坏它。
    //
    // 验证方法（写测试时用）：旋转 90° 四次，结果应与原图逐像素完全相同。
    const int w = src.width();
    const int h = src.height();

    QImage work = (src.format() == QImage::Format_ARGB32_Premultiplied)
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage result;

    if (quarterTurns == 2) {
        // 180°：尺寸不变，逐像素对角镜像
        result = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < h; ++y) {
            const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
            QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(h - 1 - y));
            for (int x = 0; x < w; ++x)
                dstLine[w - 1 - x] = srcLine[x];
        }
    } else {
        // 90° 或 270°：宽高互换
        result = QImage(h, w, QImage::Format_ARGB32_Premultiplied);

        if (quarterTurns == 1) {
            // 顺时针 90°：源 (x, y) → 目标 (h-1-y, x)
            for (int y = 0; y < h; ++y) {
                const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    // 目标列固定，行递减 —— 逐行写入会让缓存局部性变差，
                    // 但换来的是代码清晰。90° 旋转不是性能瓶颈，不必优化。
                    QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(x));
                    dstLine[h - 1 - y] = srcLine[x];
                }
            }
        } else {
            // 逆时针 90°（即顺时针 270°）：源 (x, y) → 目标 (y, w-1-x)
            for (int y = 0; y < h; ++y) {
                const QRgb *srcLine = reinterpret_cast<const QRgb *>(work.constScanLine(y));
                for (int x = 0; x < w; ++x) {
                    QRgb *dstLine = reinterpret_cast<QRgb *>(result.scanLine(w - 1 - x));
                    dstLine[y] = srcLine[x];
                }
            }
        }
    }

    if (src.format() != QImage::Format_ARGB32_Premultiplied)
        return result.convertToFormat(src.format());

    return result;
}

QImage flipImage(const QImage &src, FlipDirection dir)
{
    if (src.isNull())
        return QImage();

    // Qt5 用 mirrored()，Qt6 改名为 flipped()。
    // 差异封装在 compat/QtCompat.h 里，这里通过 zl::mirrorImage 调用。
    bool horizontal = (dir == FlipDirection::Horizontal);
    bool vertical   = (dir == FlipDirection::Vertical);

    QImage work = (src.format() == QImage::Format_ARGB32_Premultiplied)
                      ? src
                      : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    QImage result = zl::mirrorImage(work, horizontal, vertical);

    if (src.format() != QImage::Format_ARGB32_Premultiplied)
        return result.convertToFormat(src.format());

    return result;
}

QImage cropImage(const QImage &src, const QRect &rect)
{
    if (src.isNull())
        return QImage();

    // 把裁剪矩形钳制到图像范围内。用户可能拖出了边界（尤其是视觉上贴着边
    // 拖动时），不钳制会得到一张尺寸不符的图像。
    const QRect valid = rect.intersected(src.rect());
    if (valid.isEmpty())
        return QImage();

    // QImage::copy(rect) 返回的是独立副本 —— 这一点非常重要，
    // 因为它不会与源图共享内存，可以直接安全地修改。
    return src.copy(valid);
}

// ===========================================================================
//  尺寸计算
// ===========================================================================

QSize rotatedBoundsSize(const QSize &size, qreal angleDeg)
{
    if (!size.isValid() || size.isEmpty())
        return QSize();

    // ★ 直角走整数路径，不碰三角函数 ★
    //
    //   浮点里的 cos(90°) 不是 0 而是 6.1e-17。于是
    //       W' = ceil(100 · 6.1e-17 + 50 · 1) = ceil(50.000000000000006) = 51
    //   白白多出一个像素。"向上取整"这个策略本身是对的（宁可多一个
    //   透明像素也不能裁掉内容），它只是对"结果恰好是整数"过于敏感。
    //
    //   直角是能精确表达的，就不该让浮点噪声参与：奇数个 90° 互换宽高，
    //   偶数个原样返回。这样 rotatedBoundsSize(100×50, 90°) 得到精确的
    //   50×100，与 Document::rotateImage90 的无损路径结果一致。
    int quarterTurns = 0;
    if (isRightAngle(angleDeg, &quarterTurns)) {
        return (quarterTurns % 2 == 1) ? QSize(size.height(), size.width())
                                       : size;
    }

    // 把角度归一化到 [0, 90] 区间来简化计算：
    // 旋转后的包围盒尺寸在四个象限里是对称的，
    // 90+a 与 90-a 的结果相同，180 与 0 相同。
    qreal a = normalizeAngle(angleDeg);
    if (a > 180.0)
        a -= 180.0;
    if (a > 90.0)
        a = 180.0 - a;

    const qreal rad = qDegreesToRadians(a);
    const qreal cosA = qAbs(qCos(rad));
    const qreal sinA = qAbs(qSin(rad));

    const qreal w = size.width();
    const qreal h = size.height();

    // 旋转后的包围盒：
    //   W' = w·|cos| + h·|sin|
    //   H' = w·|sin| + h·|cos|
    //
    // 向上取整而不是四舍五入 —— 宁可多出一个透明像素，也不能因为少一个像素
    // 把内容切掉（尤其是边缘只有半个像素的淡色内容，裁掉就看得出缺口）。
    //
    // ★ 取整前先减掉一个极小的量 ★
    //   专治"真值恰好是整数、浮点却算成 N + 1e-16"的情形：
    //   ceil(N - 1e-9) = N（正确），ceil(N + 1e-16) = N+1（多一个像素）。
    //   1e-9 远小于任何有意义的亚像素差异，不会漏掉真正该多出的那一个像素；
    //   直角已在上面精确处理，这里的余量只服务于 30°、45° 这类中间角。
    constexpr qreal kEps = 1e-9;
    const int newW = int(std::ceil(w * cosA + h * sinA - kEps));
    const int newH = int(std::ceil(w * sinA + h * cosA - kEps));

    return QSize(qMax(1, newW), qMax(1, newH));
}

} // namespace zl
