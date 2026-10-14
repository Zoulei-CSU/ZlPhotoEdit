// ============================================================================
//  CurveEditor.cpp —— 曲线控件的实现
// ----------------------------------------------------------------------------
//  绘制与交互的细节依据 docs/曲线调整设计.md 第 4 节。
//  与文档的一处差异（y 值不允许超出 0..255）已写在头注释里，理由见那里。
// ============================================================================

#include "ui/CurveEditor.h"
#include "compat/QtCompat.h"
#include "engine/Histogram.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>

namespace zl {

namespace {

// 绘图区外的留白。留白给数值框与端点圆圈让位 ——
// 端点贴边画会被裁掉一半。
constexpr int kMargin = 12;

// 命中半径（控件像素）
constexpr int kHitTolerance = 7;

} // namespace

CurveEditor::CurveEditor(QWidget *parent)
    : QWidget(parent)
{
    // 需要鼠标移动事件来更新数值框与悬停高亮
    setMouseTracking(true);

    // ★ 必须能接收键盘焦点 ★
    //   方向键微调、Delete 删点都依赖它。默认的 NoFocus 会让
    //   "按上下键微调"完全没反应，而且是那种没有任何报错、看起来
    //   像"这个功能没实现"的静默失效。
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);

    ensureEndpoints();
    rebake();
}

// ---------------------------------------------------------------------------
//  QWidget 接口
// ---------------------------------------------------------------------------

QSize CurveEditor::sizeHint() const
{
    return QSize(300, 300);
}

QSize CurveEditor::minimumSizeHint() const
{
    return QSize(180, 180);
}

// ---------------------------------------------------------------------------
//  曲线数据（与 engine 的 Curve 互转）
// ---------------------------------------------------------------------------

void CurveEditor::setCurve(const Curve &curve)
{
    m_nodes.clear();

    const QList<CurvePoint> pts = curve.points();
    for (int i = 0; i < pts.size(); ++i) {
        CurveNode n;
        n.x = qBound(0, int(qRound(pts[i].x * 255.0)), 255);
        n.y = qBound(0, int(qRound(pts[i].y * 255.0)), 255);

        // 去重：Curve 本身已经规范化过（x 严格递增），这里只是防御 ——
        // 同 x 的两个点会让"单值函数"的前提失效。
        if (!m_nodes.isEmpty() && m_nodes.last().x >= n.x)
            continue;

        m_nodes.append(n);
    }

    ensureEndpoints();
    m_selected   = -1;
    m_hoverIndex = -1;
    rebake();
    update();
}

Curve CurveEditor::curve() const
{
    QList<CurvePoint> pts;
    pts.reserve(m_nodes.size());
    for (int i = 0; i < m_nodes.size(); ++i) {
        pts.append(CurvePoint(m_nodes[i].x / 255.0, m_nodes[i].y / 255.0));
    }
    // Curve 的构造函数会再规范化一次（排序、补端点、校验 x 递增），
    // 因此这里即便有轻微不合规也不会传下去变成非法曲线。
    return Curve(pts);
}

bool CurveEditor::isIdentity() const
{
    return curve().isIdentity();
}

// ---------------------------------------------------------------------------
//  显示选项
// ---------------------------------------------------------------------------

void CurveEditor::setHistogram(const QVector<int> &hist)
{
    m_histogram = hist;
    update();
}

void CurveEditor::setSmooth(bool on)
{
    if (m_smooth == on)
        return;
    m_smooth = on;
    rebake();
    update();
}

void CurveEditor::setCurveColor(const QColor &c)
{
    m_curveColor = c;
    update();
}

// ---------------------------------------------------------------------------
//  坐标换算
// ---------------------------------------------------------------------------

QRect CurveEditor::plotRect() const
{
    const QRect r = rect();
    if (r.width() <= 2 * kMargin || r.height() <= 2 * kMargin)
        return QRect();   // 尺寸过小（还没布局），交给调用方做空判断

    return r.adjusted(kMargin, kMargin, -kMargin, -kMargin);
}

QPoint CurveEditor::nodeToWidget(int x, int y) const
{
    const QRect r = plotRect();
    if (r.isEmpty())
        return QPoint();

    // ★ 这里是"亮度向上"与"控件 y 向下"的唯二翻转点之一 ★
    return QPoint(r.left() + qRound(r.width()  * x / 255.0),
                  r.top()  + qRound(r.height() * (255 - y) / 255.0));
}

QPoint CurveEditor::widgetToNode(const QPoint &w) const
{
    const QRect r = plotRect();
    if (r.isEmpty())
        return QPoint(0, 0);

    const qreal u = qreal(w.x() - r.left())  / r.width();
    const qreal v = qreal(r.bottom() - w.y()) / r.height();

    return QPoint(qBound(0, int(qRound(u * 255.0)), 255),
                  qBound(0, int(qRound(v * 255.0)), 255));
}

int CurveEditor::hitTest(const QPoint &w, int tolerancePx) const
{
    int best = -1;
    int bestDist = tolerancePx + 1;

    for (int i = 0; i < m_nodes.size(); ++i) {
        const QPoint p = nodeToWidget(m_nodes[i].x, m_nodes[i].y);
        // 用切比雪夫距离（max(|dx|,|dy|)）而不是欧氏距离：
        // 命中区是一个正方形，在"斜着拖动"时手感更均匀，
        // 也避免开平方。
        const int d = qMax(qAbs(p.x() - w.x()), qAbs(p.y() - w.y()));
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
//  编辑操作
// ---------------------------------------------------------------------------

void CurveEditor::ensureEndpoints()
{
    if (m_nodes.isEmpty()) {
        m_nodes.append(CurveNode(0, 0));
        m_nodes.append(CurveNode(255, 255));
        return;
    }

    // 端点必须存在。缺失时补"恒等"端点而不是补 0 ——
    // 补 0 会让曲线在端点多出一段垂直的陡坡，看起来像是用户自己调的。
    if (m_nodes.first().x != 0)
        m_nodes.prepend(CurveNode(0, 0));
    if (m_nodes.last().x != 255)
        m_nodes.append(CurveNode(255, 255));
}

int CurveEditor::insertNode(const CurveNode &n)
{
    ensureEndpoints();

    // x == 0 / x == 255 已被端点占住，不可能再插入 ——
    // 强行插入会让插值出现 Δx == 0。
    if (n.x <= 0)
        return 0;
    if (n.x >= 255)
        return m_nodes.size() - 1;

    for (int i = 0; i < m_nodes.size(); ++i) {
        if (m_nodes[i].x == n.x) {
            // 同 x 已有点：只改 y。
            // 曲线必须是单值函数，同一个输入不可能有两个输出。
            m_nodes[i].y = n.y;
            return i;
        }
        if (m_nodes[i].x > n.x) {
            m_nodes.insert(i, n);
            return i;
        }
    }

    m_nodes.append(n);
    return m_nodes.size() - 1;
}

bool CurveEditor::removeNode(int index)
{
    // 端点不可删 —— 少了端点，曲线在 0 与 255 附近就没有定义，
    // 烘焙时只能靠外推，行为会变得难以解释。
    if (index <= 0 || index >= m_nodes.size() - 1)
        return false;

    m_nodes.remove(index);
    return true;
}

bool CurveEditor::moveNode(int index, const CurveNode &n)
{
    if (index < 0 || index >= m_nodes.size())
        return false;

    const int last = m_nodes.size() - 1;

    int nx = n.x;
    const int ny = qBound(0, n.y, 255);

    if (index == 0) {
        nx = 0;                 // 起点只能上下移动
    } else if (index == last) {
        nx = 255;               // 终点同理
    } else {
        // ★ 相邻点 x 间隔必须 ≥ 1 ★
        //   否则 Δx 为 0，Fritsch–Carlson 里算差商的那一步就是除零，
        //   NaN 会顺着切线一路污染整条曲线（表现为曲线整段消失）。
        const int lo = m_nodes[index - 1].x + 1;
        const int hi = qMax(lo, m_nodes[index + 1].x - 1);
        nx = qBound(lo, nx, hi);
    }

    if (m_nodes[index].x == nx && m_nodes[index].y == ny)
        return false;   // 没有实际变化：不必重绘，更不必重算预览

    m_nodes[index] = CurveNode(nx, ny);
    return true;
}

void CurveEditor::rebake()
{
    // ★ 全工程只有这一处烘焙 ★
    //   绘制（drawCurve）、数值框（drawInfoBox）、以及 CurvesDialog
    //   生成最终 LUT，全部读这同一份 m_bakedLut / 同一套 bake()。
    m_bakedLut = curve().bake(m_smooth);
}

// ---------------------------------------------------------------------------
//  输入事件
// ---------------------------------------------------------------------------

void CurveEditor::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);

    const QPoint pos = eventPos(event).toPoint();

    // ── 右键：删除控制点 ──
    if (event->button() == Qt::RightButton) {
        const int idx = hitTest(pos);
        if (idx >= 0 && removeNode(idx)) {
            m_selected   = -1;
            m_hoverIndex = -1;
            rebake();
            update();
            emit curveChanged();
            emit editingFinished();   // 删除是一次完整编辑，立即结算
        }
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // ── 左键：点在点上则拖动它，点在空白处则新增一个点并直接进入拖动 ──
    int idx = hitTest(pos);
    if (idx < 0) {
        const QPoint cv = widgetToNode(pos);
        idx = insertNode(CurveNode(cv.x(), cv.y()));
        rebake();
        emit curveChanged();
    }

    m_selected = idx;
    m_dragging = true;
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = eventPos(event).toPoint();

    m_cursorPos = pos;
    m_hasCursor = true;

    if (m_dragging && m_selected >= 0) {
        const QPoint cv = widgetToNode(pos);
        if (moveNode(m_selected, CurveNode(cv.x(), cv.y()))) {
            rebake();
            emit curveChanged();   // 实时预览：主窗口收到后重算降采样预览
        }
    } else {
        const int hover = hitTest(pos);
        if (hover != m_hoverIndex) {
            m_hoverIndex = hover;
            setCursor(hover >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        }
    }

    // 数值框跟着光标走，所以每次移动都要重绘。
    // 控件本身只有几百像素见方，重绘成本可以忽略。
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent *event)
{
    // ★ 不按按钮过滤 ★
    //   只判断 m_dragging —— 它只可能在成功按下左键后为 true。
    //   如果在拖动中收到一个"非左键"的释放事件（误触右键、
    //   触摸板的合成事件等），按按钮过滤会让 m_dragging 永远留在 true，
    //   之后鼠标一动就在拖点，控件彻底失控。
    //   宁可多结算一次编辑，也不能卡在拖动状态里。
    //   （这个教训来自阶段三的选区工具，同一个坑。）
    if (m_dragging) {
        m_dragging = false;
        emit editingFinished();
    }

    QWidget::mouseReleaseEvent(event);
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    // 双击落在已有的点上：不做任何事。
    // （不把"双击删点"这种危险操作挂在这里 —— 用户想拖动时手抖双击
    //   就会丢点，而丢点是无法凭记忆恢复的。）
    const QPoint pos = eventPos(event).toPoint();
    if (hitTest(pos) >= 0) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }

    // 空白处双击：等价于"新增一个点"。
    // 实际上按下时已经加过了，这里再走一遍是幂等的（同 x 只改 y），
    // 保留它是为了让行为在"按下→双击"这个序列被系统合并时也成立。
    const QPoint cv = widgetToNode(pos);
    m_selected = insertNode(CurveNode(cv.x(), cv.y()));
    rebake();
    update();
    emit curveChanged();
    emit editingFinished();
}

void CurveEditor::leaveEvent(QEvent *event)
{
    m_hasCursor = false;
    m_hoverIndex = -1;
    update();
    QWidget::leaveEvent(event);
}

void CurveEditor::keyPressEvent(QKeyEvent *event)
{
    if (m_selected < 0 || m_selected >= m_nodes.size()) {
        QWidget::keyPressEvent(event);
        return;
    }

    // Shift 加速：微调用 1，粗调用 10。
    // 曲线只有 256 级，用 1 才能精确压到某个值 ——
    // 所以默认必须是 1，而不是"看起来更方便"的 5。
    const int step = (event->modifiers() & Qt::ShiftModifier) ? 10 : 1;

    CurveNode n = m_nodes[m_selected];

    switch (event->key()) {
    case Qt::Key_Up:    n.y += step; break;
    case Qt::Key_Down:  n.y -= step; break;
    case Qt::Key_Left:  n.x -= step; break;
    case Qt::Key_Right: n.x += step; break;

    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (removeNode(m_selected)) {
            m_selected = -1;
            rebake();
            update();
            emit curveChanged();
            emit editingFinished();
        }
        return;

    default:
        QWidget::keyPressEvent(event);
        return;
    }

    if (moveNode(m_selected, n)) {
        rebake();
        update();
        emit curveChanged();
        emit editingFinished();
    }
}

// ---------------------------------------------------------------------------
//  绘制
// ---------------------------------------------------------------------------

void CurveEditor::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter p(this);
    p.fillRect(rect(), QColor(252, 252, 252));

    drawGrid(p);
    drawHistogram(p);
    drawCurve(p);
    drawNodes(p);
    drawInfoBox(p);
}

void CurveEditor::drawGrid(QPainter &p)
{
    const QRect r = plotRect();
    if (r.isEmpty())
        return;

    // 4×4 等分网格：让 1/4、1/2、3/4 这三个用户最常对的位置可读
    p.setPen(QPen(QColor(228, 228, 228), 1.0));
    for (int i = 1; i < 4; ++i) {
        const int x = r.left() + r.width()  * i / 4;
        const int y = r.top()  + r.height() * i / 4;
        p.drawLine(x, r.top(), x, r.bottom());
        p.drawLine(r.left(), y, r.right(), y);
    }

    // 对角参考线 = 恒等线（输出 ≡ 输入）
    // 控件坐标里左下角是 (0,0)、右上角是 (255,255)，所以这条线从
    // 左下走到右上。有了它，用户一眼就能看出曲线是被整体抬高还是压低了。
    p.setPen(QPen(QColor(186, 186, 186), 1.0, Qt::DashLine));
    p.drawLine(r.bottomLeft(), r.topRight());

    // 外框
    p.setPen(QPen(QColor(160, 160, 160), 1.0));
    p.drawRect(r.adjusted(0, 0, -1, -1));
}

void CurveEditor::drawHistogram(QPainter &p)
{
    if (m_histogram.size() != 256)
        return;

    const QRect r = plotRect();
    if (r.isEmpty())
        return;

    // ★ 对数缩放 ★ 见 engine/Histogram.h 的说明：
    //   线性缩放时，一张"大片天空"的照片只会有一根高柱加一条平线。
    const QVector<qreal> norm = histogram::normalized(m_histogram, true);
    if (norm.isEmpty())
        return;

    QPainterPath path;
    path.moveTo(r.bottomLeft());
    for (int i = 0; i < 256; ++i) {
        const qreal x = r.left() + r.width() * i / 255.0;
        const qreal y = r.bottom() - r.height() * norm[i];
        path.lineTo(x, y);
    }
    path.lineTo(r.bottomRight());
    path.closeSubpath();

    // 半透明灰填充：作为"背景信息"，绝不能盖过曲线本身
    p.fillPath(path, QColor(0, 0, 0, 26));
}

void CurveEditor::drawCurve(QPainter &p)
{
    const QRect r = plotRect();
    if (r.isEmpty())
        return;

    // ★ 逐点连线的数据来自 m_bakedLut ★
    //   也就是"实际查表用的那张表"。这里绝不自己算插值 ——
    //   一旦自己算，就会出现"画出来的曲线和实际映射不一样"。
    QPainterPath path;
    for (int i = 0; i < 256; ++i) {
        const QPoint pt = nodeToWidget(i, int(m_bakedLut[size_t(i)]));
        if (i == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(m_curveColor, 2.0));
    p.drawPath(path);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void CurveEditor::drawNodes(QPainter &p)
{
    p.setRenderHint(QPainter::Antialiasing, true);

    for (int i = 0; i < m_nodes.size(); ++i) {
        const QPoint c = nodeToWidget(m_nodes[i].x, m_nodes[i].y);
        const bool isEndpoint = (i == 0 || i == m_nodes.size() - 1);

        // 端点画小一点并填成深色，与可增删的中间点区分开
        const int radius = isEndpoint ? 3 : 4;

        p.setPen(QPen(m_curveColor, 1.5));
        p.setBrush(isEndpoint ? m_curveColor : QColor(255, 255, 255));
        p.drawEllipse(c, radius, radius);

        // 选中 / 悬停的提示环
        if (i == m_selected || i == m_hoverIndex) {
            p.setPen(QPen(QColor(m_curveColor.red(), m_curveColor.green(),
                                 m_curveColor.blue(), 120), 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, radius + 3, radius + 3);
        }
    }

    p.setRenderHint(QPainter::Antialiasing, false);
}

void CurveEditor::drawInfoBox(QPainter &p)
{
    const QRect r = plotRect();
    if (r.isEmpty())
        return;

    // 没有光标但有选中点时（键盘微调），用选中点的位置显示，
    // 否则用户按方向键看不到任何数值反馈。
    QPoint anchor;
    int input = 0;
    int output = 0;

    if (m_hasCursor) {
        anchor = m_cursorPos;
        // 输入取光标所在的亮度值，输出取该输入在**实际 LUT** 上的值 ——
        // 显示"当前这一竖列上曲线把输入映射成了什么"，比只显示点坐标更有用。
        const QPoint cv = widgetToNode(m_cursorPos);
        input = cv.x();
        output = int(m_bakedLut[size_t(qBound(0, input, 255))]);
    } else if (m_selected >= 0 && m_selected < m_nodes.size()) {
        input = m_nodes[m_selected].x;
        output = m_nodes[m_selected].y;
        anchor = nodeToWidget(input, output);
    } else {
        return;
    }

    const QString text = tr("Input %1  Output %2").arg(input).arg(output);

    p.setFont(font());
    const QFontMetrics fm = p.fontMetrics();
    const int pad = 4;
    const QSize textSize = fm.size(Qt::TextSingleLine, text);
    QSize boxSize(textSize.width() + 2 * pad, textSize.height() + 2 * pad);

    // 跟随光标，但整个框必须留在控件内 —— 否则贴边时光标在右侧，
    // 数值框会被裁掉一半，用户反而看不到数字。
    QPoint topLeft = anchor + QPoint(12, 12);
    topLeft.setX(qBound(0, topLeft.x(), qMax(0, width()  - boxSize.width())));
    topLeft.setY(qBound(0, topLeft.y(), qMax(0, height() - boxSize.height())));

    const QRect box(topLeft, boxSize);
    p.fillRect(box, QColor(255, 255, 255, 235));
    p.setPen(QPen(QColor(150, 150, 150), 1.0));
    p.drawRect(box.adjusted(0, 0, -1, -1));
    p.setPen(QColor(60, 60, 60));
    p.drawText(box, Qt::AlignCenter, text);
}

} // namespace zl
