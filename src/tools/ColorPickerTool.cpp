#include "tools/ColorPickerTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"

#include <QImage>
#include <QPainter>

namespace zl {

// ===========================================================================
//  构造 / 身份
// ===========================================================================
ColorPickerTool::ColorPickerTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
{
    m_options = new PickerOptions(this);

    // 取样范围变了 → 取样框的大小跟着变，重画一次叠加层。
    // 之所以要连这个信号：用户把范围从"点"改成"5×5"之后如果不重绘，
    // 方框还是 1 像素大，与"实际取 5×5"不符。
    connect(m_options, &ToolOptions::changed, this, [this]() {
        if (m_hoverValid && !m_hoverStale)
            requestRepaint();
    });
}

ColorPickerTool::~ColorPickerTool() = default;

QString ColorPickerTool::id() const
{
    return QStringLiteral("eyedropper");
}

QString ColorPickerTool::displayName() const
{
    return tr("吸管");
}

int ColorPickerTool::shortcutKey() const
{
    // Photoshop 的键位：I = 吸管。用户有肌肉记忆。
    return Qt::Key_I;
}

ToolOptions *ColorPickerTool::options() const
{
    return m_options;
}

// ===========================================================================
//  取色
// ===========================================================================
QColor ColorPickerTool::pickAt(const QPoint &imagePos) const
{
    if (!isReady())
        return QColor();

    Document *doc = document();
    const QRect canvas = doc->canvasRect();
    if (!canvas.contains(imagePos))
        return QColor();        // 画布外：取不到颜色（返回无效色）

    // ★ 取合成图，不取活动图层 ★
    //   理由见头文件。注意 composite() 是有缓存的，文档没变时
    //   这一步只是返回缓存引用，不付出重新合成的代价。
    const QImage comp = doc->composite();
    if (comp.isNull() || !comp.rect().contains(imagePos))
        return QColor();

    // 取样边长（1 / 3 / 5）。平均的是**预乘分量**，理由见头文件。
    const int size = qMax(1, pickRadiusSize(m_options ? m_options->radius()
                                                      : PickRadius::Point));
    const int half = size / 2;

    qint64 sumR = 0, sumG = 0, sumB = 0, sumA = 0;
    int n = 0;

    for (int dy = -half; dy <= half; ++dy) {
        const int y = imagePos.y() + dy;
        if (y < 0 || y >= comp.height())
            continue;
        // 逐扫描线读，不用 QImage::pixel() —— 后者对预乘格式会做
        // 反预乘转换，把我们要的预乘分量又变回去了。
        const QRgb *line = reinterpret_cast<const QRgb *>(comp.constScanLine(y));
        for (int dx = -half; dx <= half; ++dx) {
            const int x = imagePos.x() + dx;
            if (x < 0 || x >= comp.width())
                continue;
            const QRgb px = line[x];
            sumR += qRed(px);
            sumG += qGreen(px);
            sumB += qBlue(px);
            sumA += qAlpha(px);
            ++n;
        }
    }

    if (n == 0)
        return QColor();

    const int avgA = int((sumA + n / 2) / n);

    // ★ 全透明区域：直接把 alpha 交出去，不去还原颜色 ★
    //   预乘空间里 A=0 意味着 RGB 也是 0，"还原颜色"要除以 0。
    //   硬除的话会得到一个巨大值再被钳到 255（纯白），
    //   于是"取一块透明区域"会得到纯白 —— 一个凭空冒出来的错误颜色。
    if (avgA == 0)
        return QColor(0, 0, 0, 0);

    // 预乘分量的平均 × 255 / avgA = 原始颜色的平均
    const auto unPremul = [avgA](qint64 sum, int count) -> int {
        const qreal avgPremul = qreal(sum) / count;          // = avg(R·A/255)
        const qreal value = avgPremul * 255.0 / avgA;
        return qBound(0, int(qRound(value)), 255);
    };

    const int r = unPremul(sumR, n);
    const int g = unPremul(sumG, n);
    const int b = unPremul(sumB, n);

    // ★ 用 QColor(r,g,b,a) 而不是 QColor(qRgba(...)) ★
    //   QColor(QRgb) 那个构造函数会把 alpha 丢掉、置成 255
    //   （实测确认，见 tests/fill_test.cpp 的同名断言）。
    //   于是"取一个半透明像素"会得到不透明色，且毫无征兆。
    return QColor(r, g, b, avgA);
}

void ColorPickerTool::doPick(const ToolEvent &e)
{
    const QPoint pos = e.imagePoint();
    const QColor c = pickAt(pos);
    if (!c.isValid())
        return;                 // 画布外：静默忽略，不改前景色

    m_lastColor = c;
    m_lastPos = pos;
    ++m_pickCount;

    // Alt → 取背景色（Photoshop 的约定），否则取前景色。
    if (e.modifiers.alt)
        emit requestBackgroundColor(c);
    else
        emit requestForegroundColor(c);

    showStatus(tr("已取样 %1  (%2, %3, %4)  α%5")
                   .arg(c.name(QColor::HexRgb))
                   .arg(c.red()).arg(c.green()).arg(c.blue())
                   .arg(c.alpha()));
}

// ===========================================================================
//  输入事件
// ===========================================================================
ToolResult ColorPickerTool::mousePress(const ToolEvent &e)
{
    if (!e.isPress() || e.button != Qt::LeftButton)
        return ToolResult::ignored();
    if (!isReady())
        return ToolResult::ignored();

    m_pressed = true;

    // 按下的那一刻也更新一次悬停缓存，否则取样框会停在旧位置
    m_hoverWidgetPos = e.widgetPos;
    m_hoverViewZoom  = e.viewZoom;
    m_hoverValid     = true;
    m_hoverStale     = false;

    doPick(e);
    return ToolResult::repaint();
}

ToolResult ColorPickerTool::mouseMove(const ToolEvent &e)
{
    // 拖动中持续取色 —— 这是各绘图软件的通用手感：
    // 按住不放拖到满意的地方松手，比"点一下看一眼、不满意再来一次"快得多。
    if (!m_pressed || e.buttons == Qt::NoButton)
        return ToolResult::ignored();

    m_hoverWidgetPos = e.widgetPos;
    m_hoverViewZoom  = e.viewZoom;
    m_hoverStale     = false;

    doPick(e);
    return ToolResult::repaint();
}

ToolResult ColorPickerTool::mouseRelease(const ToolEvent &e)
{
    if (!m_pressed)
        return ToolResult::ignored();
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    m_pressed = false;
    return ToolResult::accepted();
}

ToolResult ColorPickerTool::hoverMove(const ToolEvent &e)
{
    const bool moved = m_hoverStale || !m_hoverValid
                       || qAbs(e.widgetPos.x() - m_hoverWidgetPos.x()) > 0.01
                       || qAbs(e.widgetPos.y() - m_hoverWidgetPos.y()) > 0.01;

    m_hoverWidgetPos = e.widgetPos;
    m_hoverViewZoom  = e.viewZoom;
    m_hoverValid     = true;
    m_hoverStale     = false;

    if (!moved)
        return ToolResult::accepted();

    requestRepaint();
    return ToolResult::repaint();
}

ToolResult ColorPickerTool::keyPress(const ToolEvent &e)
{
    // 按住 Alt 时"临时切到背景色"由 doPick 里的 modifiers 判断，
    // 不需要在这里维护状态。这里只处理 Esc —— 取消本次取色
    // （把最近一次取到的东西清掉，避免自检误判）。
    if (e.key == Qt::Key_Escape && m_pressed) {
        m_pressed = false;
        return ToolResult::accepted();
    }
    return ToolResult::ignored();
}

// ===========================================================================
//  叠加层
// ===========================================================================
void ColorPickerTool::paintScreenOverlay(QPainter &painter, CanvasView *view)
{
    if (!view || !m_hoverValid || m_hoverStale)
        return;

    // 取样范围的边长（1 / 3 / 5 像素）× 缩放 → 屏幕上恒定大小。
    const int size = qMax(1, pickRadiusSize(m_options ? m_options->radius()
                                                      : PickRadius::Point));
    const qreal side = qMax(1.0, size * m_hoverViewZoom);

    // 方框中心对准鼠标位置（即被取样的那个像素的中心）
    const QRectF box(m_hoverWidgetPos.x() - side / 2.0,
                     m_hoverWidgetPos.y() - side / 2.0,
                     side, side);

    painter.setBrush(Qt::NoBrush);

    // 黑白双圈，任何底色上都看得见（与画笔轮廓同理）
    painter.setPen(QPen(QColor(0, 0, 0, 170), 1.0));
    painter.drawRect(box);
    painter.setPen(QPen(QColor(255, 255, 255, 190), 1.0));
    painter.drawRect(box.adjusted(-1.0, -1.0, 1.0, 1.0));
}

void ColorPickerTool::viewChanged()
{
    // 与画笔同理：控件坐标缓存失效，等下一次鼠标移动再恢复。
    m_hoverStale = true;
}

} // namespace zl
