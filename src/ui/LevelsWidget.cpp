// ============================================================================
//  LevelsWidget.cpp
// ============================================================================

#include "ui/LevelsWidget.h"

#include "engine/Histogram.h"
#include "engine/Lut.h"

#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygon>
#include <QSizePolicy>
#include <QtMath>

namespace zl {

namespace {

constexpr int kMargin  = 10;   // 四边留白
constexpr int kPlotH   = 110;  // 直方图区高（下面按比例分配的基准）
constexpr int kHandleH = 12;   // 手柄行高
constexpr int kGapH    = 10;   // 手柄行与输出条之间的间隔
constexpr int kBarH    = 18;   // 输出渐变条高
constexpr int kHandleW = 11;   // 三角手柄宽
constexpr int kHitTol  = 8;    // 命中容差（只比水平距离，见 hitTest）

// 与 engine 的钳制范围一致（见 adjust::levelsLut）。
// 写在这里是为了让"手柄拖不动了"与"引擎不再响应"发生在同一点上。
constexpr qreal kMinGamma = 0.1;
constexpr qreal kMaxGamma = 10.0;

// 四行高度按固定比例分配。
//
// ★ 为什么不用"固定像素累加" ★
//   控件被布局拉矮时（用户把对话框缩小），固定像素会让最下面一行
//   跑到控件外面 —— 手柄看不见，却仍然能被点到，非常迷惑。
struct RowMetrics
{
    int plotH   = 0;
    int handleH = 0;
    int gapH    = 0;
    int barH    = 0;
};

RowMetrics metricsFor(int widgetHeight)
{
    RowMetrics m;

    const int avail = qMax(0, widgetHeight - 2 * kMargin);
    if (avail <= 0)
        return m;   // 还没布局：各行高度为 0，所有几何函数据此返回空矩形

    const int total = kPlotH + kHandleH + kGapH + kBarH + kHandleH;
    const qreal s = qreal(avail) / total;

    m.plotH   = int(kPlotH   * s);
    m.handleH = int(kHandleH * s);
    m.gapH    = int(kGapH    * s);
    m.barH    = int(kBarH    * s);
    return m;
}

// ★ 深色主题适配（2026-10-15）★ 与 CurveEditor 同一理由、同一手法：
//   控件底 Window / 绘图区底 Base / 网格与描边 mix(Window, Mid, t) /
//   直方图柱 Text 派生 alpha。浅色主题下混色结果与原来的硬编码
//   （250/255/236/150/90@130）几乎一致，深色下自动变暗。
//   手柄的黑/白填充与输出渐变条是**内容语义色**（黑场手柄就该是黑的），
//   保留不动。
QColor mixWithMid(const QPalette &pal, qreal t)
{
    const QColor a = pal.color(QPalette::Window);
    const QColor b = pal.color(QPalette::Mid);
    return QColor(qRound(a.red()   * (1.0 - t) + b.red()   * t),
                  qRound(a.green() * (1.0 - t) + b.green() * t),
                  qRound(a.blue()  * (1.0 - t) + b.blue()  * t));
}

QColor withAlpha(const QColor &c, int alpha)
{
    QColor r = c;
    r.setAlpha(alpha);
    return r;
}

// 两个任意色按比例混合（手柄描边用：与 WindowText 混一半，
// 背景亮时得深边、背景暗时得亮边，黑白手柄在两种主题下都有轮廓）
QColor mixColors(const QColor &a, const QColor &b, qreal t)
{
    return QColor(qRound(a.red()   * (1.0 - t) + b.red()   * t),
                  qRound(a.green() * (1.0 - t) + b.green() * t),
                  qRound(a.blue()  * (1.0 - t) + b.blue()  * t));
}

} // namespace

// ---------------------------------------------------------------------------
//  构造
// ---------------------------------------------------------------------------

LevelsWidget::LevelsWidget(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // 方向键微调需要键盘焦点；鼠标跟踪用于悬停高亮。
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    setMinimumSize(minimumSizeHint());
}

void LevelsWidget::setParams(const adjust::LevelsParams &p)
{
    m_params = p;

    // 参数被外部改动（复位 / 自动色阶）后，拖动中的临时坐标必须失效 ——
    // 否则手柄会停在旧位置，直到下一次拖动才归位。
    m_grayDragLevel = -1;

    update();
}

void LevelsWidget::setHistogram(const QVector<int> &hist)
{
    m_histogram = hist;
    update();
}

bool LevelsWidget::isIdentity() const
{
    return m_params.inBlack  == 0   && m_params.inWhite  == 255
        && m_params.outBlack == 0   && m_params.outWhite == 255
        && qFuzzyCompare(m_params.gamma, 1.0);
}

QSize LevelsWidget::sizeHint() const
{
    return QSize(440, 2 * kMargin + kPlotH + kHandleH + kGapH + kBarH + kHandleH);
}

QSize LevelsWidget::minimumSizeHint() const
{
    // 宽度允许压到 200：对话框变窄时手柄仍能分辨；
    // 高度不可压 —— 低于各行之和时手柄行会退化成 0 高、点不到。
    return QSize(200, sizeHint().height());
}

// ---------------------------------------------------------------------------
//  几何
// ---------------------------------------------------------------------------

QRect LevelsWidget::inputPlotRect() const
{
    const RowMetrics m = metricsFor(height());
    const int w = qMax(0, width() - 2 * kMargin);
    return QRect(kMargin, kMargin, w, m.plotH);
}

QRect LevelsWidget::inHandleRow() const
{
    const RowMetrics m = metricsFor(height());
    const int w = qMax(0, width() - 2 * kMargin);

    // +1 是为了在手柄与直方图之间留一条缝，避免三角的尖压在直方图上
    return QRect(kMargin, kMargin + m.plotH + 1, w, m.handleH);
}

QRect LevelsWidget::outBarRect() const
{
    const RowMetrics m = metricsFor(height());
    const int w = qMax(0, width() - 2 * kMargin);
    return QRect(kMargin, kMargin + m.plotH + 1 + m.handleH + m.gapH, w, m.barH);
}

QRect LevelsWidget::outHandleRow() const
{
    const RowMetrics m = metricsFor(height());
    const int w = qMax(0, width() - 2 * kMargin);
    return QRect(kMargin, kMargin + m.plotH + 1 + m.handleH + m.gapH + m.barH,
                 w, m.handleH);
}

QRect LevelsWidget::handleRect(Handle h) const
{
    const bool isOut = (h == Handle::OutBlack || h == Handle::OutWhite);
    const QRect row = isOut ? outHandleRow() : inHandleRow();
    if (row.isEmpty())
        return QRect();

    int x = 0;
    switch (h) {
    case Handle::InBlack:
        x = levelToX(m_params.inBlack, row);
        break;
    case Handle::InGray:
        x = grayHandleX();
        break;
    case Handle::InWhite:
        x = levelToX(m_params.inWhite, row);
        break;
    case Handle::OutBlack:
        x = levelToX(m_params.outBlack, row);
        break;
    case Handle::OutWhite:
        x = levelToX(m_params.outWhite, row);
        break;
    case Handle::None:
        return QRect();
    }

    return QRect(x - kHandleW / 2, row.top(), kHandleW, row.height());
}

int LevelsWidget::levelToX(int level, const QRect &row) const
{
    if (row.width() <= 1)
        return row.left();

    const int lv = qBound(0, level, 255);
    return row.left() + qRound(qreal(row.width() - 1) * lv / 255.0);
}

int LevelsWidget::xToLevel(int x, const QRect &row) const
{
    if (row.width() <= 1)
        return 0;

    const qreal u = qreal(x - row.left()) / qreal(row.width() - 1);
    return qBound(0, int(qRound(u * 255.0)), 255);
}

int LevelsWidget::grayHandleX() const
{
    const QRect row = inHandleRow();
    if (row.isEmpty())
        return 0;

    // ── 拖动中：直接用目标值定位 ──
    //   反解出的 gamma 会被钳制到 [0.1, 10]；若绘制时再由 gamma 正查位置，
    //   钳制边缘会让手柄往回跳一格，手感像是卡住不动。
    if (m_dragging == Handle::InGray && m_grayDragLevel >= 0)
        return levelToX(m_grayDragLevel, row);

    // ── 其余情况一律由烘焙表反查（见头文件的铁律说明）──
    //
    //   把输出范围固定成 0..255 再烘焙：灰点位置只由"输入黑白场 + gamma"
    //   决定，与输出色阶无关。若把当前输出范围也带上，用户一拉输出白场
    //   灰点就会跟着漂 —— 那是错的。
    adjust::LevelsParams probe = m_params;
    probe.outBlack = 0;
    probe.outWhite = 255;

    const CurveLut lut = adjust::levelsLut(probe);

    const int lo = qBound(0, m_params.inBlack, 255);
    const int hi = qBound(0, m_params.inWhite, 255);

    // "中灰落在哪个输入值上" = 输出首次达到 128 的那个输入值
    for (int x = lo; x <= hi; ++x) {
        if (lut[size_t(x)] >= 128)
            return levelToX(x, row);
    }
    return levelToX(hi, row);
}

void LevelsWidget::applyParams(const adjust::LevelsParams &p)
{
    m_params = p;
    update();
}

void LevelsWidget::setGrayFromX(int x)
{
    const int lo = m_params.inBlack;
    const int hi = m_params.inWhite;
    const int span = hi - lo;

    // 黑白场之间至少要有一个色阶的跨度，否则比例无定义。
    // 界面把黑白场约束成 inBlack < inWhite，正常不会走到这里。
    if (span < 1)
        return;

    // ★ 灰点不能与黑白场重合 ★
    //   t = 0 或 t = 1 会让 gamma 变成 0 或无穷。留一个色阶的余量，
    //   配合下面的钳制，用户把灰点往两侧拖到底时手感是"顶住"而非"闪走"。
    const int target = qBound(lo + 1, x, hi - 1);
    const qreal t = qreal(target - lo) / qreal(span);   // ∈ (0,1)

    // 由 out(t) = t^(1/γ) = 0.5 反解：γ = ln t / ln 0.5
    //   t = 0.5  → γ = 1（中点即中灰）
    //   t = 0.25 → γ = 2（画面变亮，灰点左移）
    // 与 levelsLut() 里 qPow(clamped, 1.0 / gamma) 互为逆运算。
    const qreal gamma = qLn(t) / qLn(0.5);

    adjust::LevelsParams p = m_params;
    p.gamma = qBound(kMinGamma, gamma, kMaxGamma);

    m_grayDragLevel = target;
    applyParams(p);

    emit levelsChanged();
}

// ---------------------------------------------------------------------------
//  交互
// ---------------------------------------------------------------------------

LevelsWidget::Handle LevelsWidget::hitTest(const QPoint &pos) const
{
    const bool inRow  = inHandleRow().contains(pos);
    const bool outRow = outHandleRow().contains(pos);
    if (!inRow && !outRow)
        return Handle::None;

    const Handle candidates[] = {
        Handle::InBlack, Handle::InGray, Handle::InWhite,
        Handle::OutBlack, Handle::OutWhite
    };

    Handle best = Handle::None;
    int bestDist = kHitTol + 1;

    for (Handle h : candidates) {
        const bool isOut = (h == Handle::OutBlack || h == Handle::OutWhite);
        if (isOut ? !outRow : !inRow)
            continue;

        const QRect r = handleRect(h);
        if (r.isEmpty())
            continue;

        // 只比水平距离：手柄行本身很矮（约 12px），要求鼠标也落在行内
        // 已经足够严格；再加纵向分量只会让"差一点没点到"变多。
        const int d = qAbs(r.center().x() - pos.x());
        if (d < bestDist) {
            bestDist = d;
            best = h;
        }
    }
    return best;
}

void LevelsWidget::moveHandleTo(Handle h, const QPoint &pos)
{
    adjust::LevelsParams p = m_params;

    switch (h) {
    case Handle::InBlack: {
        // ★ 黑白场交叉由界面先挡住 ★
        //   引擎对交叉参数会做交换兜底（见 levelsLut 的注释），
        //   但若让用户拖出交叉再交给引擎交换，画面会"整体反转"一下，
        //   看起来像突发故障。界面直接钳住，用户感受到的是"顶住了"。
        const int lv = xToLevel(pos.x(), inHandleRow());
        p.inBlack = qBound(0, lv, p.inWhite - 1);
        break;
    }
    case Handle::InWhite: {
        const int lv = xToLevel(pos.x(), inHandleRow());
        p.inWhite = qBound(p.inBlack + 1, lv, 255);
        break;
    }
    case Handle::InGray:
        // 灰点走自己的反解路径（改 gamma，并置 m_grayDragLevel）
        setGrayFromX(xToLevel(pos.x(), inHandleRow()));
        return;
    case Handle::OutBlack: {
        const int lv = xToLevel(pos.x(), outHandleRow());
        p.outBlack = qBound(0, lv, p.outWhite - 1);
        break;
    }
    case Handle::OutWhite: {
        const int lv = xToLevel(pos.x(), outHandleRow());
        p.outWhite = qBound(p.outBlack + 1, lv, 255);
        break;
    }
    case Handle::None:
        return;
    }

    applyParams(p);
    emit levelsChanged();
}

void LevelsWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    const Handle h = hitTest(event->pos());
    if (h == Handle::None) {
        QWidget::mousePressEvent(event);
        return;
    }

    m_dragging = h;
    m_selected = h;
    setFocus(Qt::MouseFocusReason);

    // ★ 点哪儿手柄就跳到哪儿 ★
    //   色阶是"粗定位"工具，用户常直接点目标位置而不是从当前手柄拖过去。
    //   要求必须先精确压住三角才生效会很难用（三角只有 11px 宽）。
    moveHandleTo(h, event->pos());

    update();
    event->accept();
}

void LevelsWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging != Handle::None) {
        moveHandleTo(m_dragging, event->pos());
        return;
    }

    const Handle h = hitTest(event->pos());
    if (h != m_hover) {
        m_hover = h;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void LevelsWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging != Handle::None && event->button() == Qt::LeftButton) {
        m_dragging = Handle::None;

        // 松手后切回"由烘焙表反查位置"。拖动期间用的是目标值，
        // 若反解时 gamma 被钳制，两者会差一格 —— 这一步把它归位，
        // 顺带让"界面显示的参数"与"实际生效的参数"再次对齐。
        m_grayDragLevel = -1;

        update();
        emit editingFinished();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void LevelsWidget::keyPressEvent(QKeyEvent *event)
{
    int delta = 0;
    switch (event->key()) {
    case Qt::Key_Left:  delta = -1; break;
    case Qt::Key_Right: delta = +1; break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (event->modifiers() & Qt::ShiftModifier)
        delta *= 10;

    // 没有选中手柄时默认从黑场开始 —— 这样"Tab 进控件 → 按右方向键"
    // 也有明确行为，而不是静默什么都不发生。
    Handle h = m_selected;
    if (h == Handle::None)
        h = Handle::InBlack;

    if (h == Handle::InGray) {
        // 灰点微调：先由烘焙反查当前所在位置，再整体平移 delta 个色阶。
        // 用反查结果作起点，保证键盘与鼠标改的是同一个量。
        const int cur = xToLevel(grayHandleX(), inHandleRow());
        m_selected = h;
        setGrayFromX(qBound(m_params.inBlack + 1, cur + delta, m_params.inWhite - 1));

        emit editingFinished();
        event->accept();
        return;
    }

    adjust::LevelsParams p = m_params;
    switch (h) {
    case Handle::InBlack:
        p.inBlack = qBound(0, p.inBlack + delta, p.inWhite - 1);
        break;
    case Handle::InWhite:
        p.inWhite = qBound(p.inBlack + 1, p.inWhite + delta, 255);
        break;
    case Handle::OutBlack:
        p.outBlack = qBound(0, p.outBlack + delta, p.outWhite - 1);
        break;
    case Handle::OutWhite:
        p.outWhite = qBound(p.outBlack + 1, p.outWhite + delta, 255);
        break;
    case Handle::InGray:
    case Handle::None:
        break;
    }

    m_selected = h;
    applyParams(p);

    emit levelsChanged();
    emit editingFinished();
    event->accept();
}

void LevelsWidget::leaveEvent(QEvent *event)
{
    if (m_hover != Handle::None) {
        m_hover = Handle::None;
        update();
    }
    QWidget::leaveEvent(event);
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------

void LevelsWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter p(this);
    // 深色主题适配：控件底色从 palette 取（见匿名 namespace 注释）
    p.fillRect(rect(), palette().color(QPalette::Window));

    drawHistogram(p);
    drawOutputBar(p);
    drawSeparator(p);

    const Handle all[] = {
        Handle::InBlack, Handle::InGray, Handle::InWhite,
        Handle::OutBlack, Handle::OutWhite
    };
    for (Handle h : all)
        drawHandle(p, h);
}

void LevelsWidget::drawHistogram(QPainter &p)
{
    const QRect r = inputPlotRect();
    if (r.isEmpty())
        return;

    // 底色与外框先画 —— 即使直方图还没算出来（或操作的是空图层）也要有框，
    // 否则控件在视觉上会"塌掉"，用户以为是布局没展开。
    // 底色与外框先画 —— 即使直方图还没算出来（或操作的是空图层）也要有框，
    // 否则控件在视觉上会"塌掉"，用户以为是布局没展开。
    // 深色主题适配：绘图区底用 Base 角色（比控件底 Window 更"内容向"，
    // 浅色下 255、深色下 ~25，保留原来的两层分层）
    p.fillRect(r, palette().color(QPalette::Base));

    if (m_histogram.size() == 256) {
        const QVector<qreal> norm = histogram::normalized(m_histogram, true);
        if (!norm.isEmpty() && r.width() > 2 && r.height() > 4) {
            // 4 等分竖网格：判断"黑白场该落在哪"时的参考刻度
            // 深色主题适配：mix(Window, Mid, 0.2)，浅色 ≈224（原 236）
            p.setPen(QPen(mixWithMid(palette(), 0.20), 1.0));
            for (int i = 1; i < 4; ++i) {
                const int x = r.left() + r.width() * i / 4;
                p.drawLine(x, r.top() + 1, x, r.bottom() - 1);
            }

            // ★ 对数缩放 ★ 见 engine/Histogram.h：
            //   照片的直方图常集中在一两个亮度值上（大片天空、大面积白背景），
            //   线性缩放会把其余 99% 的柱子压成一条平线，看不出任何信息。
            QPainterPath path;
            path.moveTo(qreal(r.left()), qreal(r.bottom() - 1));
            for (int i = 0; i < 256; ++i) {
                const qreal x = r.left() + qreal(r.width() - 1) * i / 255.0;
                const qreal y = qreal(r.bottom() - 1)
                              - qreal(r.height() - 2) * norm[i];
                path.lineTo(x, y);
            }
            path.lineTo(qreal(r.right()), qreal(r.bottom() - 1));
            path.closeSubpath();

            // 刻意不开抗锯齿：直方图表达的是"柱状统计"，
            // 边缘被虚化后相邻的窄峰就糊在一起了。
            // 深色主题适配：柱子从 Text 派生 alpha（浅色近似 90@130，
            // 深色下变亮灰，柱子不会消失）
            p.fillPath(path, withAlpha(palette().color(QPalette::Text), 130));
        }
    }

    // ★ 被裁掉的两段压一层白雾 ★
    //   黑场左侧、白场右侧的像素在结果里会分别变成纯黑 / 纯白。
    //   把它们盖住，用户一眼就能看到自己丢掉了多少信息 ——
    //   比只在数值框里显示"黑场 = 20"直观得多。
    //   深色主题适配：雾色从 Base 派生 alpha（浅色下仍是白雾，
    //   深色下变成与绘图区同色的深雾，不会变成亮块）
    if (r.width() > 2) {
        const int xb = levelToX(m_params.inBlack, r);
        const int xw = levelToX(m_params.inWhite, r);
        const QColor veil = withAlpha(palette().color(QPalette::Base), 168);

        if (xb > r.left())
            p.fillRect(QRect(r.left(), r.top() + 1, xb - r.left(), r.height() - 2), veil);
        if (xw < r.right())
            p.fillRect(QRect(xw, r.top() + 1, r.right() - xw, r.height() - 2), veil);
    }

    // 深色主题适配：绘图区外框用 Mid（浅色 ≈160，与原 150 接近）
    p.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    p.drawRect(r.adjusted(0, 0, -1, -1));
}

void LevelsWidget::drawOutputBar(QPainter &p) const
{
    const QRect r = outBarRect();
    if (r.isEmpty())
        return;

    const QRect bar = r.adjusted(0, 2, 0, -2);
    if (bar.isEmpty())
        return;

    // ★ 渐变的两端就是输出黑白场 ★
    //   把输出白场拉到 200，条子右端立刻变暗 —— 这是"输出色阶"
    //   唯一直观的可视化，也是它值得占一行高度的原因。
    QLinearGradient g(qreal(bar.left()), 0.0, qreal(bar.right() + 1), 0.0);
    g.setColorAt(0.0, QColor(m_params.outBlack, m_params.outBlack, m_params.outBlack));
    g.setColorAt(1.0, QColor(m_params.outWhite, m_params.outWhite, m_params.outWhite));
    p.fillRect(bar, g);

    // 深色主题适配：描边用 Mid（浅色 ≈160，与原 150 接近）
    p.setPen(QPen(palette().color(QPalette::Mid), 1.0));
    p.drawRect(bar.adjusted(0, 0, -1, -1));
}

void LevelsWidget::drawSeparator(QPainter &p) const
{
    const RowMetrics m = metricsFor(height());
    const int y = kMargin + m.plotH + 1 + m.handleH + m.gapH / 2;
    if (y <= 0 || y >= height())
        return;

    // 深色主题适配：mix(Window, Mid, 0.15)，浅色 ≈228（原 225）
    p.setPen(QPen(mixWithMid(palette(), 0.15), 1.0));
    p.drawLine(kMargin, y, width() - kMargin, y);
}

void LevelsWidget::drawHandle(QPainter &p, Handle h) const
{
    const QRect r = handleRect(h);
    if (r.isEmpty() || r.height() < 3)
        return;

    const bool active = (h == m_hover) || (h == m_dragging);

    // 颜色沿用 Photoshop 的约定：黑场黑、灰点中灰、白场白。
    // 用户对这三个颜色的含义有现成的直觉，换成统一配色反而要重新学。
    QColor fill;
    switch (h) {
    case Handle::InBlack:
    case Handle::OutBlack:
        fill = QColor(40, 40, 40);
        break;
    case Handle::InWhite:
    case Handle::OutWhite:
        fill = QColor(245, 245, 245);
        break;
    case Handle::InGray:
        fill = QColor(128, 128, 128);
        break;
    case Handle::None:
        return;
    }
    if (active)
        fill = QColor(86, 138, 220);   // 悬停 / 拖动统一高亮

    QPolygon tri;
    tri << QPoint(r.center().x(), r.top() + 1)
        << QPoint(r.left(),        r.bottom())
        << QPoint(r.right(),       r.bottom());

    p.setRenderHint(QPainter::Antialiasing, true);
    // 深色主题适配：描边与 WindowText 混一半 —— 浅色底得深边（原 130/60），
    // 深色底得亮边。否则黑手柄(40)在深色底(~53)上会找不到轮廓。
    p.setPen(QPen(mixColors(fill, palette().color(QPalette::WindowText), 0.5), 1.0));
    p.setBrush(fill);
    p.drawPolygon(tri);

    // 灰点手柄加一条竖脊：手柄行只有十几像素高，
    // 中灰与深灰在小尺寸下不易分辨，竖脊让"这是中间那个"一目了然。
    if (h == Handle::InGray && r.height() >= 8) {
        p.setPen(QPen(QColor(255, 255, 255), 1.0));
        const int cx = r.center().x();
        p.drawLine(cx, r.top() + 4, cx, r.bottom() - 2);
    }

    p.setRenderHint(QPainter::Antialiasing, false);
}

} // namespace zl
