#include "tools/ShapeTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "engine/Brush.h"           // composeStroke（与画笔、油漆桶共用同一条合成路径）
#include "commands/CommandStack.h"
#include "commands/DrawCommand.h"

#include <QPainter>
#include <QtMath>

namespace zl {

// ===========================================================================
//  构造 / 身份
// ===========================================================================
ShapeTool::ShapeTool(Kind kind, Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
    , m_kind(kind)
{
    // 参数对象以 this 为 parent —— 生命周期随工具，工具切换不重置用户设置。
    // ★ 只创建一个：直线与形状的选项内容不同（线型 vs 圆角/填充），
    //   硬凑成一个会让选项条上出现一堆对当前工具无意义的控件。★
    if (m_kind == Kind::Line) {
        m_lineOptions = new LineOptions(this);
    } else {
        m_shapeOptions = new ShapeOptions(m_kind == Kind::Rectangle
                                              ? ShapeKind::Rectangle
                                              : ShapeKind::Ellipse,
                                          this);
    }
}

ShapeTool::~ShapeTool() = default;

QString ShapeTool::id() const
{
    // ★ id 同时是 QSettings 分组名与"上次使用的工具"持久化键，定了就别改 ★
    switch (m_kind) {
    case Kind::Line:      return QStringLiteral("line");
    case Kind::Rectangle: return QStringLiteral("rect");
    case Kind::Ellipse:   return QStringLiteral("ellipse");
    }
    return QStringLiteral("line");
}

QString ShapeTool::displayName() const
{
    switch (m_kind) {
    case Kind::Line:      return tr("Line");
    case Kind::Rectangle: return tr("Rectangle");
    case Kind::Ellipse:   return tr("Ellipse");
    }
    return tr("Line");
}

int ShapeTool::shortcutKey() const
{
    // 直线用 L（Photoshop 里 L 是套索组，本工程没有套索，L 空着）。
    //
    // ★★ 矩形与椭圆**故意**返回同一个键 U ★★
    //
    //   ToolManager::findByShortcut() 是"命中第一个就返回"，所以按 U 一定
    //   先进矩形。而"已经是形状工具时再按 U 就换成另一种形状"这条
    //   轮换逻辑写在 MainWindow::activateToolByShortcut() 里
    //   （与 M 键在两种选框间轮换完全同构）。
    //
    //   所以这里返回同一个键**不是 bug**，是那条轮换逻辑的入口条件。
    //   谁要是"顺手"把椭圆改成别的键，轮换就静默失效了 ——
    //   而 U 仍然能用，用户不会觉得有问题，只是少了轮换。
    return (m_kind == Kind::Line) ? Qt::Key_L : Qt::Key_U;
}

ToolOptions *ShapeTool::options() const
{
    // 隐式向上转型到基类指针；同一时刻只有一个非空。
    return m_lineOptions ? static_cast<ToolOptions *>(m_lineOptions)
                         : static_cast<ToolOptions *>(m_shapeOptions);
}

// ===========================================================================
//  颜色同步
// ===========================================================================
void ShapeTool::setForegroundColor(const QColor &color)
{
    if (!color.isValid())
        return;

    if (m_lineOptions)
        m_lineOptions->setColor(color);
    else if (m_shapeOptions)
        m_shapeOptions->setStrokeColor(color);
}

void ShapeTool::setBackgroundColor(const QColor &color)
{
    if (!color.isValid())
        return;

    // 只有形状才有"填充色"的概念；直线接了就当没听见。
    if (m_shapeOptions)
        m_shapeOptions->setFillColor(color);
}

// ===========================================================================
//  形状的生命周期
// ===========================================================================
bool ShapeTool::beginShapeAt(const QPointF &imagePos)
{
    if (!isReady())
        return false;

    // 图层隐藏 / 锁定时这会发状态栏消息，所以这里不重复提示。
    if (!canWriteToActiveLayer())
        return false;

    Document *doc = document();
    const int index = doc->activeLayerIndex();
    Layer *layer = doc->layers().layer(index);
    if (!layer || layer->isNull())
        return false;

    if (doc->canvasSize().isEmpty())
        return false;       // 退化文档，防除零

    // ★ 没有撤销栈就干脆不开始 ★
    //   拖动期间写的是真实像素，只有松手时才压栈。如果栈是空的，
    //   用户拖完就会发现图层上多了一块**永远撤不掉**的痕迹 ——
    //   比"工具不动"严重得多。油漆桶那边是同一条纪律（BucketTool.cpp）。
    if (!commandStack()) {
        showStatus(tr("No undo stack available - drawing cancelled"));
        return false;
    }

    // ── 落笔前快照（浅拷贝；写图层时才由 QImage 的写时复制真正深拷）──
    m_originalImage = layer->image();
    m_layerIndex = index;

    m_anchor = imagePos;
    m_prevBounds = QRect();
    m_contentBounds = QRect();
    m_dragged = false;
    m_hasMove = false;
    m_lastShift = false;

    // ── 选区快照 ──
    //   三条有效性条件与画笔完全一致（非空 / 尺寸等于画布 / 格式为 Grayscale8）——
    //   尺寸对不上的选区（旧画布尺寸的残留）绝不能拿来用，
    //   那会把形状裁到错误的位置上。
    m_selectionMask = QImage();
    const Selection &sel = doc->selection();
    if (sel.isActive()) {
        const QImage &mask = sel.mask();
        if (!mask.isNull()
            && mask.size() == doc->canvasSize()
            && mask.format() == QImage::Format_Grayscale8) {
            m_selectionMask = mask;     // 浅拷贝
        }
    }

    setBusy(true);
    return true;
}

ShapeMask ShapeTool::buildMask(const QPointF &anchor, const QPointF &current,
                               bool constrain) const
{
    const QSize canvas = document() ? document()->canvasSize() : QSize();
    if (canvas.isEmpty())
        return ShapeMask();

    if (m_kind == Kind::Line) {
        // ★ 约束只改"终点"，不改锚点 ★
        //   锚点是用户按下时定下的，拖动过程中它必须纹丝不动 ——
        //   否则用户会看到线的起点跟着鼠标跑。
        const QPointF end = constrain ? constrainTo8Directions(anchor, current)
                                      : current;

        const int width     = m_lineOptions ? m_lineOptions->width() : 2;
        const Qt::PenStyle s = m_lineOptions ? m_lineOptions->style() : Qt::SolidLine;
        const bool aa       = m_lineOptions ? m_lineOptions->antialias() : true;

        return rasterizeLine(canvas, anchor, end, width, s, aa);
    }

    const ShapeKind kind = (m_kind == Kind::Rectangle) ? ShapeKind::Rectangle
                                                       : ShapeKind::Ellipse;
    const QRectF rect = rectFromDrag(anchor, current, constrain);

    const ShapeMode mode = m_shapeOptions ? m_shapeOptions->mode() : ShapeMode::Stroke;
    const int width      = m_shapeOptions ? m_shapeOptions->width() : 2;
    const int corner     = m_shapeOptions ? m_shapeOptions->cornerRadius() : 0;
    const bool aa        = m_shapeOptions ? m_shapeOptions->antialias() : true;

    return rasterizeShape(canvas, rect, kind, mode, width, corner, aa);
}

QImage *ShapeTool::writableLayerImage()
{
    if (!isReady() || m_layerIndex < 0)
        return nullptr;

    Layer *layer = document()->layers().layer(m_layerIndex);
    if (!layer || layer->isNull())
        return nullptr;

    return &layer->mutableImage();
}

QColor ShapeTool::strokeColor() const
{
    if (m_lineOptions)
        return m_lineOptions->color();
    if (m_shapeOptions)
        return m_shapeOptions->strokeColor();
    return QColor(Qt::black);
}

QColor ShapeTool::fillColor() const
{
    if (m_shapeOptions)
        return m_shapeOptions->fillColor();
    return QColor(Qt::white);
}

void ShapeTool::updatePreview(const QPointF &anchor, const QPointF &current,
                              bool constrain)
{
    if (m_layerIndex < 0)
        return;

    m_dragged = true;

    const ShapeMask mask = buildMask(anchor, current, constrain);

    // ── 重算区域 = 上一帧包围盒 ∪ 本帧包围盒 ──
    //
    //  ★ 为什么必须并上"上一帧" ★
    //    形状是"每次几何变化都整体重画"，不是在旧结果上追加。
    //    若只重算本帧包围盒，上一个位置的那块形状会**留在画面上**
    //    （新形状画在旁边，旧形状还在）—— 用户会看到一串残影。
    //
    //  ★ 形状完全在画布外时（mask.bounds 为空）★
    //    region 退化成"上一帧包围盒"，而 applyRegion 在蒙版为空时
    //    只会把快照原样写回 —— 正好就是"擦掉旧预览"。
    //    所以这里不需要为它单开一条分支。
    QRect region = m_prevBounds.united(mask.bounds);
    if (!m_originalImage.isNull())
        region &= QRect(QPoint(0, 0), m_originalImage.size());

    if (!region.isEmpty())
        applyRegion(region, mask);

    m_prevBounds = mask.bounds;

    // 整段操作的脏矩形（松手时作为 DrawCommand 的范围）。
    // 注意要并的是 mask.bounds（形状本身），不是 region ——
    // region 里属于"上一帧"的部分是恢复成原样，不算改动。
    m_contentBounds = m_contentBounds.united(mask.bounds);
}

void ShapeTool::applyRegion(const QRect &region, const ShapeMask &mask)
{
    if (region.isEmpty())
        return;

    QImage *target = writableLayerImage();
    if (!target)
        return;

    // ── 基底永远取自"落笔前的快照"──
    //   这是幂等性的来源：无论拖动把同一块区域重算多少次，
    //   结果都只取决于（快照, 当前几何）这一对输入。
    //   在上一帧结果上继续叠加的话，拖出去再拖回来会留下痕迹。
    QImage out = m_originalImage.copy(region);
    if (out.isNull())
        return;

    const QImage selPatch = m_selectionMask.isNull()
                                ? QImage()
                                : m_selectionMask.copy(region);

    // ── 填充先、描边后 ──
    //   顺序不能反："描边压在填充之上"是用户对形状的预期，
    //   而且与 Photoshop 一致（半透明描边叠在填充上会更深）。
    //
    //   ★ 两次都走 composeStroke ★
    //     于是"选区限制""预乘空间合成""颜色 alpha 即透明度"三条规则
    //     在整个工程里只有一份实现 —— 这是 engine/Shape 只产出蒙版、
    //     不产出图像的全部意义（见 Shape.h 顶部说明）。
    if (!mask.fillMask.isNull()) {
        const QImage fm = embedMask(mask.fillMask, mask.bounds, region);
        if (!fm.isNull())
            out = composeStroke(out, fm, fillColor(), false, selPatch);
    }

    if (!mask.strokeMask.isNull()) {
        const QImage sm = embedMask(mask.strokeMask, mask.bounds, region);
        if (!sm.isNull())
            out = composeStroke(out, sm, strokeColor(), false, selPatch);
    }

    // ── 写回 ──
    //   用 CompositionMode_Source：要的是"把这块区域替换成重算结果"，
    //   包括它比原来更透明的情况。SourceOver 会把"更透明"当成"不覆盖"。
    //   （画笔那边踩过同一个坑，见 BrushTool::applyRegion 的注释。）
    QPainter painter(target);
    if (!painter.isActive())
        return;

    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(region.topLeft(), out);
    painter.end();

    // ── 只失效"这次真正重算的那一块" ──
    //
    // region 就是本函数的入参：上一次 updatePreview 算出来的
    // "上一帧包围盒 ∪ 本帧包围盒"（再与画布求过交）。
    // 换句话说，**改动的像素恰好就是这个矩形**，多一个都没有 ——
    // 所以这里可以精确回答，不需要保守估计。
    //
    // ★ 为什么不是 markDirty() ★
    //   拖动期间每个鼠标事件都会走到这里。原先每次都整图失效 →
    //   大画布上拖一个矩形，每一帧都要整张画布重合成
    //   （实测 4K 431 ms/帧，见 docs/性能优化评估.md）。而真正变的
    //   往往只有形状那一小块。
    document()->markDirtyRegion(region);
}

void ShapeTool::finishShape()
{
    const int   layerIndex = m_layerIndex;
    const QRect bounds     = m_contentBounds;
    const bool  dragged    = m_dragged;

    // 先清状态，避免下面的 push() 触发重入时看到半截状态。
    setBusy(false);
    m_layerIndex = -1;
    m_dragged = false;
    m_prevBounds = QRect();
    m_contentBounds = QRect();

    Document *doc = document();
    CommandStack *stack = commandStack();

    const bool hasChange = !bounds.isEmpty()
                           && !m_originalImage.isNull()
                           && layerIndex >= 0;

    if (doc && stack && hasChange) {
        Layer *layer = doc->layers().layer(layerIndex);
        if (layer) {
            // 两个 patch 都必须是独立副本（同画笔：共享内存的 patch 会
            // 随着图层后续改动一起变，撤销就失效了）。
            const QImage before = m_originalImage.copy(bounds);
            const QImage after  = layer->image().copy(bounds);

            if (!before.isNull() && !after.isNull()) {
                // ★ standalone()：整段拖动 = 一条撤销记录 ★
                //   它取一个新的描边编号，所以"画一个矩形、再画一个"
                //   一定是两条记录。自己 new 并传固定编号的话，
                //   连续两个形状会被 QUndoStack 静默合并成一步
                //   （这个坑在阶段六首批刚踩过，见 DrawCommand.h 的说明）。
                const QString text = (m_kind == Kind::Line)      ? tr("Line")
                                     : (m_kind == Kind::Rectangle) ? tr("Rectangle")
                                                                   : tr("Ellipse");
                stack->push(DrawCommand::standalone(doc, layerIndex, bounds,
                                                    before, after, text));
            }
        }
    } else if (dragged && !hasChange) {
        // 拖过，但一点像素都没落到画布上（整个形状在画布外）。
        // 这时**不提示"什么都没有"**会让人以为工具坏了 —— 提示一次。
        showStatus(tr("The shape is outside the canvas - nothing changed"));
    }

    m_originalImage = QImage();
    m_selectionMask = QImage();
}

void ShapeTool::discardShape()
{
    // 把这段操作触及过的区域整块恢复成落笔前的样子。
    if (document() && m_layerIndex >= 0
        && !m_contentBounds.isEmpty() && !m_originalImage.isNull()) {

        Layer *layer = document()->layers().layer(m_layerIndex);
        if (layer && !layer->isNull()) {
            const QRect r = m_contentBounds
                            & QRect(QPoint(0, 0), m_originalImage.size());
            if (!r.isEmpty()) {
                const QImage restore = m_originalImage.copy(r);

                QPainter painter(&layer->mutableImage());
                if (painter.isActive()) {
                    painter.setCompositionMode(QPainter::CompositionMode_Source);
                    painter.drawImage(r.topLeft(), restore);
                    painter.end();
                    // 恢复回去的那一块 = 这段拖动碰过的全部区域（r）。
                    // 与 applyRegion 同一条理由：能精确回答就别整图失效。
                    document()->markDirtyRegion(r);
                }
            }
        }
    }

    // 与 finishShape 一样彻底清空 —— 但**不压栈**。
    // 这正是 cancel 与 commit 的全部区别。
    setBusy(false);
    m_layerIndex = -1;
    m_dragged = false;
    m_prevBounds = QRect();
    m_contentBounds = QRect();
    m_originalImage = QImage();
    m_selectionMask = QImage();
}

void ShapeTool::commit()
{
    // 切换工具 / 关文档：把正在拖的形状正常收尾（留下 + 记进撤销栈）。
    if (m_layerIndex >= 0)
        finishShape();
}

void ShapeTool::cancel()
{
    // Esc：这段不算数。
    if (m_layerIndex >= 0)
        discardShape();
}

bool ShapeTool::isBusy() const
{
    // 与画笔同一条理由：m_layerIndex 才是"确实有一次操作在进行"的权威依据。
    return m_layerIndex >= 0;
}

// ===========================================================================
//  输入事件
// ===========================================================================
ToolResult ShapeTool::mousePress(const ToolEvent &e)
{
    if (!e.isPress())
        return ToolResult::ignored();

    // 只响应左键（右键归画布上下文菜单，中键归画布平移）
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    // 上一次的 release 丢了（例如拖到窗口外松手）：先把它收尾，
    // 否则那一次画的形状永远没有撤销记录。
    if (m_layerIndex >= 0)
        finishShape();

    if (!beginShapeAt(e.imagePos))
        return ToolResult::ignored();

    // ★ 落笔瞬间**不画任何东西** ★
    //   形状是"拖出来的"，按下时它还是零面积。这里若画一下，
    //   用户点一下就会留下一个点（直线）或什么都没有（矩形）——
    //   两者都不是他想要的。真正落笔发生在第一次 mouseMove。
    return ToolResult::accepted();
}

ToolResult ShapeTool::mouseMove(const ToolEvent &e)
{
    if (m_layerIndex < 0)
        return ToolResult::ignored();

    // 没有按键按下的移动不算拖动（有些平台在 hover 时也发 Move）
    if (e.buttons == Qt::NoButton)
        return ToolResult::ignored();

    // ── 节流：位置与 Shift 状态都没变就不重算 ──
    //
    //  拖动中有些平台会持续发位置相同的事件；这时每帧重算一次
    //  "上一帧 ∪ 本帧"整块区域纯属浪费（大形状下能把 CPU 吃满）。
    //  比较用 0.01 像素的容差，避免浮点噪声导致节流失效。
    const bool moved = !m_hasMove
                       || qAbs(e.imagePos.x() - m_lastMovePos.x()) > 0.01
                       || qAbs(e.imagePos.y() - m_lastMovePos.y()) > 0.01
                       || e.modifiers.shift != m_lastShift;

    m_lastMovePos = e.imagePos;
    m_lastShift = e.modifiers.shift;
    m_hasMove = true;

    if (!moved)
        return ToolResult::accepted();

    updatePreview(m_anchor, e.imagePos, e.modifiers.shift);
    return ToolResult::repaint();
}

ToolResult ShapeTool::mouseRelease(const ToolEvent &e)
{
    if (m_layerIndex < 0)
        return ToolResult::ignored();

    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    finishShape();
    return ToolResult::repaint();
}

// ===========================================================================
//  自检入口
// ===========================================================================
bool ShapeTool::drawShape(const QPointF &anchor, const QPointF &current, bool constrain)
{
    if (!beginShapeAt(anchor))
        return false;

    updatePreview(anchor, current, constrain);

    const bool changed = !m_contentBounds.isEmpty();

    // 与鼠标路径完全一致地收尾（包括"拖到画布外"的提示分支）
    finishShape();

    return changed;
}

} // namespace zl
