#include "core/Selection.h"

#include "engine/Filters.h"   // featherGrayMask（羽化的实现搬到了 engine 层）

#include <QAtomicInteger>
#include <QPainter>
#include <QRegion>
#include <QVector>
#include <QtMath>

namespace zl {

// ===========================================================================
//  修订号的发号器
// ===========================================================================

namespace {

// ★ 必须是**全局**单调递增，不能是每个 Selection 各一个计数器 ★
//
//   下游（CanvasView）用"修订号变了没有"决定要不要重建选区轮廓。
//   如果号只在单个对象内部从 1 开始数，那么**撤销/重做把一份旧快照
//   赋回选区**时，"快照里的号"与"下游记着的号"完全可能相等，
//   而两者的蒙版内容并不相同 —— 判据会说"没变"，
//   画面上的蚂蚁线于是停在上一版选区上：不崩、不报错、只是画错。
//   全局发号器让"号相同 ⇒ 内容相同"这条性质恒成立。
quint64 nextRevision()
{
    static QAtomicInteger<quint64> counter(1);
    return counter.fetchAndAddRelaxed(1);
}

} // namespace

// ===========================================================================
//  内部辅助
// ===========================================================================

// 内容变更的唯一记账点。所有改到蒙版或状态标志的地方都必须走它。
void Selection::touch()
{
    m_boundsDirty = true;
    m_revision    = nextRevision();
}

void Selection::ensureMask(const QSize &size)
{
    if (!size.isValid() || size.isEmpty())
        return;

    if (m_mask.size() != size || m_mask.format() != QImage::Format_Grayscale8) {
        // 重建蒙版时保持内容对齐：新蒙版尺寸可能与旧的不同（画布变化），
        // 这里简单地全量重置，由调用方（Document::applyGeometryChange）
        // 在几何变化后决定如何处置选区 —— 它更清楚该保留还是清空。
        m_mask = QImage(size, QImage::Format_Grayscale8);
        m_mask.fill(0);
        touch();
    }
}

// ===========================================================================
//  状态查询
// ===========================================================================

QRect Selection::bounds() const
{
    if (!m_active || m_allSelected)
        return QRect();

    if (!m_boundsDirty)
        return m_cachedBounds;

    // 遍历蒙版求非零像素的包围盒。
    // 优化：逐行扫描，用 memchr 类似的思路先快速跳过全零行。
    // 这里用一个朴素的实现保证正确性，包围盒只在选区改变后计算一次，
    // 性能足够（4000×3000 约 2~3ms）。
    int minX = m_mask.width();
    int minY = m_mask.height();
    int maxX = -1;
    int maxY = -1;

    const int w = m_mask.width();
    const int h = m_mask.height();

    for (int y = 0; y < h; ++y) {
        const uchar *line = m_mask.constScanLine(y);

        // 快速判断整行是否全零：全零行可以直接跳过
        bool rowHasData = false;
        for (int x = 0; x < w; ++x) {
            if (line[x] != 0) {
                rowHasData = true;
                if (x < minX) minX = x;
                // 不必继续找 maxX —— 稍后统一处理，这里只找最左
                break;
            }
        }
        if (!rowHasData)
            continue;

        if (y < minY) minY = y;
        maxY = y;

        // 从右往左找最右的非零像素
        for (int x = w - 1; x >= 0; --x) {
            if (line[x] != 0) {
                if (x > maxX) maxX = x;
                break;
            }
        }
    }

    if (maxX < 0 || maxY < 0)
        m_cachedBounds = QRect();
    else
        m_cachedBounds = QRect(QPoint(minX, minY), QPoint(maxX, maxY));

    m_boundsDirty = false;
    return m_cachedBounds;
}

// ===========================================================================
//  尺寸准备
// ===========================================================================

void Selection::prepareCanvas(const QSize &size)
{
    if (!size.isValid() || size.isEmpty())
        return;

    // ── 情况 1：已经有尺寸正确的蒙版 ──
    //
    //   要么用户已经做过选区（★ 必须原样保留 ★），
    //   要么之前已经调用过本函数。两种情况都不该动内容。
    if (!m_mask.isNull()
        && m_mask.size() == size
        && m_mask.format() == QImage::Format_Grayscale8) {
        return;
    }

    // ── 情况 2：有一份蒙版，但尺寸对不上 ──
    //
    //   这说明"画布尺寸变了，但没人通知选区"。正常情况下这不该发生 ——
    //   所有会改画布尺寸的操作（缩放/画布/旋转/裁剪）都会调用
    //   Document::syncSelectionToCanvas()，由它按锚点平移选区内容，
    //   把选区正确地搬到新尺寸下。
    //
    //   走到这个分支意味着调用方漏了那一步。此时旧蒙版已经**没有任何
    //   可用的语义**（尺寸不对的蒙版在任何运算里都会被拒绝），
    //   与其留着一份注定失效的数据，不如明确退回"全选"这个安全默认值。
    //
    //   ★ 这里刻意不做"按左上对齐拷贝旧内容"的补救 ★
    //     因为没有办法知道该按什么规则对齐（锚点？居中？包围盒？），
    //     猜错的后果是"用户得到一个看起来有选区、但位置莫名其妙的结果"，
    //     比明确全选更糟 —— 后者至少是用户熟悉且可预期的状态。
    //     真要做对，只能在 Document 那一层做（它知道锚点），
    //     而那已经由 syncSelectionToCanvas() 实现了。
    //
    //   情况 1 与情况 2 都会落到下面的重建代码。
    m_mask = QImage(size, QImage::Format_Grayscale8);

    // ★ 填 255 而不是 0 ★ 理由见头文件注释：
    //   底色代表"当前处于全选状态"，Add/Subtract/Intersect 才有正确语义。
    m_mask.fill(255);

    touch();
    m_active      = false;   // 尺寸已变，旧选区作废
    m_allSelected = true;

    // ★ 为什么正常路径下不动这两个标志 ★
    //   全 255 等价于"没有选区限制"。硬把 m_active 置 true 会让
    //   所有工具都付出一次逐像素蒙版乘法的代价，而这个代价在
    //   "用户还没画选区就先去点了一下画布"的场景下纯属浪费。
    //   只有上面"尺寸对不上"的异常路径才需要显式归位。
}

// ===========================================================================
//  修改选区
// ===========================================================================

void Selection::clear()
{
    m_mask = QImage();
    m_active      = false;
    m_allSelected = true;
    touch();
}

void Selection::setFromPath(const QPainterPath &path, bool antialiased)
{
    // 通过 drawPath 把路径渲染到灰度蒙版上。
    // 这是最省事的做法：QPainter 会自己处理裁剪、变换、抗锯齿，
    // 我们只需把填充色设为白色、背景设黑。
    if (path.isEmpty()) {
        clear();
        return;
    }

    // 需要一个尺寸参考。若当前没有蒙版，调用方应先通过 setMask 或
    // 其它方式建立尺寸。这里若没有蒙版则无法工作，直接返回。
    if (m_mask.isNull())
        return;

    ensureMask(m_mask.size());

    // 重新分配一份空蒙版，再用 Replace 语义绘制
    QImage target(m_mask.size(), QImage::Format_Grayscale8);
    target.fill(0);

    QPainter painter(&target);
    painter.setRenderHint(QPainter::Antialiasing, antialiased);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, antialiased);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawPath(path);
    painter.end();

    m_mask  = target;
    m_active = true;
    m_allSelected = false;
    touch();
}

void Selection::setMask(const QImage &grayMask)
{
    if (grayMask.isNull()) {
        clear();
        return;
    }

    if (grayMask.format() != QImage::Format_Grayscale8)
        m_mask = grayMask.convertToFormat(QImage::Format_Grayscale8);
    else
        m_mask = grayMask.copy();

    m_active      = true;
    m_allSelected = false;
    touch();
}

void Selection::markModified()
{
    touch();
    if (!m_mask.isNull()) {
        m_active      = true;
        m_allSelected = false;
    }
}

// ===========================================================================
//  选区布尔运算
// ===========================================================================

void Selection::applyPath(const QPainterPath &path, Op op, bool antialiased)
{
    if (path.isEmpty())
        return;
    if (m_mask.isNull())
        return;

    // Op::Replace 是最常见的情况，走单独的快速路径
    if (op == Op::Replace) {
        setFromPath(path, antialiased);
        return;
    }

    // 其余三种运算：先把路径渲染成一份临时蒙版，再做逐像素布尔运算。
    // 不做成"直接合成到原蒙版"的原因是抗锯齿边缘的处理 ——
    // 直接在原蒙版上叠加会让边界处的灰阶值变得无法预测。
    QImage temp(m_mask.size(), QImage::Format_Grayscale8);
    temp.fill(0);
    {
        QPainter painter(&temp);
        painter.setRenderHint(QPainter::Antialiasing, antialiased);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawPath(path);
        painter.end();
    }

    applyMask(temp, op);
}

void Selection::applyMask(const QImage &otherMask, Op op)
{
    if (otherMask.isNull() || m_mask.isNull())
        return;
    if (otherMask.size() != m_mask.size())
        return;

    const QImage src = (otherMask.format() == QImage::Format_Grayscale8)
                           ? otherMask
                           : otherMask.convertToFormat(QImage::Format_Grayscale8);

    const int w = m_mask.width();
    const int h = m_mask.height();

    for (int y = 0; y < h; ++y) {
        uchar *dst = m_mask.scanLine(y);
        const uchar *s = src.constScanLine(y);

        for (int x = 0; x < w; ++x) {
            const int a = dst[x];
            const int b = s[x];

            switch (op) {
            case Op::Add:
                // 并集取大值。用 max 而不是相加，避免两个半透明区域重叠后
                // 变成"超饱和"的 255（相加会在重叠处错误地全选中）。
                dst[x] = uchar(qMax(a, b));
                break;

            case Op::Subtract:
                // 差集：按比例扣减。例如原蒙版 200、减去区域 128，
                // 结果 200 - 200*128/255 ≈ 100，而不是简单地归零。
                // 这样在抗锯齿边缘处也能得到平滑结果。
                dst[x] = uchar(a - (a * b) / 255);
                break;

            case Op::Intersect:
                // 交集取小值
                dst[x] = uchar(qMin(a, b));
                break;

            case Op::Replace:
                dst[x] = uchar(b);
                break;
            }
        }
    }

    markModified();
}

// ===========================================================================
//  羽化
// ===========================================================================

void Selection::feather(qreal radius)
{
    if (!m_active || m_allSelected || m_mask.isNull() || radius <= 0.0)
        return;

    // ── 实现已提取到 engine 层的纯函数 ──
    //
    // 原先这份"三次盒式模糊"的七十行代码是内联在这里的。之所以搬到
    // engine/filters::featherGrayMask()，是因为选区工具也需要它 ——
    // 工具要先对"刚拖出来的形状蒙版"单独羽化，再与已有选区做布尔运算；
    // 若它自己再实现一遍，两处的模糊参数（半径换算、模糊次数、
    // 精度策略）迟早会漂移，表现为"工具做出来的羽化比菜单里的更硬/更软"。
    //
    // 放在 engine 层也符合分层约定：那是个"输入 QImage、输出 QImage"
    // 的无副作用纯函数，正是 engine 该放的东西；Selection 只负责
    // 状态标志与包围盒缓存的维护。
    //
    // ★ 注意语义 ★
    //   本函数是对**当前蒙版**的就地处理，应作为选区操作的收尾步骤。
    //   不要在"每次改动选区的中间步骤"里调用（会累积模糊，
    //   改三次就烂成一团）。这也是选区工具选择"先羽化形状、再合入"
    //   而不是"合入后再羽化"的原因。
    m_mask = filters::featherGrayMask(m_mask, radius);

    markModified();
}

// ===========================================================================
//  便捷操作
// ===========================================================================

void Selection::selectRect(const QRect &rect, Op op, bool antialiased)
{
    // ★ 注意这里又一次提前返回 ★
    //   没有蒙版就没有尺寸参考。新文档上第一次做选区前，
    //   调用方必须先调 prepareCanvas()，否则这一句会静默吞掉操作。
    //   详见 prepareCanvas() 的注释。
    if (m_mask.isNull())
        return;

    QPainterPath path;
    path.addRect(rect);
    applyPath(path, op, antialiased);
}

void Selection::selectEllipse(const QRect &ellipseRect, Op op, bool antialiased)
{
    // 同上：蒙版为空时提前返回，调用方需先 prepareCanvas()。
    if (m_mask.isNull())
        return;

    QPainterPath path;
    path.addEllipse(ellipseRect);
    applyPath(path, op, antialiased);
}

void Selection::translate(const QPoint &delta)
{
    if (!m_active || m_allSelected || m_mask.isNull())
        return;
    if (delta.isNull())
        return;

    // 平移蒙版内容。越界的部分直接丢弃（不环绕），与原蒙版保持一致的语义。
    QImage moved(m_mask.size(), QImage::Format_Grayscale8);
    moved.fill(0);

    const int w = m_mask.width();
    const int h = m_mask.height();

    for (int y = 0; y < h; ++y) {
        const int srcY = y - delta.y();
        if (srcY < 0 || srcY >= h)
            continue;

        const uchar *srcLine = m_mask.constScanLine(srcY);
        uchar *dstLine = moved.scanLine(y);

        for (int x = 0; x < w; ++x) {
            const int srcX = x - delta.x();
            if (srcX < 0 || srcX >= w)
                continue;
            dstLine[x] = srcLine[srcX];
        }
    }

    m_mask = moved;
    touch();
}

void Selection::invert()
{
    // 全选状态下反选 = 空选区；但本工程的语义是"没有选区即全选"，
    // 因此反选一个全选状态会得到"什么都没选"，这是个死胡同。
    // 处理方式：直接返回，由调用方（菜单项）在无选区时禁用该项。
    if (!m_active || m_allSelected || m_mask.isNull())
        return;

    const int w = m_mask.width();
    const int h = m_mask.height();

    for (int y = 0; y < h; ++y) {
        uchar *line = m_mask.scanLine(y);
        for (int x = 0; x < w; ++x)
            line[x] = uchar(255 - line[x]);
    }

    touch();
}

Selection Selection::clone() const
{
    Selection copy;
    copy.m_mask = m_mask.isNull() ? QImage() : m_mask.copy();
    copy.m_active      = m_active;
    copy.m_allSelected = m_allSelected;
    // ★ 克隆是"同一份内容"的副本：修订号**原样带走**，不另取新号 ★
    //   取新号会让"撤销恢复出来的选区"看起来像被改过 —— 不至于出错，
    //   但会白白多重建一次轮廓。带同一个号是安全的：内容确实相同
    //   （这正是 revision() 那条语义要求的）。
    copy.m_revision    = m_revision;
    copy.m_boundsDirty = true;
    return copy;
}

// ===========================================================================
//  尺寸同步
// ===========================================================================

void Selection::resizeCanvas(const QSize &newSize, const QPoint &offset)
{
    if (!m_active || m_allSelected || m_mask.isNull()) {
        // 无有效选区时只需跟随画布尺寸
        m_mask = QImage();
        touch();
        return;
    }

    // 画布变化后，选区按 offset 平移并重新对齐到新尺寸。
    // offset 的语义与 Document::applyGeometryChange 一致：新坐标 = 旧坐标 + offset。
    QImage resized(newSize, QImage::Format_Grayscale8);
    resized.fill(0);

    const int w = qMin(m_mask.width(),  newSize.width());
    const int h = qMin(m_mask.height(), newSize.height());

    for (int y = 0; y < h; ++y) {
        const int dstY = y + offset.y();
        if (dstY < 0 || dstY >= newSize.height())
            continue;

        const uchar *srcLine = m_mask.constScanLine(y);
        uchar *dstLine = resized.scanLine(dstY);

        for (int x = 0; x < w; ++x) {
            const int dstX = x + offset.x();
            if (dstX < 0 || dstX >= newSize.width())
                continue;
            dstLine[dstX] = srcLine[x];
        }
    }

    m_mask = resized;
    touch();
}

} // namespace zl
