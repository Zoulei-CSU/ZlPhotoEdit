#include "tools/MoveTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "commands/CommandStack.h"
#include "commands/ImageCommand.h"
#include "commands/LayerCommand.h"
#include "engine/Transform.h"

#include <QPainter>
#include <QFontMetrics>

namespace zl {

// ============================================================================
//  构造 / 析构
// ============================================================================

MoveTool::MoveTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
    , m_options(new MoveOptions(this))
{
    // 工具本身不关心具体的参数值，它只在画叠加层时读一下
    // "是否显示变换框"。参数变化时请求重绘即可。
    connect(m_options, &ToolOptions::changed, this, [this]() {
        emit overlayChanged();
    });
}

MoveTool::~MoveTool() = default;

ToolOptions *MoveTool::options() const
{
    return m_options;
}

QCursor MoveTool::cursor() const
{
    // 四向箭头是"移动"的通用语义。
    // 不用 SizeAllCursor 是因为它在不同主题下渲染差异较大，
    // 而 OpenHandCursor 会被误认为抓手工具。
    return QCursor(Qt::SizeAllCursor);
}

// ============================================================================
//  鼠标按下 —— 开始拖动
// ============================================================================

ToolResult MoveTool::mousePress(const ToolEvent &e)
{
    // 只处理左键。右键留给 CanvasView 的上下文菜单，
    // 中键留给"临时平移"，都不能被工具吞掉。
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    if (!isReady())
        return ToolResult::ignored();

    // ── 自动选择图层（可选）──
    // 如果用户勾了"自动选择图层"，先看看鼠标下方哪个图层有非透明像素。
    if (m_options && m_options->autoSelectLayer()) {
        Document *doc = document();
        const QPoint imagePt = e.imagePoint();

        // ★ 从最上层往下找 ★
        //   图层列表索引 0 是最底部，所以要倒序遍历。
        //   找到第一个"该点像素 alpha 不为 0 且可见"的图层。
        for (int i = doc->layerCount() - 1; i >= 0; --i) {
            const Layer *layer = doc->layers().layer(i);
            if (!layer || !layer->isVisible() || layer->isLocked())
                continue;

            const QRect bounds = layer->rect();
            if (!bounds.contains(imagePt))
                continue;   // 鼠标在图层范围外

            // 读取该点的 alpha。
            // ★ 用 QImage::pixelColor 而不是 pixel() ★
            //   pixel() 返回的 QRgb 在预乘格式下需要手动反预乘，
            //   pixelColor() 则已经做了转换，直接看 alpha() 即可。
            const QColor c = layer->image().pixelColor(imagePt);
            if (c.alpha() > 0) {
                doc->setActiveLayerIndex(i);
                break;
            }
        }
    }

    // ── 检查活动图层是否可写 ──
    if (!canWriteToActiveLayer())
        return ToolResult::ignored();

    // Alt 按下时执行"复制式拖动"
    const bool duplicate = e.modifiers.alt;

    beginDrag(e, e.imagePoint(), duplicate);

    if (m_dragLayerIndex < 0)
        return ToolResult::ignored();

    return ToolResult::repaint();
}

// ============================================================================
//  beginDrag —— 记录起始状态
// ============================================================================

void MoveTool::beginDrag(const ToolEvent &e, const QPoint & /*startImagePos*/, bool duplicate)
{
    Document *doc = document();
    if (!doc)
        return;

    const int index = doc->activeLayerIndex();
    Layer *layer = doc->activeLayer();
    if (!layer)
        return;

    m_dragLayerIndex = index;
    m_startImagePos  = e.imagePos;
    m_currentOffset  = QPoint();
    m_anythingMoved  = false;
    m_duplicated     = duplicate;

    // ★ 整层快照，这是撤销数据的来源 ★
    //
    //   注意这里必须用 image().copy() 而不是直接赋值：
    //     QImage 的赋值是浅拷贝（引用计数），如果之后图层位图被就地修改，
    //     两个 QImage 会共享同一块内存 —— 我们保存的"原始位图"就会
    //     跟着一起变，撤销时拿到的不是拖动前的样子。
    //
    //   实际上 QPainter/QImage 的写时复制机制通常能保护我们，但
    //   这里要保存的是一个"跨越多次修改的历史状态"，主动 copy()
    //   才是有保证的做法（成本：一次 48MB 的内存拷贝，约 10ms，
    //   只在按下鼠标时发生一次）。
    m_originalImage = layer->image().copy();

    if (duplicate) {
        // ── 复制式拖动：先新增一个图层 ──
        //
        // 顺序很重要：必须先压入"新增图层"命令，再在后面压"移动"命令。
        // 这样撤销时会先取消移动、再删掉新图层，回到完全原始的状态。
        //
        // 用 CommandStack 的宏机制保证这两步是**一次撤销**：
        // 用户按一次 Ctrl+Z 就应该回到"拖动之前"，而不是"取消移动但
        // 多出一个重合的图层"。
        CommandStack *stack = commandStack();
        if (stack) {
            stack->beginMacro(tr("复制并移动图层"));

            // 用 LayerCommand 记录图层结构变化。
            // 这里的 before/after 是图层列表的浅拷贝（像素共享），成本很低。
            const QList<Layer> beforeLayers = doc->layers().cloneAll();

            // ★ Document::addLayer 有两个重载 ★
            //     addLayer(const QString &name)        —— 新建空白图层
            //     addLayer(const Layer &layer, int index = -1)
            //   没有"接受 QImage"的重载，所以这里要先把位图包成 Layer。
            //   这个重载集在写代码时很容易记混（三个参数看起来都"应该"有），
            //   编译器会报 "没有重载函数可以转换所有参数类型"。
            Layer duplicate(m_originalImage, layer->name() + tr(" 副本"));
            duplicate.setOpacity(layer->opacity());
            duplicate.setBlendMode(layer->blendMode());
            duplicate.setVisible(layer->isVisible());

            const int newIndex = doc->addLayer(duplicate);

            const QList<Layer> afterLayers = doc->layers().cloneAll();

            auto *cmd = new LayerCommand(doc, LayerCommand::Op::Duplicate,
                                         beforeLayers, afterLayers,
                                         index, newIndex,
                                         tr("复制图层"));
            stack->push(cmd);

            // 现在活动图层应该是新副本 —— 后续的移动作用在它上面
            m_dragLayerIndex = newIndex;
        } else {
            // 没有撤销栈时降级为"就地移动"，不复制。
            // 这种降级只在极端异常情况下发生（例如尚未创建文档栈），
            // 保持功能可用比直接失败要好。
            m_duplicated = false;
        }
    } else {
        m_dragLayerIndex = index;
    }

    setBusy(true);
}

// ============================================================================
//  鼠标移动 —— 实时平移
// ============================================================================

ToolResult MoveTool::mouseMove(const ToolEvent &e)
{
    if (!isBusy() || m_dragLayerIndex < 0)
        return ToolResult::ignored();

    // 只有在左键仍按着时才继续拖动。
    // 这个检查是必要的：某些情况下（例如弹出了模态对话框）
    // release 事件可能丢失，导致工具一直卡在"拖动中"状态。
    if (!(e.buttons & Qt::LeftButton)) {
        // 补一次 release，避免卡住
        ToolEvent releaseEvent = e;
        releaseEvent.type = ToolEventType::Release;
        return mouseRelease(releaseEvent);
    }

    // 计算总位移（图像坐标，浮点 → 取整）
    const QPointF delta = e.imagePos - m_startImagePos;

    QPoint offset;
    if (e.modifiers.shift) {
        // ── Shift 约束：只在水平或垂直方向移动 ──
        //
        // 判定主轴用绝对值比较，而不是"先触发哪个方向就锁哪个"。
        // 后者在用户斜向拖动时体验很差（因为手抖会让判定反复翻转）。
        if (qAbs(delta.x()) >= qAbs(delta.y()))
            offset = QPoint(qRound(delta.x()), 0);
        else
            offset = QPoint(0, qRound(delta.y()));
    } else {
        offset = QPoint(qRound(delta.x()), qRound(delta.y()));
    }

    applyDragOffset(offset, e.modifiers.shift);

    setLastImagePos(e.imagePos);
    return ToolResult::repaint();
}

// ============================================================================
//  applyDragOffset —— 从原始位图重新计算目标状态
// ============================================================================

void MoveTool::applyDragOffset(const QPoint &offset, bool /*constrain*/)
{
    Document *doc = document();
    if (!doc || m_dragLayerIndex < 0 || m_dragLayerIndex >= doc->layerCount())
        return;

    // 位移没变就什么都不做 —— mouseMove 会被高频调用（每秒上百次），
    // 而由于坐标取整，很多次事件算出来的 offset 是相同的。
    // 这个早返回能省掉大量无意义的整层 memcpy。
    if (offset == m_currentOffset)
        return;

    m_currentOffset = offset;
    m_anythingMoved = true;

    Layer *layer = doc->layers().layer(m_dragLayerIndex);
    if (!layer)
        return;

    if (offset.isNull()) {
        // 位移归零：直接把原始位图放回去，不要走 translate（会有一次无谓的拷贝）
        layer->setImage(m_originalImage);
    } else {
        // ★ 始终从 m_originalImage 出发计算 ★
        //   原因见头文件中的说明：从原始状态重算保证"所见即所得"，
        //   避免累加误差与方向约束跳变。
        const QImage moved = transform::translate(m_originalImage, offset);
        layer->setImage(moved);
    }

    doc->markDirty();
    emit overlayChanged();
}

// ============================================================================
//  restoreOriginalImage —— 回滚
// ============================================================================

void MoveTool::restoreOriginalImage()
{
    Document *doc = document();
    if (!doc || m_dragLayerIndex < 0 || m_dragLayerIndex >= doc->layerCount())
        return;

    if (m_originalImage.isNull())
        return;

    Layer *layer = doc->layers().layer(m_dragLayerIndex);
    if (!layer)
        return;

    layer->setImage(m_originalImage);
    doc->markDirty();
}

// ============================================================================
//  鼠标释放 —— 提交撤销命令
// ============================================================================

ToolResult MoveTool::mouseRelease(const ToolEvent &e)
{
    // 释放事件本身的坐标不用 —— 拖动的最终位置已经在 mouseMove 里
    // 通过 applyDragOffset 应用过了，这里只需要"收尾"。
    // （用 e 的位置会多一次无谓的整层重算。）
    Q_UNUSED(e);

    if (!isBusy())
        return ToolResult::ignored();

    Document *doc = document();
    CommandStack *stack = commandStack();

    // ── 情况一：用户没真正移动（只是点了一下）──
    //
    // 这时必须把图层位图恢复原样。因为 applyDragOffset 可能已经
    // 因为取整误差写回过一次（例如 offset 从 (0,0) 变成 (0,0) 之外的
    // 值又变回来），而且复制式拖动时还多了一个图层 —— 那个图层要删掉。
    if (!m_anythingMoved || m_currentOffset.isNull()) {
        if (m_duplicated) {
            // 复制式拖动但没移动：把刚才新增的图层撤销掉
            if (stack) {
                // 直接 undo 更简单，但要确保撤销栈顶就是那条命令。
                // 用 endMacro 结束宏后再 undo，语义清晰。
                stack->endMacro();
                stack->undo();
            } else if (doc) {
                doc->removeLayer(m_dragLayerIndex);
            }
        } else {
            restoreOriginalImage();
        }

        setBusy(false);
        m_dragLayerIndex = -1;
        m_originalImage  = QImage();
        m_duplicated     = false;
        return ToolResult::repaint();
    }

    // ── 情况二：真的移动了，记录撤销 ──

    if (doc && m_dragLayerIndex >= 0 && m_dragLayerIndex < doc->layerCount()) {
        Layer *layer = doc->layers().layer(m_dragLayerIndex);
        if (layer) {
            // 拖动后的位图 —— 同样要 copy()，因为它是 ImageCommand 的
            // "after" 状态，不能在后续操作中被就地修改。
            const QImage afterImage = layer->image().copy();

            if (stack) {
                // 注意 macro 的配对：
                //   复制式拖动的 beginMacro 在 beginDrag 里已经调用，
                //   这里 push 完"移动"命令后再 endMacro；
                //   非复制式拖动没有宏，直接 push。
                auto *cmd = new ImageCommand(doc, m_dragLayerIndex,
                                             m_originalImage, afterImage,
                                             m_duplicated ? tr("复制并移动图层")
                                                          : tr("移动图层"));
                stack->push(cmd);

                if (m_duplicated)
                    stack->endMacro();
            }
        }
    }

    setBusy(false);
    m_dragLayerIndex = -1;
    m_originalImage  = QImage();   // 及时释放，避免 48MB 的内存常驻
    m_duplicated     = false;
    m_currentOffset  = QPoint();
    m_anythingMoved  = false;

    return ToolResult::committed();
}

// ============================================================================
//  键盘 —— 方向键精确移动
// ============================================================================

ToolResult MoveTool::keyPress(const ToolEvent &e)
{
    if (!isReady() || !canWriteToActiveLayer())
        return ToolResult::ignored();

    // 方向键位移量：Shift 加速 10 倍
    int step = e.modifiers.shift ? 10 : 1;

    QPoint delta;
    switch (e.key) {
    case Qt::Key_Left:  delta = QPoint(-step, 0); break;
    case Qt::Key_Right: delta = QPoint( step, 0); break;
    case Qt::Key_Up:    delta = QPoint(0, -step); break;
    case Qt::Key_Down:  delta = QPoint(0,  step); break;
    default:
        return ToolResult::ignored();
    }

    Document *doc = document();
    CommandStack *stack = commandStack();
    if (!doc)
        return ToolResult::ignored();

    Layer *layer = doc->activeLayer();
    if (!layer)
        return ToolResult::ignored();

    const QImage before = layer->image().copy();
    const QImage after  = transform::translate(before, delta);
    layer->setImage(after);
    doc->markDirty();

    if (stack) {
        // ★ 连续按方向键应当合并成一次撤销 ★
        //
        //   ImageCommand 的 mergeWith 依赖相同的 m_mergeId。
        //   这里每次构造时给一个新的 mergeId，但通过命令的文本与
        //   一个约定实现合并 —— 见下面的说明。
        //
        //   实际做法：ImageCommand 的 mergeId 在构造时自增，
        //   所以默认不合并。对于方向键，我们希望连续按压合并成一步。
        //   当前先用最简单的策略：每次按压一条命令。
        //   理由 —— 用户按方向键的次数通常很少（1~5 次），
        //   留 5 条撤销记录不会造成困扰；而错误地合并会把
        //   "上一次的移动"和"这一次的移动"粘在一起，反而更难用。
        auto *cmd = new ImageCommand(doc, doc->activeLayerIndex(),
                                     before, after, tr("移动图层"));
        stack->push(cmd);
    }

    emit overlayChanged();
    return ToolResult::committed();
}

// ============================================================================
//  commit / cancel
// ============================================================================

void MoveTool::commit()
{
    // 拖动中途切换工具（例如按了别的快捷键）：
    // 把当前位移提交掉，这与"松手"的语义一致。
    if (!isBusy())
        return;

    Document *doc = document();
    CommandStack *stack = commandStack();

    if (doc && stack && m_dragLayerIndex >= 0
        && m_dragLayerIndex < doc->layerCount() && m_anythingMoved
        && !m_currentOffset.isNull()) {
        Layer *layer = doc->layers().layer(m_dragLayerIndex);
        if (layer) {
            auto *cmd = new ImageCommand(doc, m_dragLayerIndex,
                                         m_originalImage, layer->image().copy(),
                                         m_duplicated ? tr("复制并移动图层")
                                                      : tr("移动图层"));
            stack->push(cmd);
            if (m_duplicated)
                stack->endMacro();
        }
    } else {
        // 什么都没移动 —— 走回滚路径
        cancel();
        return;
    }

    setBusy(false);
    m_dragLayerIndex = -1;
    m_originalImage  = QImage();
    m_duplicated     = false;
    m_currentOffset  = QPoint();
    m_anythingMoved  = false;
}

void MoveTool::cancel()
{
    if (!isBusy())
        return;

    // 这里只需要撤销栈（结束宏 / 撤销），不需要 Document ——
    // 图层的恢复走 restoreOriginalImage()，它自己会去取文档。
    CommandStack *stack = commandStack();

    if (m_duplicated && stack) {
        // 复制式拖动被取消：结束宏并撤销掉整个宏（含新增图层）
        stack->endMacro();
        stack->undo();
    } else {
        restoreOriginalImage();
    }

    setBusy(false);
    m_dragLayerIndex = -1;
    m_originalImage  = QImage();
    m_duplicated     = false;
    m_currentOffset  = QPoint();
    m_anythingMoved  = false;
}

// ============================================================================
//  paintScreenOverlay —— 显示当前位移
// ============================================================================
//
//  ★ 本函数在 painter.resetTransform() 之后被调用 ★
//    所以这里用的是**控件坐标**，不是图像坐标。
//    位移数值需要在屏幕上显示成固定大小，这正是不该受视图变换影响的原因。
// ============================================================================

void MoveTool::paintScreenOverlay(QPainter &painter, CanvasView *view)
{
    if (!isBusy() || !view)
        return;

    if (m_currentOffset.isNull())
        return;

    // 在画布左上角附近显示"Δx, Δy"。
    // 位置固定在控件坐标的 (12, 12)，与图像内容的位置无关 ——
    // 这样无论滚到哪里都能看到，不需要跟随鼠标。
    const QString text = tr("Δ %1, %2")
                             .arg(m_currentOffset.x())
                             .arg(m_currentOffset.y());

    painter.save();

    QFont f = painter.font();
    f.setPointSizeF(qMax(9.0, f.pointSizeF()));
    painter.setFont(f);

    const QFontMetrics fm(f);
    const QRect textRect = fm.boundingRect(text).adjusted(-6, -3, 6, 3);
    const QRect bgRect(12, 12, textRect.width(), textRect.height() + 2);

    // 半透明底板 + 描边，保证在任何图像内容上都能看清
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(bgRect, 3, 3);

    painter.setPen(QColor(255, 255, 255));
    painter.drawText(bgRect, Qt::AlignCenter, text);

    painter.restore();
}

} // namespace zl
