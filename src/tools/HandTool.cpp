#include "tools/HandTool.h"

namespace zl {

// ============================================================================
//  构造 / 身份
// ============================================================================
HandTool::HandTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
{
}

HandTool::~HandTool() = default;

QString HandTool::id() const
{
    // ★ 这个 id 是 QSettings 的键名（"上次使用的工具"），一旦发布不要再改 ★
    return QStringLiteral("hand");
}

QString HandTool::displayName() const
{
    return tr("抓手");
}

int HandTool::shortcutKey() const
{
    // H = Hand，与 Photoshop / GIMP / Krita 一致。
    // 用户在别的软件里养成的肌肉记忆在这里应该继续有效。
    return Qt::Key_H;
}

QCursor HandTool::cursor() const
{
    // 拖动中是"握紧的手"——这是用户唯一能确认"我确实抓住了画布"的反馈。
    // 空闲时是"张开的手"。
    return QCursor(m_dragging ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
}

// ============================================================================
//  输入事件
// ============================================================================

ToolResult HandTool::mousePress(const ToolEvent &e)
{
    // 只认左键。中键与右键在 CanvasView 的派发阶段就被拦掉了
    // （中键是"临时平移"，右键留给上下文菜单），所以走到这里的基本只有左键。
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    m_dragging = true;
    m_lastWidgetPos = e.widgetPos;
    setBusy(true);

    return ToolResult::accepted();
}

ToolResult HandTool::mouseMove(const ToolEvent &e)
{
    if (!m_dragging)
        return ToolResult::ignored();       // 没有按下就划过去 —— 什么都不做

    // ★ 增量 = 本次 - 上一次，然后立刻把"上一次"更新掉 ★
    //   顺序写反（先更新、再相减）的话增量恒为 0，
    //   症状是"抓手完全拖不动"，而且不报任何错。
    const QPointF delta = e.widgetPos - m_lastWidgetPos;
    m_lastWidgetPos = e.widgetPos;

    if (delta.isNull())
        return ToolResult::accepted();      // 原地抖动：不发无意义的平移请求

    m_lastDelta = delta;
    ++m_panCount;
    emit requestPan(delta);

    return ToolResult::accepted();
}

ToolResult HandTool::mouseRelease(const ToolEvent &e)
{
    if (!m_dragging)
        return ToolResult::ignored();

    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    m_dragging = false;
    setBusy(false);

    return ToolResult::accepted();
}

// ============================================================================
//  收尾
// ============================================================================

void HandTool::cancel()
{
    // 抓手没有任何"未完成的数据操作"（不碰像素、不压命令），
    // 所以这里唯一要做的是把状态清干净 —— 否则切走再切回来时
    // 光标会停留在"握紧的手"上，而且下次不按下也会跟着平移。
    m_dragging = false;

    // 基类版本负责 m_busy 与 dragRect。
    // 显式调用基类而不是自己赋值，是为了将来基类收尾逻辑变多时不会漏。
    AbstractTool::cancel();
}

} // namespace zl
