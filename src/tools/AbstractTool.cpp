#include "tools/AbstractTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "commands/CommandStack.h"

#include <QPainter>
#include <QIcon>

namespace zl {

// ============================================================================
//  构造 / 析构
// ============================================================================

AbstractTool::AbstractTool(Document *doc, CommandStack *stack, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_stack(stack)
{
}

AbstractTool::~AbstractTool() = default;

// ============================================================================
//  默认实现
// ============================================================================
//  基类的所有事件处理都返回 ignored()，也就是"我不处理"。
//  这样 CanvasView 可以据此执行自己的默认行为（平移、缩放等）。
//
//  为什么不做成纯虚函数：
//    15 个工具里，大部分只需要其中 2~3 个事件（直线工具只要 press/move/release，
//    吸管只要 press）。如果设成纯虚，每个工具都要写一堆空实现。
// ============================================================================

ToolResult AbstractTool::mousePress(const ToolEvent &)
{
    return ToolResult::ignored();
}

ToolResult AbstractTool::mouseMove(const ToolEvent &)
{
    return ToolResult::ignored();
}

ToolResult AbstractTool::mouseRelease(const ToolEvent &)
{
    return ToolResult::ignored();
}

ToolResult AbstractTool::mouseDoubleClick(const ToolEvent &)
{
    // 默认把双击当作普通按下处理 —— 这样可以避免"双击时第一次点击
    // 被吞掉，导致工具行为异常"这种隐蔽的小 bug。
    return ToolResult::ignored();
}

ToolResult AbstractTool::keyPress(const ToolEvent &)
{
    return ToolResult::ignored();
}

ToolResult AbstractTool::keyRelease(const ToolEvent &)
{
    return ToolResult::ignored();
}

ToolResult AbstractTool::hoverMove(const ToolEvent &)
{
    return ToolResult::ignored();
}

void AbstractTool::paintOverlay(QPainter &, CanvasView *)
{
    // 默认什么都不画
}

void AbstractTool::paintScreenOverlay(QPainter &, CanvasView *)
{
    // 默认什么都不画
}

QIcon AbstractTool::icon() const
{
    // 返回空图标。工具面板会把空图标显示成一个占位方块 + 工具名，
    // 比"显示一个错误的图标"要好。
    return QIcon();
}

QCursor AbstractTool::cursor() const
{
    return QCursor(Qt::CrossCursor);
}

// ============================================================================
//  commit / cancel
// ============================================================================

void AbstractTool::commit()
{
    // 默认实现：如果没有正在进行的操作就什么都不做。
    // 有正在进行的操作时，说明子类忘了重写 commit() —— 这时保守处理，
    // 调用 cancel() 回滚，避免把"半成品"留在数据里。
    if (m_busy)
        cancel();
}

void AbstractTool::cancel()
{
    m_busy = false;
    resetDragRect();
}

// ============================================================================
//  文档切换
// ============================================================================

void AbstractTool::setDocument(Document *doc)
{
    if (m_doc == doc)
        return;

    // ★ 切换文档前必须先收尾 ★
    //   画笔正在画一笔时用户切换了标签页，如果不 cancel，
    //   这一笔的内容会留在旧文档的图层里（因为预览是直接写位图的），
    //   但撤销命令没有压栈 —— 用户会发现"多了一道擦不掉的痕迹"。
    if (m_busy)
        cancel();

    m_doc = doc;
    emit overlayChanged();
}

void AbstractTool::setCommandStack(CommandStack *stack)
{
    m_stack = stack;
}

// ============================================================================
//  受保护的辅助函数
// ============================================================================

bool AbstractTool::isReady() const
{
    // 撤销栈允许为空 —— 有些工具（吸管、抓手、缩放）不修改数据，
    // 不需要撤销栈也能工作。真正需要它的工具应当在用到的地方单独判断。
    return m_doc != nullptr;
}

bool AbstractTool::canWriteToActiveLayer() const
{
    if (!m_doc)
        return false;

    const Layer *layer = m_doc->activeLayer();
    if (!layer)
        return false;

    if (!layer->isVisible()) {
        // const 方法里不能调非 const 的 showStatus，所以这里用 const_cast。
        // 这是可接受的：showStatus 只是发信号，不改变对象逻辑状态。
        const_cast<AbstractTool *>(this)->showStatus(tr("The current layer is hidden and cannot be edited"));
        return false;
    }

    if (layer->isLocked()) {
        const_cast<AbstractTool *>(this)->showStatus(tr("The current layer is locked and cannot be edited"));
        return false;
    }

    return true;
}

void AbstractTool::showStatus(const QString &message, int timeoutMs)
{
    emit statusMessage(message, timeoutMs);
}

void AbstractTool::requestRepaint()
{
    emit overlayChanged();
}

void AbstractTool::expandDragRect(const QRectF &r)
{
    if (!r.isValid())
        return;

    m_dragRect = m_dragRect.isNull() ? r : m_dragRect.united(r);
}

void AbstractTool::resetDragRect()
{
    m_dragRect = QRectF();
}

} // namespace zl
