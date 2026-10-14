#include "tools/ZoomTool.h"

#include "tools/ToolOptions.h"
#include "tools/ToolParams.h"

#include <QPainter>

namespace zl {

// ============================================================================
//  构造 / 身份
// ============================================================================
ZoomTool::ZoomTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
{
    // ★ 复用已有的 ZoomOptions ★
    //   它在阶段二就写好了（选项条上有"放大 / 缩小"两个互斥按钮，
    //   外加一句"按住 Alt 可临时切换到相反方向"的提示）。
    //   本工具要做的是把那条提示**兑现**，而不是另起一套参数。
    m_options = new ZoomOptions(this);
}

ZoomTool::~ZoomTool() = default;

QString ZoomTool::id() const
{
    // ★ QSettings 的键名，发布后不要再改 ★
    return QStringLiteral("zoom");
}

QString ZoomTool::displayName() const
{
    return tr("Zoom");
}

int ZoomTool::shortcutKey() const
{
    // Z = Zoom，与 Photoshop 一致
    return Qt::Key_Z;
}

ToolOptions *ZoomTool::options() const
{
    return m_options;
}

bool ZoomTool::zoomsIn(bool altDown) const
{
    const bool base = m_options ? m_options->zoomIn() : true;

    // ★ 用 != 表达"取反"，而不是 if (alt) return !base ★
    //   两者结果一样，但 != 一眼就能看出"无论如何都在 Alt 下反转"。
    //   写成 if 分支的话，将来加第三个方向（例如"缩放到选区"）时
    //   很容易漏掉 Alt 那一支，而漏掉的表现只是"Alt 不灵了"，
    //   没有任何报错。  bool 的 != 就是 XOR。
    return base != altDown;
}

// ============================================================================
//  框选矩形
// ============================================================================

QRectF ZoomTool::marqueeImageRect() const
{
    // normalized() 负责"从右下往左上拖"这种反向拖动的规范化。
    // 不归一化的话得到的会是负宽高的矩形，zoomToRect 直接判 isEmpty 走人 ——
    // 症状是"只有从左上往右下拖才有效"，用户会以为框选缩放是碰运气。
    return QRectF(m_pressImagePos, m_currentImagePos).normalized();
}

QRectF ZoomTool::marqueeWidgetRect() const
{
    return QRectF(m_pressWidgetPos, m_currentWidgetPos).normalized();
}

// ============================================================================
//  输入事件
// ============================================================================

ToolResult ZoomTool::mousePress(const ToolEvent &e)
{
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    m_pressed = true;
    m_pressWidgetPos   = e.widgetPos;
    m_pressImagePos    = e.imagePos;
    m_currentWidgetPos = e.widgetPos;
    m_currentImagePos  = e.imagePos;

    // ★ 每次按下都把框选状态清掉 ★
    //   上一次的框如果留下来，这一次"点一下"的松手就会被判成框选，
    //   而且框还是上一次的位置 —— 一个极难复现的错位 bug。
    m_marqueeActive = false;

    setBusy(true);
    return ToolResult::accepted();
}

ToolResult ZoomTool::mouseMove(const ToolEvent &e)
{
    if (!m_pressed)
        return ToolResult::ignored();

    m_currentWidgetPos = e.widgetPos;
    m_currentImagePos  = e.imagePos;

    if (!m_marqueeActive) {
        // ★ 阈值用控件坐标 ★ —— 理由见头文件（用图像坐标的话，
        //   放大到 800% 时永远触发不了框选）。
        //   取 |dx| 与 |dy| 的较大者而不是欧氏距离：用户拖的是矩形，
        //   "这个框够不够大"本来就该看它最长的那条边。
        const QPointF d = m_currentWidgetPos - m_pressWidgetPos;
        if (qMax(qAbs(d.x()), qAbs(d.y())) < marqueeThreshold())
            return ToolResult::accepted();       // 还在"手抖"范围内，先当作没拖

        m_marqueeActive = true;
    }

    // 框选过程中每次移动都要重画虚线框
    return ToolResult::repaint();
}

ToolResult ZoomTool::mouseRelease(const ToolEvent &e)
{
    if (!m_pressed)
        return ToolResult::ignored();

    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    m_currentWidgetPos = e.widgetPos;
    m_currentImagePos  = e.imagePos;

    m_pressed = false;
    setBusy(false);

    if (m_marqueeActive) {
        const QRectF rect = marqueeImageRect();
        m_marqueeActive = false;

        // ★ 退化判定在**图像坐标**里做 ★
        //   框选缩放的产物是"图像上的一个区域"，所以"这个框有没有意义"
        //   只能在图像坐标里量。低缩放下拖 6 个控件像素只有零点几个图像
        //   像素，交给 zoomToRect 会得到几千倍 —— 必须挡在这里。
        if (rect.width() >= 1.0 && rect.height() >= 1.0) {
            m_lastZoomToRect = rect;
            ++m_marqueeZoomCount;
            emit requestZoomToRect(rect);
            return ToolResult::repaint();
        }

        // 退化了 → 落到下面的"单击缩放"，而不是无声返回。
        // 用户拖了一下却什么都没发生，会被当成卡死。
    }

    // ── 单击：以按下点为锚点缩放一档 ──
    //
    // ★ 锚点用**按下点**而不是松手点 ★
    //   用户瞄准的是按下那一刻看到的位置；用松手点的话，
    //   手抖几个像素就会让画面往旁边偏一点，连续点击会累积漂移。
    const qreal factor = zoomsIn(e.modifiers.alt) ? kZoomStepFactor
                                                  : (1.0 / kZoomStepFactor);

    m_lastZoomFactor = factor;
    m_lastZoomAnchor = m_pressWidgetPos;
    ++m_clickZoomCount;
    emit requestZoom(factor, m_pressWidgetPos);

    return ToolResult::repaint();
}

// ============================================================================
//  叠加层：框选的虚线框
// ============================================================================

void ZoomTool::paintScreenOverlay(QPainter &painter, CanvasView *view)
{
    // ★ 这里**故意**不写 `if (!view) return;` ★
    //
    //   本工具画这个框只需要"按下点与当前点的控件坐标"，那两个值来自
    //   ToolEvent，不需要回头去问画布。ColorPickerTool 那边的空指针检查是
    //   防御性写法，但它的代价是"叠加层只有在一个真窗口里才测得到"。
    //   而铁律要求每条自绘管线都有一条"量它画出来的像素"的断言 ——
    //   为了那条断言，这里保持"不依赖 view"。
    //   （所以请不要"顺手"把这个判断补上：它会让 tests/hand_zoom_test.cpp
    //     里那组量像素的断言整组失效，而且不报错。）
    Q_UNUSED(view)

    if (!m_marqueeActive)
        return;

    const QRectF box = marqueeWidgetRect();
    if (box.width() < 1.0 || box.height() < 1.0)
        return;

    painter.setBrush(Qt::NoBrush);

    // 黑白双线，任何底色上都至少有一条看得见（与吸管取样框、画笔轮廓同款）。
    // ★ 线宽固定 1.0 ★ —— paintScreenOverlay 是在 resetTransform() 之后调的，
    //   画笔宽度天然就是屏幕像素，不需要再除以 zoom。
    painter.setPen(QPen(QColor(0, 0, 0, 200), 1.0));
    painter.drawRect(box);
    painter.setPen(QPen(QColor(255, 255, 255, 220), 1.0));
    painter.drawRect(box.adjusted(-1.0, -1.0, 1.0, 1.0));
}

// ============================================================================
//  收尾
// ============================================================================

void ZoomTool::cancel()
{
    m_pressed = false;
    m_marqueeActive = false;

    // ★ 坐标也要清空 ★
    //   不清的话 marqueeImageRect() 会一直返回"上一次那个框"，
    //   而 cancel() 的语义就是"当作没发生过"。
    m_pressWidgetPos   = QPointF();
    m_pressImagePos    = QPointF();
    m_currentWidgetPos = QPointF();
    m_currentImagePos  = QPointF();

    AbstractTool::cancel();
}

} // namespace zl
