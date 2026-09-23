#include "tools/BrushTool.h"

#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "commands/CommandStack.h"
#include "commands/DrawCommand.h"
#include "tools/ToolOptions.h"

#include <QPainter>
#include <QPen>
#include <QColor>
#include <QtMath>

namespace zl {

// ===========================================================================
//  构造 / 析构
// ===========================================================================
BrushTool::BrushTool(Kind kind, Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
    , m_kind(kind)
{
    // 参数对象以 this 为 parent，生命周期随工具。
    // BrushOptions 内部会根据 Kind 设置默认值（橡皮默认更大更硬）
    // 以及共用同一个设置分组（切工具时尺寸不突变）。
    m_options = new BrushOptions(kind == Kind::Brush ? BrushOptions::Kind::Brush
                                                    : BrushOptions::Kind::Eraser,
                                 this);

    // ★ 参数变化 → 只让笔尖缓存失效，不立刻重建 ★
    //
    //   用户在选项条上拖"大小"滑块时，每一像素都会触发 changed()。
    //   如果每次都重新生成笔尖蒙版，500 像素的笔刷要算 25 万个像素、
    //   2000 像素的要算 400 万个 —— 拖动滑块会明显卡顿。
    //
    //   改成"下次落笔时按需重建"（见 currentTip()）：拖动期间界面只
    //   更新数字，不付出任何算法代价。实测这一处改动让滑块从"拖不动"
    //   变成"完全跟手"。
    connect(m_options, &ToolOptions::changed, this, [this]() {
        m_tipDiameter = -1;
        m_tipHardness = -1;

        // 笔刷大小/硬度变了，轮廓圆的大小也要跟着变，
        // 否则用户看到的圈和实际落笔范围对不上。
        // 这里不需要重算坐标 —— 直接按当前缓存的 widgetPos 重画即可。
        if (!m_hoverStale)
            requestRepaint();
    });
}

BrushTool::~BrushTool() = default;

// ===========================================================================
//  身份
// ===========================================================================
QString BrushTool::id() const
{
    // id 一旦确定不要再改 —— 它同时是 QSettings 的分组名和
    // "上次使用的工具"的持久化键，改了用户的设置就丢了。
    return m_kind == Kind::Brush ? QStringLiteral("brush")
                                 : QStringLiteral("eraser");
}

QString BrushTool::displayName() const
{
    return m_kind == Kind::Brush ? tr("画笔") : tr("橡皮");
}

int BrushTool::shortcutKey() const
{
    // 沿用 Photoshop 的键位：B = 画笔，E = 橡皮。
    // 用户有肌肉记忆，没有理由自创一套。
    return m_kind == Kind::Brush ? Qt::Key_B : Qt::Key_E;
}

QCursor BrushTool::cursor() const
{
    // ★ 隐藏系统光标，改由 paintScreenOverlay 画轮廓圆 ★
    //   理由见头文件。注意：如果用 Qt::ArrowCursor，屏幕上会同时出现
    //   "箭头 + 圆"，看起来像两个光标。
    return QCursor(Qt::BlankCursor);
}

ToolOptions *BrushTool::options() const
{
    return m_options;
}

void BrushTool::setForegroundColor(const QColor &color)
{
    if (!m_options || !color.isValid())
        return;

    // 橡皮不消费颜色，但仍然同步 —— 这样从橡皮切回画笔时，
    // 选项条上的颜色块立刻就是正确的最新前景色，而不是上次的旧值。
    m_options->setColor(color);
}

// ===========================================================================
//  笔尖与间距
// ===========================================================================
const BrushTip &BrushTool::currentTip()
{
    const int diameter = m_options ? m_options->size() : 20;
    const int hardness = m_options ? m_options->hardness() : 100;

    // 只在参数真的变了才重建。这是上面那段注释里说的"按需重建"。
    if (m_tipDiameter != diameter || m_tipHardness != hardness) {
        m_tip = makeBrushTip(diameter, hardness);
        m_tipDiameter = diameter;
        m_tipHardness = hardness;
    }

    return m_tip;
}

qreal BrushTool::stampSpacing() const
{
    // 间距取直径的 1/4。
    //
    // 这个比例是各绘图软件的通用取值，依据是：
    //   · 间距 ≤ 半径 时，相邻两个笔尖的覆盖区一定重叠
    //     → 笔画的边缘不会出现"扇贝形"的波浪缺口
    //   · 再密下去只是让同一像素被反复覆盖，白白浪费算力
    //
    // 下限 1 像素：极小的笔刷（1~3 像素）按比例算出的间距会小于 1，
    // 那样补点数量会暴涨（跨度 1000 像素时要落 3000 个点），
    // 而实际精度并没有提高 —— 整数网格上 1 像素已经是最小有效步长。
    const int diameter = m_options ? m_options->size() : 20;
    return qMax(1.0, diameter * 0.25);
}

QColor BrushTool::strokeColor() const
{
    QColor c = m_options ? m_options->color() : QColor(Qt::black);

    // 不透明度乘进 alpha，作为"整笔的覆盖上限"。
    // 见头文件 strokeColor() 的说明。
    const int opacity = m_options ? qBound(0, m_options->opacity(), 100) : 100;
    c.setAlpha(c.alpha() * opacity / 100);

    return c;
}

QImage *BrushTool::writableLayerImage()
{
    if (!isReady() || m_layerIndex < 0)
        return nullptr;

    Layer *layer = document()->layers().layer(m_layerIndex);
    if (!layer || layer->isNull())
        return nullptr;

    return &layer->mutableImage();
}

// ===========================================================================
//  笔画生命周期
// ===========================================================================
bool BrushTool::beginStroke(const ToolEvent &e)
{
    if (!isReady())
        return false;

    // canWriteToActiveLayer 内部已经会发状态栏消息（"图层已锁定"等），
    // 所以这里不再重复提示。
    if (!canWriteToActiveLayer())
        return false;

    Document *doc = document();
    const int index = doc->activeLayerIndex();
    Layer *layer = doc->layers().layer(index);
    if (!layer || layer->isNull())
        return false;

    // 画布尺寸为 0 的退化文档（理论上不会出现，但要防）
    if (doc->canvasSize().isEmpty())
        return false;

    // ── 落笔前的整层快照 ──
    //
    // ★ 这一行是浅拷贝（只加一次引用计数），不是 48MB 的深拷贝 ★
    //   真正的拷贝发生在稍后第一次写图层位图时，由 QImage 的
    //   写时复制机制自动完成。详见头文件里的说明。
    m_originalImage = layer->image();
    m_layerIndex = index;

    // ── 整笔覆盖度蒙版 ──
    m_strokeMask = QImage(doc->canvasSize(), QImage::Format_Grayscale8);
    if (m_strokeMask.isNull()) {
        // 内存不足。不崩溃，安静地放弃这一笔。
        m_originalImage = QImage();
        m_layerIndex = -1;
        return false;
    }
    m_strokeMask.fill(0);   // 0 = 完全没被笔刷碰到

    // ── 选区快照 ──
    //
    // 只在"有活动选区"时才取。没有选区时保持空 QImage，
    // composeStroke 会把它理解为"全选"，从而跳过逐像素的选区乘法 ——
    // 这是最常见的情况，值得为它省掉一次乘法。
    const Selection &sel = doc->selection();
    if (sel.isActive()) {
        const QImage &mask = sel.mask();
        // 尺寸必须与画布一致。不一致说明选区是旧画布尺寸的残留，
        // 用它去裁切只会把笔画擦到错误的位置上。
        if (!mask.isNull() && mask.size() == doc->canvasSize()
            && mask.format() == QImage::Format_Grayscale8) {
            m_selectionMask = mask;     // 同样是浅拷贝
        } else {
            m_selectionMask = QImage();
        }
    } else {
        m_selectionMask = QImage();
    }

    m_strokeBounds = QRect();
    m_lastStampPos = e.imagePos;
    m_hasLastStamp = true;

    // 记下控件坐标与缩放比，供笔刷轮廓圆使用。
    // 落笔的瞬间轮廓也应该跟着过来，否则笔一按下轮廓就跳到别处。
    m_hoverWidgetPos = e.widgetPos;
    m_hoverViewZoom  = e.viewZoom;
    m_hoverStale     = false;

    return true;
}

QRect BrushTool::paintSegment(const QPointF &from, const QPointF &to)
{
    const BrushTip &tip = currentTip();
    if (tip.isNull())
        return QRect();

    // 流量决定单次落笔的强度。
    // 与不透明度的分工见头文件 strokeColor() 与 Brush.h 的说明。
    const qreal strength = m_options ? qBound(0.0, m_options->flow() / 100.0, 1.0)
                                     : 1.0;

    // 补点：把"上一帧到这一帧"的直线填成密集落笔点。
    // 不补点的话快速拖动会画成一串断开的圆点（"算盘珠"）。
    const QVector<QPointF> stamps = interpolateStamps(from, to, stampSpacing());

    QRect dirty;

    for (const QPointF &p : stamps) {
        // 压进累积蒙版。内部用"取最大"合成 ——
        // 这是同一笔内重叠不会越涂越深的关键。
        stampTipInPlace(m_strokeMask, tip, p, strength);

        // 累加本帧的脏矩形。用 united 而不是 union 单个点，
        // 因为一个笔尖本身就覆盖一个矩形。
        dirty = dirty.united(stampBoundingRect(tip, p));
    }

    // 与画布求交：笔尖可能有一半在画布外（贴着边缘画的常见情形）。
    // 不求交的话后面 copy(region) 会取到画布外的区域，
    // Qt 会静默返回黑色/无效像素，表现为"贴边画出一条黑带"。
    const QRect canvasRect(0, 0, m_strokeMask.width(), m_strokeMask.height());
    dirty &= canvasRect;

    if (dirty.isEmpty())
        return QRect();     // 整个笔尖都在画布外，本帧没有实际落笔

    // 累进"整笔包围盒"。松手时它就是 DrawCommand 的脏矩形。
    m_strokeBounds = m_strokeBounds.united(dirty);

    return dirty;
}

void BrushTool::applyRegion(const QRect &region)
{
    if (region.isEmpty())
        return;

    QImage *target = writableLayerImage();
    if (!target)
        return;

    // ── 三步：取基底 → 合成 → 写回 ──
    //
    // ★ 基底永远取自"落笔前的快照"，不是取自图层当前状态 ★
    //   这是整个画笔实现能做到幂等的关键：
    //   无论这一笔被重算多少次、重算顺序如何，结果都只取决于
    //   （快照, 累积蒙版）这一对输入，与调用次数无关。
    //   因此"按住鼠标不动时的微小抖动"不会让线条逐渐变深。
    const QImage basePatch = m_originalImage.copy(region);
    const QImage maskPatch = m_strokeMask.copy(region);
    const QImage selPatch = m_selectionMask.isNull() ? QImage()
                                                     : m_selectionMask.copy(region);

    const QImage composed = composeStroke(basePatch, maskPatch, strokeColor(),
                                          m_kind == Kind::Eraser, selPatch);
    if (composed.isNull())
        return;

    // ── 写回图层 ──
    //
    // 用 CompositionMode_Source 而不是默认的 SourceOver：
    // 我们要的是"把这块区域替换成合成结果"，包括它变透明的情况
    // （橡皮擦出来的透明区）。SourceOver 会把透明像素当作"不覆盖"，
    // 于是橡皮完全擦不掉东西 —— 这是最容易踩的一个坑。
    //
    // 这里构造 QPainter 也会触发图层位图的写时复制（detach），
    // 从而把 m_originalImage 持有的那份数据保护下来。
    QPainter painter(target);
    if (!painter.isActive())
        return;

    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(region.topLeft(), composed);
    painter.end();

    // ── 只让"本帧真正写过的那一块"失效 ──
    //
    // ★ 这里曾经是 markDirty()，那是一个把本功能整个架空的错误 ★
    //   markDirty() 会立起 m_fullInvalidate → CanvasView 下一帧把**整张画布**
    //   重新合成一遍。也就是说：本类费了很大力气把每帧的像素级重算限制在
    //   `region` 内（见 BrushTool.h 顶部"两层限制"），结果到了呈现层
    //   又被整图重合成抵消掉了 —— 4K 图上每帧约 430ms，拖动依然卡。
    //   实测 4K 画笔的卡顿（评估文档 §1.3）根因就在这一句。
    //
    // ★ 为什么改成 region 是安全的 ★
    //   · 本帧真正动过的像素恰好是 region（paintSegment 返回的并集矩形，
    //     已与画布求交）；
    //   · region 之外的图层像素这一帧没被写过 —— 它们保留着前几帧写入的
    //     正确值（composeStroke 对"快照 + 累积蒙版"是幂等的，见 BrushTool.h）；
    //   · 选区、画布尺寸、图层结构本帧都没动。
    //   所以"只脏 region"与"脏整图"在结果上完全一致，只是不再白算。
    //
    // ★ region 为空时无需在这里兜底 ★
    //   调用点（mousePress / mouseMove）都在 region 非空时才调用本函数；
    //   而即便漏传，markDirtyRegion(空) 的语义也是"退回全量"，是安全方向。
    document()->markDirtyRegion(region);
}

void BrushTool::finishStroke()
{
    const int layerIndex = m_layerIndex;
    const QRect bounds = m_strokeBounds;

    // 先把状态清掉，避免下面的 push() 触发重入时看到半截状态。
    setBusy(false);
    m_hasLastStamp = false;
    m_layerIndex = -1;

    Document *doc = document();
    CommandStack *stack = commandStack();

    // 真正需要压栈的条件：文档与栈都在、确实改动过像素。
    // "确实改动过"用 m_strokeBounds 非空来判断 ——
    // 用户点了画布但笔尖完全落在画布外时不该产生撤销记录。
    const bool hasChange = !bounds.isEmpty()
                           && !m_originalImage.isNull()
                           && layerIndex >= 0;

    if (doc && stack && hasChange) {
        Layer *layer = doc->layers().layer(layerIndex);
        if (layer) {
            // ★ 两个 patch 都必须是独立副本 ★
            //   QImage::copy(rect) 返回的是"从原图复制出来的一块新像素"，
            //   不是子视图。如果用其它方式（比如构造一个引用同一内存的
            //   QImage）拿到 patch，那么下次图层被改写时，
            //   撤销数据会跟着一起变 —— 撤销就失效了。
            const QImage before = m_originalImage.copy(bounds);
            const QImage after  = layer->image().copy(bounds);

            if (!before.isNull() && !after.isNull()) {
                // 只在整个笔画期间确实改动了内容时才记录。
                // before 与 after 完全相同时（例如在完全透明处用画笔
                // 画了一个 alpha 为 0 的颜色）压一条空命令没有意义，
                // 但那种情况极罕见，不值得为此做一次逐像素比较
                // （比较本身就要遍历整个脏矩形）。
                // ★ 用 standalone()：整笔自成一档撤销 ★
                //   它会取一个新的描边编号，所以"画完一笔再画一笔"
                //   一定是两条撤销记录。自己 new 并传一个固定编号的话，
                //   连续两笔会被 QUndoStack 的 mergeWith 合并成一步 ——
                //   用户画了两笔，Ctrl+Z 一次两笔全没了。
                stack->push(DrawCommand::standalone(
                    doc, layerIndex, bounds, before, after,
                    m_kind == Kind::Eraser ? tr("橡皮擦除") : tr("画笔描边")));
            }
        }
    }

    // ── 释放整笔的临时数据 ──
    //
    // 累积蒙版是画布尺寸的（4000×3000 就是 12MB），
    // 不释放的话每画一笔就多留 12MB，很快就会耗尽内存。
    m_strokeMask    = QImage();
    m_originalImage = QImage();
    m_selectionMask = QImage();
    m_strokeBounds  = QRect();
}

void BrushTool::discardStroke()
{
    // 把图层被涂改的区域恢复成落笔前的样子。
    // 只在"确实改动过"时才需要恢复 —— 否则白拷一块像素。
    if (document() && m_layerIndex >= 0
        && !m_strokeBounds.isEmpty() && !m_originalImage.isNull()) {

        Layer *layer = document()->layers().layer(m_layerIndex);
        if (layer && !layer->isNull()) {
            const QImage restore = m_originalImage.copy(m_strokeBounds);

            QPainter painter(&layer->mutableImage());
            if (painter.isActive()) {
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(m_strokeBounds.topLeft(), restore);
                painter.end();

                // 恢复的范围就是这一笔改过的范围（m_strokeBounds），
                // 所以走局部失效 —— 取消一笔不该让整张画布重算一次。
                document()->markDirtyRegion(m_strokeBounds);
            }
        }
    }

    // 与 finishStroke 一样彻底清空 —— 但**不压栈**。
    // 这正是 cancel 与 commit 的全部区别。
    setBusy(false);
    m_hasLastStamp = false;
    m_layerIndex = -1;

    m_strokeMask    = QImage();
    m_originalImage = QImage();
    m_selectionMask = QImage();
    m_strokeBounds  = QRect();
}

void BrushTool::commit()
{
    // 工具被切换走 / 窗口关闭时调用。
    // 把正在画的一笔正常收尾（记进撤销栈），而不是丢弃 ——
    // 用户点了别的工具，刚才画的东西应该留下。
    if (m_layerIndex >= 0)
        finishStroke();
}

void BrushTool::cancel()
{
    // Esc / 显式取消：这一笔不算数，图层回到落笔前，撤销栈里不留痕。
    if (m_layerIndex >= 0)
        discardStroke();
}

bool BrushTool::isBusy() const
{
    // ★ 用 m_layerIndex 判断而不是基类的 m_busy 标志 ★
    //   m_layerIndex >= 0 是"确实有一笔在进行中"的权威依据
    //   （它在 beginStroke 成功后设置、在收尾时清掉）。
    //   基类的 m_busy 只是个辅助标志，两者容易不同步。
    return m_layerIndex >= 0;
}

// ===========================================================================
//  输入事件
// ===========================================================================
ToolResult BrushTool::mousePress(const ToolEvent &e)
{
    if (!e.isPress())
        return ToolResult::ignored();

    // 只响应左键。右键留给画布的上下文菜单，
    // 中键留给 CanvasView 自己的平移手势。
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    // 已经有一笔在进行中（例如上一个 release 事件丢了）。
    // 先把它收尾，避免状态被覆盖导致那一笔永远无法撤销。
    if (m_layerIndex >= 0)
        finishStroke();

    if (!beginStroke(e))
        return ToolResult::ignored();

    // ★ 落笔即画一个点 ★
    //   用户"单击一下"应该在画布上留下一个点，而不是什么都没发生。
    //   实现上就是让 from == to 的一次补点（interpolateStamps 在
    //   距离为 0 时会返回单个落笔点）。
    const QRect dirty = paintSegment(e.imagePos, e.imagePos);
    if (!dirty.isEmpty())
        applyRegion(dirty);

    setBusy(true);
    return ToolResult::repaint();
}

ToolResult BrushTool::mouseMove(const ToolEvent &e)
{
    // 没有按键按下的移动由 hoverMove 处理（那里只更新轮廓圆）。
    if (e.buttons == Qt::NoButton)
        return ToolResult::ignored();

    // ★★ 先把悬停缓存挪到新位置 —— 这一句必须在下面**所有**提前 return 之前 ★★
    //
    //   笔刷轮廓圆画在 m_hoverWidgetPos 上，而这个缓存原本只有两处更新：
    //   beginStroke()（落笔瞬间）与 hoverMove()（不按键的悬停）。
    //
    //   可是画布是**按"此刻有没有按键"分派**的（见 CanvasView::mouseMoveEvent
    //   里"区分悬停与拖动"那段）：没有键按下 → hoverMove，有键按下 → 本函数。
    //   于是"按住鼠标拖动"这条路**一次都走不到 hoverMove** ——
    //   轮廓圆就死死钉在落笔点（beginStroke 设的那个值）不动，
    //   非要松开鼠标才恢复跟随。
    //
    //   这正是用户报的："用画笔或橡皮按住拖动时，小圆圈不跟着鼠标走，
    //   不按住的时候才跟着走。"
    //
    //   ★ 为什么放在最前面（先于"这一笔有没有落成"的判断）★
    //     图层被锁、图层为空、画布尺寸为 0 时 beginStroke() 会失败，
    //     此时 m_layerIndex 仍是 -1，下面的分支直接返回。
    //     但用户眼睛盯着的还是这个圆圈 —— 它照样得跟着鼠标走，
    //     否则"点一下没反应、圈还卡在别处"会让人以为工具坏了。
    //
    //   取色工具（ColorPickerTool::mouseMove）是同一套处理，可对照。
    m_hoverWidgetPos = e.widgetPos;
    m_hoverViewZoom  = e.viewZoom;
    m_hoverStale     = false;

    if (m_layerIndex < 0 || !m_hasLastStamp)
        return ToolResult::repaint();

    const QRect dirty = paintSegment(m_lastStampPos, e.imagePos);

    // 落笔点一定要推进，即使本帧的脏矩形为空
    // （例如笔尖整体移到了画布外）。否则光标移回来时，
    // 会从很久以前的位置补出一长条穿越画布的直线。
    m_lastStampPos = e.imagePos;

    if (!dirty.isEmpty())
        applyRegion(dirty);

    // ★ 本帧没落笔也必须重绘 ★
    //   dirty 为空只表示"这一帧没写任何像素"（笔尖整体在画布外，
    //   或者位置变了但覆盖度没变），**轮廓圆已经挪了位置** ——
    //   不重绘就等于退回上面那个 bug。
    //   重绘本身很便宜：合成缓存没失效，只是把它重贴一遍再画个圆。
    return ToolResult::repaint();
}

ToolResult BrushTool::mouseRelease(const ToolEvent &e)
{
    if (m_layerIndex < 0)
        return ToolResult::ignored();

    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    finishStroke();
    return ToolResult::repaint();
}

ToolResult BrushTool::hoverMove(const ToolEvent &e)
{
    // 缓存控件坐标与缩放比 —— 笔刷轮廓圆完全靠这两个值绘制，
    // 不需要（也不能）去问 CanvasView。见 paintScreenOverlay 的说明。
    //
    // 只在位置真的变了才重绘。否则鼠标停着不动时也会持续触发
    // 全画布重绘，白白占用 CPU（有些平台会持续发位置相同的 move 事件）。
    const bool moved = m_hoverStale
                       || qAbs(e.widgetPos.x() - m_hoverWidgetPos.x()) > 0.01
                       || qAbs(e.widgetPos.y() - m_hoverWidgetPos.y()) > 0.01;

    m_hoverWidgetPos = e.widgetPos;
    m_hoverViewZoom  = e.viewZoom;
    m_hoverStale     = false;

    if (!moved)
        return ToolResult::accepted();

    // 让画布重绘，把新的轮廓位置画出来
    requestRepaint();
    return ToolResult::repaint();
}

// ===========================================================================
//  叠加层：笔刷轮廓圆
// ===========================================================================
void BrushTool::paintScreenOverlay(QPainter &painter, CanvasView *view)
{
    // ★ 这个方法在 painter.resetTransform() 之后调用，坐标系是控件坐标 ★
    //
    //  ★★ 为什么这里完全不调用 view 的成员函数 ★★
    //
    //    原因：tools/ 层不允许 include ui/CanvasView.h（那会破坏分层），
    //    所以在编译期 CanvasView 是个**不完整类型** ——
    //    编译器根本不知道它有没有 imageToWidget() / zoom()。
    //    调用它们会直接报 "使用了未定义类型 zl::CanvasView"。
    //
    //    那坐标换算怎么办？ToolEvent 里已经带好了：
    //      · widgetPos —— 鼠标的控件坐标，正是绘制所需的值
    //      · viewZoom  —— 当前缩放比，算屏幕上的圆半径要用
    //    这两个字段本来就是为"屏幕尺寸恒定的预览"准备的，
    //    工具只需把它们缓存下来。缓存失效见 viewChanged()。
    //
    //  ★ view 允许为空 ★（原来这里写的是 `if (!view || ...)`）
    //
    //    本函数真正需要的两个量全在 m_hoverWidgetPos / m_hoverViewZoom 里，
    //    view 一个都用不上 —— 唯一的用处是"确认现在确实有个画布"。
    //    可这个判断是死的：生产路径上唯一的调用者是 CanvasView::paintEvent，
    //    它传的就是 this，永不为空。
    //
    //    留着它的代价却是实打实的：**离线自检里画不出这个圆**。
    //    而轮廓圆是一条自绘管线 —— 本项目对自绘管线的纪律是"量它画出的
    //    像素"，不量就守不住（圆心算错、半径算错、少一次 resetTransform
    //    都能全绿）。传 nullptr 直接早退，等于把这条管线整个让出去。
    //
    //    放宽之后，自检可以拿一张内存 QImage 当画布直接画、直接量
    //    （见 tests/brush_tool_test.cpp 的"轮廓圆跟随鼠标"组）。
    //    若将来真要用到 view，会在自检里立刻空指针崩掉 —— 那正是我们
    //    想要的**响亮的失败**，而不是静默地画错。
    Q_UNUSED(view)

    if (!m_options || m_hoverStale)
        return;

    // 半径 = 图像里的半径 × 当前缩放比。
    // 有了 viewZoom，这一步就是普通乘法：
    // 20 像素的笔刷在 800% 下画成 160 像素的圆（而不是被视图放大的巨圈）。
    const qreal radius = qMax(1.0, m_options->radius() * m_hoverViewZoom);

    painter.setBrush(Qt::NoBrush);

    // ── 双色描边 ──
    //   画两圈：半透明黑 + 半透明白（错开 1 像素）。
    //   单色轮廓在"同色背景"上会消失 —— 纯黑圈在暗部看不见，
    //   纯白圈在亮部看不见。黑白双圈组合则在任何背景上都可辨。
    //   这是所有绘图软件画拾取框/笔刷轮廓的标准做法。
    QPen pen(QColor(0, 0, 0, 170), 1.0);
    painter.setPen(pen);
    painter.drawEllipse(m_hoverWidgetPos, radius, radius);

    pen.setColor(QColor(255, 255, 255, 190));
    painter.setPen(pen);
    painter.drawEllipse(m_hoverWidgetPos, radius + 1.0, radius + 1.0);

    // ── 极小笔刷补一个中心十字 ──
    //   笔刷只有 1~2 像素时，轮廓圆半径不足 3 像素，圆圈看不出来，
    //   用户会以为光标消失了。补一个十字标明精确落笔位置。
    if (radius < 3.0) {
        painter.setPen(QPen(QColor(0, 0, 0, 170), 1.0));
        painter.drawLine(QPointF(m_hoverWidgetPos.x() - 5.0, m_hoverWidgetPos.y()),
                         QPointF(m_hoverWidgetPos.x() + 5.0, m_hoverWidgetPos.y()));
        painter.drawLine(QPointF(m_hoverWidgetPos.x(), m_hoverWidgetPos.y() - 5.0),
                         QPointF(m_hoverWidgetPos.x(), m_hoverWidgetPos.y() + 5.0));
    }
}

// ===========================================================================
//  视图变化
// ===========================================================================
void BrushTool::viewChanged()
{
    // 缩放/平移之后，缓存下来的控件坐标立即失效 ——
    // 同一个图像位置在新的视图下对应的屏幕位置完全不同了。
    //
    // 工具无法自行重算（需要 CanvasView 的 imageToWidget()，
    // 而 tools 层不能依赖 ui 层），所以只能标记失效、暂时不画轮廓。
    // 下一次鼠标移动会带来新的 widgetPos 与 viewZoom，届时自动恢复。
    //
    // 表现：缩放后笔刷轮廓消失一瞬，动一下鼠标就回来。
    // 这是刻意的取舍 —— 比"轮廓停留在错误的屏幕位置"好得多，
    // 后者会让用户按错误的位置落笔。
    m_hoverStale = true;
}

} // namespace zl
