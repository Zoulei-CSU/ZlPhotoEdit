#include "tools/MagicWandTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "engine/Fill.h"
#include "engine/Filters.h"
#include "commands/CommandStack.h"
#include "commands/SelectionCommand.h"

#include <QSize>

namespace zl {

// ===========================================================================
//  构造 / 身份
// ===========================================================================

MagicWandTool::MagicWandTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
{
    m_options = new MagicWandOptions(this);

    // 参数变了要重绘一次：容差/抗锯齿本身不改变**已有**选区
    // （它们只影响下一次点击），但状态栏与选项条上的提示会变，
    // 重绘一次代价极低，且避免了"改了参数界面不响应"的观感。
    connect(m_options, &ToolOptions::changed, this, [this]() {
        requestRepaint();
    });
}

MagicWandTool::~MagicWandTool()
{
    // m_options 以 this 为 parent，由 QObject 的父子关系释放；
    // 这里只断开信号，避免析构过程中还有回调打进来。
    if (m_options)
        disconnect(m_options, nullptr, this, nullptr);
}

QString MagicWandTool::id() const
{
    // ★ 持久化键：一旦发布就不能再改 ★
    //   它同时是 QSettings 的分组名、工具箱按钮的 objectName 后缀
    //   （"toolAction_magicwand"）与 icons::iconForTool() 的映射键。
    return QStringLiteral("magicwand");
}

QString MagicWandTool::displayName() const
{
    return tr("Magic Wand");
}

int MagicWandTool::shortcutKey() const
{
    return Qt::Key_W;
}

ToolOptions *MagicWandTool::options() const
{
    return m_options;
}

// ===========================================================================
//  取样并建立选区
// ===========================================================================
bool MagicWandTool::selectAt(const QPoint &imagePos, ToolModifiers modifiers)
{
    if (!isReady())
        return false;

    Document     *doc   = document();
    CommandStack *stack = commandStack();

    // ★ 没有撤销栈时直接拒绝，不"先改后说" ★
    //   与油漆桶同一条纪律：若先改了选区再发现没有撤销栈，
    //   用户就得到一份永远撤不掉的选区。
    //   （工具在文档关闭的瞬间仍存在，此时 stack 就是 nullptr。）
    if (!stack)
        return false;

    const QSize canvas = doc->canvasSize();
    if (canvas.isEmpty())
        return false;

    // 画布外点击：静默忽略（手抖点在画布外是常事，不该报错）。
    if (!doc->canvasRect().contains(imagePos))
        return false;

    // ── ① 判据取自哪张图（见头文件决定 ①）──
    //
    //   合成图 = 所见即所得，与吸管一致；
    //   活动图层 = 严格按这一层的内容判定。
    //
    //   ★ composite() 有缓存 ★
    //     文档没变时它只返回缓存引用（QImage 隐式共享，不拷贝像素），
    //     所以"取合成图"在点击这种低频操作上没有任何额外代价。
    const bool sampleAllLayers = m_options ? m_options->sampleAllLayers() : true;

    QImage source;
    if (sampleAllLayers) {
        source = doc->composite();
    } else {
        const Layer *layer = doc->layers().layer(doc->activeLayerIndex());
        if (!layer || layer->isNull())
            return false;
        source = layer->image();      // 浅拷贝，floodFill 内部不改它
    }
    if (source.isNull())
        return false;

    // ── ② 算法：与油漆桶**同一个**内核 ──
    //
    //   注意这里与油漆桶的差别只有"喂哪张图"：
    //     油漆桶喂活动图层（因为它要往那里写）；
    //     魔术棒喂上面选好的那张。
    FloodFillParams params;
    params.seed       = imagePos;
    params.tolerance  = m_options ? m_options->tolerance() : 32;
    params.contiguous = m_options ? m_options->contiguous() : true;
    params.antialias  = m_options ? m_options->antialias() : true;

    const FillResult fill = floodFill(source, params);

    // 起点越界或内存不足时 floodFill 返回空结果。
    // ★ 一个像素都没选到 → 连命令都不压 ★
    //   压一条空命令会让撤销栈里多出一条"什么都没变"的记录，
    //   用户按 Ctrl+Z 时看起来像"撤销没反应"。
    if (fill.isEmpty()) {
        showStatus(tr("There is no selectable area here"));
        return false;
    }

    // ── ③ 运算方式：按下瞬间的修饰键优先，否则用选项条上的模式 ──
    //
    //   复用 SelectionOptions 的两个静态函数，而不是在这里再写一遍映射 ——
    //   "修饰键 ⇄ 运算"与"选项枚举 ⇄ Selection::Op"各只有一处定义，
    //   否则魔术棒与矩形选框迟早会出现"同一个 Shift 含义不同"。
    const SelectionMode mode = SelectionOptions::modeFromModifiers(
        modifiers.shift, modifiers.alt,
        m_options ? m_options->mode() : SelectionMode::Replace);
    const Selection::Op op =
        static_cast<Selection::Op>(SelectionOptions::toSelectionOp(mode));

    Selection &sel = doc->selection();

    // ── ④ ★ 尺寸准备：第一次建选区之前必须先调 ★ ──
    //
    //   不做这一步的话，在刚打开的图片上第一次点魔术棒会**毫无反应**：
    //   applyMask() 发现蒙版为空、拿不到尺寸参考就静默返回了。
    //   详见 Selection::prepareCanvas() 的注释 —— 那里的假设正是"魔棒
    //   将来也会这么用"。
    sel.prepareCanvas(canvas);

    // ── ⑤ 抓"改动前"的快照 ★ 必须在任何修改之前 ★ ──
    //   放在 prepareCanvas() 之后：prepareCanvas 只可能把**尺寸失配的
    //   残留蒙版**归位成"全选"，那是它真实的改动前状态。
    const Selection before = sel.clone();

    // ── ⑥ 羽化（作用在**新蒙版**上，见头文件决定 ②）──
    QImage mask = fill.mask;
    const qreal featherRadius = m_options ? m_options->featherRadius() : 0.0;
    if (featherRadius > 0.0)
        mask = filters::featherGrayMask(mask, featherRadius);

    // ── ⑦ 合入选区：四种运算共用这一条路径 ──
    //   不实现第二条"魔棒专用"的布尔运算 —— applyMask 里已经有一份，
    //   包括"差集按比例扣减"这种容易写歪的细节。
    sel.applyMask(mask, op);

    // ── ⑧ 退化归一化（见头文件决定 ③）──
    //
    //   ★ 判据用 bounds() 而不是自己扫一遍蒙版 ★
    //     Selection::bounds() 正是"非零区域的包围盒"，对全 0 蒙版返回空矩形，
    //     而且带缓存。自己再写一个 isMaskEmpty 就等于同一件事有两份实现。
    if (sel.bounds().isEmpty())
        sel.clear();

    // 选区变了要让 canvas 重画蚂蚁线。与 SelectionTool 同一条路径。
    doc->markDirty();

    // ── ⑨ 值不值得记一条撤销 ──
    //
    //   "改动前是全选、改动后还是全选"只可能来自一件事：
    //   用户在一个已经全选的文档上做了个"减去"结果减没了。
    //   往撤销栈里塞一条这样的记录毫无意义，只会让 Ctrl+Z 多按一次。
    const Selection after = sel.clone();
    const bool beforeIsAllSelected = !before.isActive();
    const bool afterIsAllSelected  = !after.isActive();
    if (beforeIsAllSelected && afterIsAllSelected)
        return false;

    stack->push(new SelectionCommand(doc, before, after, tr("Magic Wand Selection")));

    // 状态栏报尺寸：用户能立刻判断"容差是不是调大了/小了"。
    const QRect bounds = after.bounds();
    showStatus(tr("Selected %1 × %2 pixels (tolerance %3)")
                   .arg(bounds.width()).arg(bounds.height())
                   .arg(params.tolerance));
    return true;
}

// ===========================================================================
//  输入事件
// ===========================================================================

ToolResult MagicWandTool::mousePress(const ToolEvent &e)
{
    if (!e.isPress() || e.button != Qt::LeftButton)
        return ToolResult::ignored();
    if (!isReady())
        return ToolResult::ignored();

    // 一次点击 = 一次选区变更 = 最多一条撤销记录。
    // 返回 repaint 而不是 committed：与油漆桶一致 —— 失败时状态栏消息
    // 已经变了，返回 ignored 会让画布以为"没人处理"，可能触发画布的默认行为。
    selectAt(e.imagePoint(), e.modifiers);
    return ToolResult::repaint();
}

} // namespace zl
