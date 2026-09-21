#include "tools/CropTool.h"

#include "tools/ToolOptions.h"      // CropOptions（要读 preset / grid，必须完整定义）
#include "core/Document.h"
#include "core/Selection.h"

#include <QPainter>
#include <QPainterPath>
#include <QFontMetrics>
#include <QTransform>
#include <QPaintDevice>
#include <QtMath>

namespace zl {

// ============================================================================
//  常量
// ============================================================================

namespace {

// 角柄的屏幕边长（像素）。约定 9：这类"控件"的尺寸在屏幕上恒定，
// 不随视图缩放变化 —— 否则 25% 时只有 2px 点不中，800% 时 64px 糊成一片。
constexpr qreal kHandleScreenSize = 9.0;

// 命中手柄时额外放宽的屏幕像素。9px 的方块在 4K 屏上并不好点，放宽 3px 后
// 实际可点区域 15px；又不至于大到"想点框内结果抓住了柄"。
constexpr qreal kHandleHitSlack = 3.0;

// 裁剪框的最小尺寸（图像像素）。小于它视为"点了一下，没拖动"。
constexpr qreal kMinSize = 1.0;

// 框外遮罩的不透明度（0~255）。110 是试出来的：再深看不清框外的内容，
// 再浅对比不出"哪部分会被丢掉"。
constexpr int kMaskAlpha = 110;

constexpr qreal kEps = 1e-6;

inline qreal clampVal(qreal v, qreal lo, qreal hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 把点钳进矩形。新建框与拖手柄都要用它 —— 用户的手会拖出画布之外。
inline QPointF clampPoint(const QRectF &b, const QPointF &p)
{
    return QPointF(clampVal(p.x(), b.left(), b.right()),
                   clampVal(p.y(), b.top(),  b.bottom()));
}

} // namespace

// ============================================================================
//  构造 / 析构
// ============================================================================

CropTool::CropTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
    , m_options(new CropOptions(this))
{
    // 参数一变就重绘。比例预设还要**立刻套到已有的框上** ——
    // 用户在选项条里选 16:9，框却纹丝不动，会以为这个下拉框坏了。
    connect(m_options, &ToolOptions::changed, this, [this]() {
        applyRatioToCurrentRect();
        emit overlayChanged();
    });
}

CropTool::~CropTool() = default;

ToolOptions *CropTool::options() const
{
    return m_options;
}

QCursor CropTool::cursor() const
{
    // 裁剪是"框一个矩形"，十字光标是唯一说得通的选择：
    // 需要精确对准要保留的那条边。
    return QCursor(Qt::CrossCursor);
}

// ============================================================================
//  几何：画布 / 比例 / 播种 / 取整
// ============================================================================

QRectF CropTool::canvasBounds() const
{
    if (!document())
        return QRectF();

    const QSize s = document()->canvasSize();
    // ★ 用 QRectF(x, y, w, h) 而不是 QRect 转过来 ★
    //   QRect::right() 是 x+w-1，QRectF::right() 是 x+w。混用会让
    //   "贴着右边缘"的框差一个像素，表现为"裁剪结果比框少一列"。
    return QRectF(0.0, 0.0, qreal(s.width()), qreal(s.height()));
}

qreal CropTool::targetRatio() const
{
    return m_options ? CropOptions::presetRatio(m_options->preset()) : 0.0;
}

void CropTool::ensureSeeded()
{
    if (!isReady())
        return;

    const QSize cs = document()->canvasSize();
    if (cs.isEmpty())
        return;

    // 画布尺寸变了（刚裁完一次、或撤销了一次裁剪）→ 旧框已经不可能还在
    // 画布内，必须重新播种。这是唯一会让框"自己动"的情形。
    if (m_seeded && m_seedCanvasSize == cs)
        return;

    const QRectF bounds = canvasBounds();
    QRectF seed = bounds;

    // ── 有选区就用选区包围盒 ──
    //   用户已经用选框表达过"我要保留这一块"，再让他拿裁剪工具重框一遍
    //   是重复劳动。这也是"选区先行、裁剪随后"这条工作流的自然延伸
    //   （菜单的「图像 → 裁剪」正是同一件事的一键版本，见设计文档 6.8）。
    const Selection &sel = document()->selection();
    if (sel.isActive()) {
        const QRect b = sel.bounds();
        if (b.isValid() && !b.isEmpty()) {
            const QRectF clipped = QRectF(b).intersected(bounds);
            if (clipped.width() >= kMinSize && clipped.height() >= kMinSize)
                seed = clipped;
        }
    }

    m_rect = seed;
    m_seeded = true;
    m_seedCanvasSize = cs;
}

void CropTool::applyRatioToCurrentRect()
{
    if (!isReady())
        return;

    ensureSeeded();

    const QRectF bounds = canvasBounds();
    const qreal ratio = targetRatio();

    // 切成「自由」时**保留当前框的原形状**，不强行掰回什么默认值 ——
    // 用户只是想"从现在开始不约束了"，不是想改当前这个框。
    if (ratio <= kEps || bounds.isEmpty() || m_rect.isEmpty())
        return;

    // ── 内切到目标比例，锚点是框中心 ──
    //
    // ★ 为什么是"内切"而不是"外接" ★
    //   外接会让框突然涨出画布，然后被钳制回来 —— 用户看到的是一次跳变。
    //   内切只缩不放，视觉上像"框自己收了一下"，最容易理解。
    qreal w = m_rect.width();
    qreal h = m_rect.height();

    if (w / h > ratio) w = h * ratio;   // 太扁 → 收窄
    else               h = w / ratio;   // 太高 → 压低

    // 再整体缩到放得下（保持比例）
    qreal k = 1.0;
    if (w > bounds.width())  k = qMin(k, bounds.width() / w);
    if (h > bounds.height()) k = qMin(k, bounds.height() / h);
    w *= k;
    h *= k;

    const QPointF center = m_rect.center();
    const qreal x0 = clampVal(center.x() - w / 2.0, bounds.left(),
                              qMax(bounds.left(), bounds.right() - w));
    const qreal y0 = clampVal(center.y() - h / 2.0, bounds.top(),
                              qMax(bounds.top(), bounds.bottom() - h));

    m_rect = QRectF(x0, y0, w, h);
}

QRect CropTool::currentRect() const
{
    if (!document() || m_rect.isEmpty())
        return QRect();

    const QSize cs = document()->canvasSize();
    if (cs.isEmpty())
        return QRect();

    // ── 浮点框 → 像素矩形 ──
    //
    // ★ 判据是"像素**中心**落在框内"，不是"像素与框相交" ★
    //
    //   一个像素列 i 覆盖 [i, i+1)，中心是 i+0.5。框是两条**线**，
    //   覆盖的是两条线之间的格子：
    //
    //       i + 0.5 >= left    →  i >= left - 0.5    →  i >= ceil(left - 0.5)
    //       i + 0.5 <  right   →  i <  right - 0.5   →  i <= ceil(right - 0.5) - 1
    //
    //   于是 **宽度为整数 w 的框必然给出 w 个像素**，与框落在哪个小数坐标
    //   无关 —— 这正是"标签上写的尺寸"与"实际裁出来的尺寸"能对上的前提。
    //
    //   ★ 两种错法都试过 ★
    //     · 用"相交"判据（floor(left) / ceil(right)-1）：left=20.4、宽 40
    //       的框会给出 20..60 共 41 列 —— 标签写 40、裁出来 41。
    //     · 用 qRound(left) / qRound(right)：在半像素处同样会多算或少算一列。
    //   两者都只在**非整数坐标**下露馅，而 100% 之外的所有缩放级别
    //   都会产生非整数坐标（33% 下拖 1200 个屏幕像素 = 3636.4 个图像像素），
    //   所以这不是理论问题，是每次缩放后拖框都会碰到的问题。
    const int x0 = qBound(0, int(qCeil(m_rect.left()  - 0.5)), cs.width() - 1);
    const int y0 = qBound(0, int(qCeil(m_rect.top()   - 0.5)), cs.height() - 1);
    const int x1 = qBound(x0, int(qCeil(m_rect.right()  - 0.5)) - 1, cs.width() - 1);
    const int y1 = qBound(y0, int(qCeil(m_rect.bottom() - 0.5)) - 1, cs.height() - 1);

    return QRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

// ============================================================================
//  几何：三个纯函数（static，自检直接调）
// ============================================================================

QRectF CropTool::dragNewRect(const QRectF &bounds, const QPointF &anchor,
                             const QPointF &current, qreal ratio, bool square)
{
    if (square)
        ratio = 1.0;   // Shift 的临时约束覆盖选项条上的比例

    const QPointF a = clampPoint(bounds, anchor);
    const QPointF c = clampPoint(bounds, current);

    // 先把鼠标点折到锚点的四个象限之一，之后全部计算都在"正的宽高"上做，
    // 最后按象限摆回去。比一边算一边判断符号少一半分支，也不容易写反。
    const qreal sx = (c.x() >= a.x()) ? 1.0 : -1.0;
    const qreal sy = (c.y() >= a.y()) ? 1.0 : -1.0;

    qreal w = qAbs(c.x() - a.x());
    qreal h = qAbs(c.y() - a.y());

    if (ratio > kEps) {
        // ── 以"较大的那个方向"为准反算另一轴 ──
        //
        // ★ 不是"以宽为准反算高" ★
        //   那样在竖向拖得比横向远时，框会跟不上鼠标（高被改小），
        //   手感上是"我明明往下拖了，框却没长"。取较大的方向为准，
        //   框总是至少包住用户拖出的范围。
        if (w >= h * ratio) h = w / ratio;
        else                w = h * ratio;

        // 再按锚点到画布边缘的可用空间**等比收缩**。
        // ★ 必须等比 ★ 直接截断会把比例改掉，用户会觉得"框突然变形了"。
        const qreal availX = (sx > 0) ? (bounds.right() - a.x())
                                      : (a.x() - bounds.left());
        const qreal availY = (sy > 0) ? (bounds.bottom() - a.y())
                                      : (a.y() - bounds.top());
        qreal k = 1.0;
        if (w > kEps) k = qMin(k, availX / w);
        if (h > kEps) k = qMin(k, availY / h);
        k = clampVal(k, 0.0, 1.0);
        w *= k;
        h *= k;
    }

    const qreal x0 = (sx > 0.0) ? a.x() : a.x() - w;
    const qreal y0 = (sy > 0.0) ? a.y() : a.y() - h;
    return QRectF(x0, y0, w, h);
}

QRectF CropTool::resizeRect(const QRectF &bounds, const QRectF &start, Handle handle,
                            const QPointF &current, qreal ratio, bool square)
{
    if (square)
        ratio = 1.0;

    const bool movesL = (handle == Handle::TopLeft || handle == Handle::Left ||
                         handle == Handle::BottomLeft);
    const bool movesR = (handle == Handle::TopRight || handle == Handle::Right ||
                         handle == Handle::BottomRight);
    const bool movesT = (handle == Handle::TopLeft || handle == Handle::Top ||
                         handle == Handle::TopRight);
    const bool movesB = (handle == Handle::BottomLeft || handle == Handle::Bottom ||
                         handle == Handle::BottomRight);

    if (!movesL && !movesR && !movesT && !movesB)
        return start;   // 不是手柄（Inside / None）→ 不动

    // 不动的边直接从 start 继承。这样"拖左边"不会顺带把上下边挪了。
    qreal left   = start.left();
    qreal top    = start.top();
    qreal right  = start.right();
    qreal bottom = start.bottom();

    const QPointF c = clampPoint(bounds, current);

    if (ratio <= kEps) {
        // ── 无比例约束：每条边独立跟手 ──
        //   钳制写成"先夹到画布内，再保证与对边至少留 kMinSize"，
        //   并且用 qMax/qMin 保证下界不会超过上界 —— 否则 qBound 会断言。
        if (movesL) left   = clampVal(c.x(), bounds.left(),
                                      qMax(bounds.left(), right - kMinSize));
        if (movesR) right  = clampVal(c.x(), qMin(bounds.right(), left + kMinSize),
                                      bounds.right());
        if (movesT) top    = clampVal(c.y(), bounds.top(),
                                      qMax(bounds.top(), bottom - kMinSize));
        if (movesB) bottom = clampVal(c.y(), qMin(bounds.bottom(), top + kMinSize),
                                      bounds.bottom());

        return QRectF(QPointF(left, top), QPointF(right, bottom)).normalized();
    }

    // ── 比例约束 ──
    //   固定点：角柄 → 对角；边柄 → 对边的中点（另一轴因此天然居中）。
    const QPointF anchor(movesL ? right  : (movesR ? left  : (left + right) / 2.0),
                         movesT ? bottom : (movesB ? top   : (top + bottom) / 2.0));

    const qreal sx = movesR ? 1.0 : (movesL ? -1.0 : 0.0);
    const qreal sy = movesB ? 1.0 : (movesT ? -1.0 : 0.0);

    // 先沿拖动轴把鼠标夹住，防止"拖过头把框翻面"。
    // 翻面在无比例时靠 normalized() 还能看，在比例约束下会算出一个
    // 朝着反方向长出去的框，视觉上像是失控了。
    QPointF cc = c;
    if (movesL) cc.setX(qMin(cc.x(), right));
    if (movesR) cc.setX(qMax(cc.x(), left));
    if (movesT) cc.setY(qMin(cc.y(), bottom));
    if (movesB) cc.setY(qMax(cc.y(), top));

    qreal w = 0.0;
    qreal h = 0.0;

    if (sx != 0.0 && sy != 0.0) {
        // 角柄：与新建时同一条规则（以较大的方向为准）
        w = qAbs(cc.x() - anchor.x());
        h = qAbs(cc.y() - anchor.y());
        if (w >= h * ratio) h = w / ratio;
        else                w = h * ratio;
    } else if (sx != 0.0) {
        // 左右边柄：宽跟手，高由比例定，且在垂直方向上以对边中点居中
        w = qAbs(cc.x() - anchor.x());
        h = w / ratio;
    } else {
        // 上下边柄：高跟手，宽由比例定，水平方向居中
        h = qAbs(cc.y() - anchor.y());
        w = h * ratio;
    }

    // ── 可用空间 ──
    //   拖动方向那一侧到画布边缘；居中轴取"两侧较近的那一边 × 2"。
    qreal availX = 0.0;
    if (sx > 0.0)      availX = bounds.right()  - anchor.x();
    else if (sx < 0.0) availX = anchor.x() - bounds.left();
    else               availX = 2.0 * qMin(anchor.x() - bounds.left(),
                                          bounds.right() - anchor.x());

    qreal availY = 0.0;
    if (sy > 0.0)      availY = bounds.bottom() - anchor.y();
    else if (sy < 0.0) availY = anchor.y() - bounds.top();
    else               availY = 2.0 * qMin(anchor.y() - bounds.top(),
                                          bounds.bottom() - anchor.y());

    qreal k = 1.0;
    if (w > kEps) k = qMin(k, availX / w);
    if (h > kEps) k = qMin(k, availY / h);
    k = clampVal(k, 0.0, 1.0);
    w *= k;
    h *= k;

    const qreal x0 = (sx > 0.0) ? anchor.x()
                                : (sx < 0.0 ? anchor.x() - w : anchor.x() - w / 2.0);
    const qreal y0 = (sy > 0.0) ? anchor.y()
                                : (sy < 0.0 ? anchor.y() - h : anchor.y() - h / 2.0);
    return QRectF(x0, y0, w, h);
}

QRectF CropTool::moveRect(const QRectF &bounds, const QRectF &start, const QPointF &delta)
{
    QRectF r = start.translated(delta);

    // ── 整框钳制 ──
    //   框一旦被推出画布，用户就再也抓不到它了：框外点击是"新建"，而新建
    //   需要一次完整的拖动。所以宁可让框贴着边走，也不能让它出界。
    qreal dx = 0.0;
    if (r.left() < bounds.left())         dx = bounds.left() - r.left();
    else if (r.right() > bounds.right())  dx = bounds.right() - r.right();

    qreal dy = 0.0;
    if (r.top() < bounds.top())            dy = bounds.top() - r.top();
    else if (r.bottom() > bounds.bottom()) dy = bounds.bottom() - r.bottom();

    return r.translated(dx, dy);
}

// ============================================================================
//  命中测试
// ============================================================================

CropTool::Handle CropTool::hitTest(const QPointF &imagePos, qreal zoom) const
{
    if (m_rect.isEmpty())
        return Handle::None;

    // 屏幕上的可点半径 → 图像坐标。
    // zoom 取保护值：极端缩放下它可能算成 0（手柄永远点不中）或很大
    // （整个画布都成了手柄，框内再也拖不动）。
    const qreal z = qMax(0.01, qAbs(zoom));
    const qreal tol = (kHandleScreenSize / 2.0 + kHandleHitSlack) / z;

    const QPointF c = m_rect.center();
    struct Probe { Handle h; QPointF p; };
    const Probe probes[] = {
        { Handle::TopLeft,     m_rect.topLeft()     },
        { Handle::TopRight,    m_rect.topRight()    },
        { Handle::BottomLeft,  m_rect.bottomLeft()  },
        { Handle::BottomRight, m_rect.bottomRight() },
        { Handle::Top,    QPointF(c.x(), m_rect.top())    },
        { Handle::Bottom, QPointF(c.x(), m_rect.bottom()) },
        { Handle::Left,   QPointF(m_rect.left(),  c.y())  },
        { Handle::Right,  QPointF(m_rect.right(), c.y())  },
    };

    // ★ 角柄先于边柄 ★
    //   两者的可点区域会重叠（框很小时尤其明显）。角柄能一次改两个方向，
    //   是用户更可能想要的那个。
    for (const Probe &pr : probes) {
        if (qAbs(imagePos.x() - pr.p.x()) <= tol &&
            qAbs(imagePos.y() - pr.p.y()) <= tol)
            return pr.h;
    }

    return m_rect.contains(imagePos) ? Handle::Inside : Handle::None;
}

// ============================================================================
//  输入事件
// ============================================================================

ToolResult CropTool::mousePress(const ToolEvent &e)
{
    if (!isReady() || !e.isPress())
        return ToolResult::ignored();

    // 右键 / 中键留给画布（右键菜单、中键平移）。工具抢了它们，
    // 用户就没法在裁剪工具下平移视图了。
    if (e.button == Qt::RightButton || e.button == Qt::MiddleButton)
        return ToolResult::ignored();

    ensureSeeded();

    const QRectF bounds = canvasBounds();
    if (bounds.isEmpty())
        return ToolResult::ignored();

    m_dragStart     = e.imagePos;
    m_dragStartRect = m_rect;
    m_dragRatio     = targetRatio();

    const Handle h = hitTest(e.imagePos, e.viewZoom);

    // ★ 特例："框 == 整幅画布"时，框内按下算拉新框，不算平移 ★
    //
    //   默认（没有选区）时框就是整幅画布，此时画布上**根本没有"框外"可以按**。
    //   若还是按"框内 = 平移"处理，用户第一次在画布上拖动会毫无反应 ——
    //   整幅画布已被 moveRect 钳死，往哪个方向都推不动。看起来就像工具坏了，
    //   而这是**绝大多数用户第一次用裁剪工具时会做的事**。
    //
    //   框一旦小于画布，"框内 = 平移"就恢复正常语义（这时平移是有意义的）。
    const QRectF boundsForHit = canvasBounds();
    const bool rectCoversCanvas =
        qAbs(m_rect.width()  - boundsForHit.width())  < kEps &&
        qAbs(m_rect.height() - boundsForHit.height()) < kEps;

    if (h == Handle::Inside && !rectCoversCanvas) {
        m_mode = Mode::Move;
        m_activeHandle = Handle::None;
    } else if (h != Handle::None && h != Handle::Inside) {
        m_mode = Mode::Resize;
        m_activeHandle = h;
    } else {
        // 框外 → 拉一个新框。先把框塌成按下点这一个"零尺寸"状态：
        // 这样拖动过程中遮罩天然就是"整幅画布都被挡"，用户一眼看到
        // 自己正在决定哪一块留下来。松开时若长宽仍为 0 就还原。
        m_mode = Mode::NewRect;
        m_activeHandle = Handle::None;
        m_rect = QRectF(e.imagePos, e.imagePos);
    }

    setBusy(true);
    return ToolResult::repaint();
}

ToolResult CropTool::mouseMove(const ToolEvent &e)
{
    if (m_mode == Mode::Idle || !isReady())
        return ToolResult::ignored();

    const QRectF bounds = canvasBounds();
    const bool square   = e.modifiers.shift;   // Shift → 临时正方形

    switch (m_mode) {
    case Mode::NewRect:
        m_rect = dragNewRect(bounds, m_dragStart, e.imagePos, m_dragRatio, square);
        break;

    case Mode::Move:
        // ★ 从"按下时的框"重算，不在上次结果上累加 ★
        //   累加会让"拖出去再拖回来"产生漂移（本项目反复踩过的同一个坑：
        //   画笔、移动、选区都是如此）。
        m_rect = moveRect(bounds, m_dragStartRect, e.imagePos - m_dragStart);
        break;

    case Mode::Resize:
        m_rect = resizeRect(bounds, m_dragStartRect, m_activeHandle,
                            e.imagePos, m_dragRatio, square);
        break;

    default:
        break;
    }

    return ToolResult::repaint();
}

ToolResult CropTool::mouseRelease(const ToolEvent &e)
{
    Q_UNUSED(e)

    if (m_mode == Mode::Idle)
        return ToolResult::ignored();

    // ── "点了一下没拖动"必须还原 ──
    //   否则一次误点就把用户调好的框塌成一个零尺寸矩形，遮罩还会盖满
    //   整幅画布 —— 看起来像工具坏了。
    if (m_mode == Mode::NewRect &&
        (m_rect.width() < kMinSize || m_rect.height() < kMinSize)) {
        m_rect = m_dragStartRect;
        showStatus(tr("在画布上拖动即可框出要保留的区域"), 2500);
    }

    m_mode = Mode::Idle;
    m_activeHandle = Handle::None;
    setBusy(false);
    return ToolResult::repaint();
}

ToolResult CropTool::mouseDoubleClick(const ToolEvent &e)
{
    if (!isReady())
        return ToolResult::ignored();

    ensureSeeded();

    // 只有双击落在框内才提交。双击框外应当是"从这里开始拉一个新框"，
    // 与 PS 一致 —— 框外第一下按下时就已经开始拉新框了。
    if (!m_rect.contains(e.imagePos))
        return ToolResult::ignored();

    emitCrop();
    return ToolResult::accepted();
}

ToolResult CropTool::keyPress(const ToolEvent &e)
{
    if (!isReady())
        return ToolResult::ignored();

    ensureSeeded();

    // ── Enter / 小键盘 Enter：提交 ──
    if (e.key == Qt::Key_Return || e.key == Qt::Key_Enter) {
        emitCrop();
        return ToolResult::accepted();
    }

    // ★ Esc 不会走到这里 ★
    //   CanvasView::keyPressEvent 在派发给工具**之前**就把 Esc 拦下来
    //   改调 cancelActiveTool() 了。所以真正生效的是 CropTool::cancel()。

    // ── 方向键微调框的位置 ──
    //   与移动工具同一套手感：1 像素 / Shift 加 10 像素。
    const qreal step = e.modifiers.shift ? 10.0 : 1.0;
    QPointF delta;
    switch (e.key) {
    case Qt::Key_Left:  delta = QPointF(-step,   0.0); break;
    case Qt::Key_Right: delta = QPointF( step,   0.0); break;
    case Qt::Key_Up:    delta = QPointF( 0.0, -step); break;
    case Qt::Key_Down:  delta = QPointF( 0.0,  step); break;
    default:
        return ToolResult::ignored();
    }

    const QRectF bounds = canvasBounds();
    const bool fullCanvas = qAbs(m_rect.width()  - bounds.width())  < kEps &&
                            qAbs(m_rect.height() - bounds.height()) < kEps;

    // 框本来就等于整幅画布时，移动没有意义 —— 继续执行只会把"整幅"
    // 变成一个偏在一边的框，用户会以为自己按错了键。
    if (fullCanvas) {
        showStatus(tr("裁剪框已经是整幅画布，无需移动"), 1500);
        return ToolResult::accepted();
    }

    m_rect = moveRect(bounds, m_rect, delta);
    return ToolResult::repaint();
}

// ============================================================================
//  提交
// ============================================================================

void CropTool::emitCrop()
{
    if (!isReady())
        return;

    ensureSeeded();

    const QRect canvas = document()->canvasRect();
    const QRect r      = currentRect();

    if (r.width() < 1 || r.height() < 1) {
        showStatus(tr("裁剪框太小，请先拖出一个要保留的区域"), 3000);
        return;
    }

    if (!r.intersects(canvas)) {
        showStatus(tr("裁剪框不在画布内"), 3000);
        return;
    }

    // 框就是整幅画布 → Document::crop() 会返回 false（等于没裁），
    // 这里提前挡住，免得压一条"什么都没改"的撤销记录。
    if (r == canvas) {
        showStatus(tr("裁剪框就是整幅画布，无需裁剪"), 3000);
        return;
    }

    // ★ 唯一的"动手"出口 ★
    //   真正的快照、Document::crop() 与内存预检都在 MainWindow 里，
    //   与另外 8 个几何入口共用同一段收尾代码（见头文件顶部 ③）。
    emit cropRequested(r);
}

// ============================================================================
//  叠加层
// ============================================================================

void CropTool::paintOverlay(QPainter &painter, CanvasView *view)
{
    Q_UNUSED(view)

    if (!isReady())
        return;

    ensureSeeded();

    const QRectF bounds = canvasBounds();
    if (bounds.isEmpty())
        return;

    // ── ① 框外遮罩 ──
    //
    // ★ 用"两个子路径 + 奇偶填充"，而不是四个矩形 ★
    //   四个矩形在角上会互相重叠，半透明色叠两次比边上深，四个角会明显
    //   更黑。奇偶填充一次成图，绝不会出现接缝或角上加深。
    QPainterPath mask;
    mask.setFillRule(Qt::OddEvenFill);
    mask.addRect(bounds);
    if (!m_rect.isEmpty())
        mask.addRect(m_rect);

    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, kMaskAlpha));
    painter.drawPath(mask);
    painter.restore();

    if (m_rect.isEmpty())
        return;

    // ── ② 视图缩放 ──
    //
    // ★ 为什么从 painter 里读，而不是新增接口 ★
    //   paintOverlay 被调用时，painter 的变换就是画布刚设好的 m_viewTransform
    //   （见 CanvasView::drawToolOverlay），而 tools/ 层不允许 include
    //   ui/CanvasView.h，拿不到 imageToWidget()。直接从矩阵里读缩放项，
    //   口径与画布完全一致，又不破坏分层。m11 可能带翻转的负号，取绝对值。
    const QTransform viewTransform = painter.worldTransform();
    const qreal zoom = qMax(0.01, qAbs(viewTransform.m11()));

    // ── ③ 构图网格（图像坐标；线宽折算成"屏幕上 1 像素"）──
    //
    //   网格只是构图参考，**绝不会进合成结果** —— 这与参考线、网格一样，
    //   属于"视图辅助不是图像内容"（架构规划第 8 条）。
    const CropGrid grid = m_options ? m_options->grid() : CropGrid::None;

    if (grid != CropGrid::None) {
        painter.save();
        painter.setClipRect(m_rect);   // 线只画在框内

        QPen gridPen(QColor(255, 255, 255, 140));
        gridPen.setWidthF(1.0 / zoom);
        painter.setPen(gridPen);

        const qreal l = m_rect.left();
        const qreal t = m_rect.top();
        const qreal w = m_rect.width();
        const qreal h = m_rect.height();

        auto vline = [&](qreal f) {
            painter.drawLine(QPointF(l + w * f, t), QPointF(l + w * f, t + h));
        };
        auto hline = [&](qreal f) {
            painter.drawLine(QPointF(l, t + h * f), QPointF(l + w, t + h * f));
        };

        switch (grid) {
        case CropGrid::Thirds:
            vline(1.0 / 3.0); vline(2.0 / 3.0);
            hline(1.0 / 3.0); hline(2.0 / 3.0);
            break;
        case CropGrid::Grid:
            for (int i = 1; i <= 5; ++i) {
                const qreal f = i / 6.0;
                vline(f); hline(f);
            }
            break;
        case CropGrid::Diagonal:
            painter.drawLine(QPointF(l,     t),     QPointF(l + w, t + h));
            painter.drawLine(QPointF(l + w, t),     QPointF(l,     t + h));
            break;
        case CropGrid::GoldenRatio:
            vline(0.382); vline(0.618);
            hline(0.382); hline(0.618);
            break;
        case CropGrid::None:
        default:
            break;
        }

        painter.restore();
    }

    // ── ④ 边框、角柄与尺寸标签（控件坐标）──
    //
    // ★ 必须 resetTransform ★
    //   这三样都要保持**屏幕**尺寸恒定（约定 9）。若在图像坐标下画，
    //   9px 的角柄在 25% 缩放下只有 2px（点不中），在 800% 下变成 72px
    //   （糊成一片、盖住半个画面）。
    painter.save();
    painter.resetTransform();

    // 用画布自己的变换把框映到控件坐标 —— 不自算 zoom/offset，
    // 否则翻转、旋转一个不落地都要在工具里再实现一遍。
    const QRectF rw = viewTransform.mapRect(m_rect);

    QPen frame(QColor(255, 255, 255, 230));
    frame.setWidthF(1.0);
    painter.setPen(frame);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rw);

    if (!rw.isEmpty()) {
        painter.setPen(QPen(QColor(40, 40, 40, 220), 1.0));
        painter.setBrush(QColor(255, 255, 255, 235));

        const qreal hs = kHandleScreenSize / 2.0;
        const QPointF handles[] = {
            rw.topLeft(), rw.topRight(), rw.bottomLeft(), rw.bottomRight(),
            QPointF(rw.center().x(), rw.top()),
            QPointF(rw.center().x(), rw.bottom()),
            QPointF(rw.left(),  rw.center().y()),
            QPointF(rw.right(), rw.center().y())
        };
        for (const QPointF &p : handles)
            painter.drawRect(QRectF(p.x() - hs, p.y() - hs, hs * 2.0, hs * 2.0));

        // ── 尺寸标签 ──
        //   报的是**最终结果的像素尺寸**，与 currentRect() 同源 ——
        //   标签写 800×600 而裁出来 799×600 是最伤信任的一类 bug。
        const QRect r = currentRect();
        const QString text = tr("%1 × %2 px").arg(r.width()).arg(r.height());

        QFont f = painter.font();
        f.setPointSizeF(qMax(9.0, f.pointSizeF()));
        painter.setFont(f);

        const QFontMetrics fm(f);
        const QRect textRect = fm.boundingRect(text).adjusted(-6, -3, 6, 3);

        qreal lx = rw.left();
        qreal ly = rw.top() - textRect.height() - 6.0;
        if (ly < 2.0)   // 框贴到视口顶边时翻到框内，免得被切掉
            ly = rw.top() + 6.0;

        if (painter.device()) {
            const qreal dw = qreal(painter.device()->width());
            lx = qMin(lx, dw - textRect.width() - 4.0);
        }
        lx = qMax(lx, 2.0);

        const QRect bgRect(int(lx), int(ly), textRect.width(), textRect.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 170));
        painter.drawRoundedRect(bgRect, 3, 3);

        painter.setPen(QColor(255, 255, 255));
        painter.drawText(bgRect, Qt::AlignCenter, text);
    }

    painter.restore();
}

// ============================================================================
//  生命周期
// ============================================================================

void CropTool::setDocument(Document *doc)
{
    if (document() == doc)
        return;

    // 换了文档 → 旧框是**旧画布**的坐标，重新播种之前用不得。
    m_seeded = false;
    m_mode = Mode::Idle;
    m_activeHandle = Handle::None;
    m_rect = QRectF();

    AbstractTool::setDocument(doc);
}

void CropTool::commit()
{
    // ★ 这里绝不能执行裁剪 ★
    //   ToolManager::setActiveTool 在"用户点了别的工具"时也会调 commit()，
    //   而那时用户并没有确认过任何事。偷偷裁一刀是**数据损失**，
    //   而且看起来像是工具自己乱动。未确认的框直接丢掉 ——
    //   与 SelectionTool 的 commit() 同一条语义。
    m_mode = Mode::Idle;
    m_activeHandle = Handle::None;
    m_seeded = false;
    m_rect = QRectF();

    AbstractTool::cancel();   // 清 busy 标志与拖拽累加矩形

    emit overlayChanged();
}

void CropTool::cancel()
{
    // Esc 走这里（CanvasView::keyPressEvent 拦下后调 cancelActiveTool）。
    //
    // ★ 关键：这里**什么都不用回滚** ★
    //   拖动期间压根没碰过 Document —— 这是"预览只用一个 QRectF"换来的
    //   直接好处。所谓"取消"就是把框按当前上下文重新播种。
    m_mode = Mode::Idle;
    m_activeHandle = Handle::None;

    AbstractTool::cancel();

    if (!isReady())
        return;

    m_seeded = false;
    ensureSeeded();          // 有选区→选区包围盒；否则→整幅画布
    emit overlayChanged();
    showStatus(tr("已复位裁剪框"), 1500);
}

} // namespace zl
