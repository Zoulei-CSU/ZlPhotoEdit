#include "tools/SelectionTool.h"

#include "tools/MarchingAnts.h"
#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "commands/CommandStack.h"
#include "commands/SelectionCommand.h"
#include "engine/Filters.h"

#include <QPainter>
#include <QPainterPath>
#include <QImage>
#include <QtMath>

namespace zl {

// ===========================================================================
//  构造 / 身份
// ===========================================================================

SelectionTool::SelectionTool(Kind kind, Document *doc, CommandStack *stack,
                             QObject *parent)
    : AbstractTool(doc, stack, parent)
    , m_kind(kind)
{
    // ★ 参数对象的设置分组共用 ★
    //   矩形与椭圆共用同一份"模式/羽化/抗锯齿"设置。
    //   用户在矩形选框里设了羽化 5px，切到椭圆选框时仍应是 5px ——
    //   若各存一份，切换形状时参数会突变，很反直觉。
    //   （这也是 ToolOptions::settingsGroup() 存在的意义。）
    m_options = new SelectionOptions(this);
    m_options->setSettingsGroup(QStringLiteral("selection"));

    // 参数一变就请求重绘：抗锯齿开关会影响正在拖动的预览外观
    connect(m_options, &ToolOptions::changed, this, [this]() {
        requestRepaint();
    });
}

SelectionTool::~SelectionTool()
{
    // m_options 以 this 为 parent，由 QObject 的父子关系自动释放。
    // 这里只断开信号，避免析构过程中还有回调打进来。
    if (m_options)
        disconnect(m_options, nullptr, this, nullptr);
}

QString SelectionTool::id() const
{
    return (m_kind == Kind::Rectangle) ? QStringLiteral("rectselect")
                                       : QStringLiteral("ellipseselect");
}

QString SelectionTool::displayName() const
{
    return (m_kind == Kind::Rectangle) ? tr("矩形选框") : tr("椭圆选框");
}

int SelectionTool::shortcutKey() const
{
    // 只有矩形选框绑 M。椭圆选框返回 0（无快捷键）。
    //
    // ★ 为什么不让两个工具都绑 M ★
    //   ToolManager::findByShortcut() 是"按 key 查工具"，命中第一个就返回。
    //   两个工具都绑 M 的话，椭圆选框永远轮不到 —— 与其留一个
    //   永远不生效的绑定（会让读代码的人误以为它能用），
    //   不如明确返回 0，由 MainWindow 统一实现"M 键轮换"。
    return (m_kind == Kind::Rectangle) ? int(Qt::Key_M) : 0;
}

ToolOptions *SelectionTool::options() const
{
    return m_options;
}

// ===========================================================================
//  几何计算
// ===========================================================================

QRectF SelectionTool::shapeRect(const ToolEvent &e) const
{
    qreal dx = e.imagePos.x() - m_startPos.x();
    qreal dy = e.imagePos.y() - m_startPos.y();

    // ── 拖动中的 Shift：约束为正方形 / 正圆 ──
    //
    // 取 |dx| 与 |dy| 的较大者作为边长，这样框"跟得上"用户的拖动方向：
    // 往右下拖时用户会自然地把手指划得更远，取较大值不会让框比"看到的"
    // 还小，手感更接近 Photoshop。
    if (e.modifiers.shift) {
        const qreal side = qMax(qAbs(dx), qAbs(dy));
        dx = (dx < 0.0) ? -side : side;
        dy = (dy < 0.0) ? -side : side;
    }

    // ── 拖动中的 Alt：以起点为中心向四周扩展 ──
    if (e.modifiers.alt) {
        return QRectF(m_startPos.x() - qAbs(dx), m_startPos.y() - qAbs(dy),
                      qAbs(dx) * 2.0, qAbs(dy) * 2.0);
    }

    // 普通情况：以起点与当前点为对角
    return QRectF(qMin(m_startPos.x(), m_startPos.x() + dx),
                  qMin(m_startPos.y(), m_startPos.y() + dy),
                  qAbs(dx), qAbs(dy));
}

QImage SelectionTool::buildShapeMask(const QRectF &rect, const QSize &canvasSize) const
{
    QImage mask(canvasSize, QImage::Format_Grayscale8);
    if (mask.isNull())
        return mask;

    // 底色 0（未选中）。合一瞬间会用 applyMask 与已有选区做布尔运算，
    // 所以这里只需要"本形状自身的蒙版"，不需要考虑已有选区。
    mask.fill(0);

    if (rect.isEmpty())
        return mask;

    QPainter painter(&mask);
    // 抗锯齿开关来自选项条。开着的边缘会有 1~254 的过渡值，
    // 这正是"灰度蒙版而非 QRegion"这个设计的价值所在。
    painter.setRenderHint(QPainter::Antialiasing, m_options->antialias());
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);

    if (m_kind == Kind::Ellipse)
        painter.drawEllipse(rect);
    else
        painter.drawRect(rect);

    painter.end();
    return mask;
}

bool SelectionTool::isMaskEmpty(const QImage &mask)
{
    if (mask.isNull())
        return true;

    const int w = mask.width();
    const int h = mask.height();
    for (int y = 0; y < h; ++y) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            if (line[x] != 0)
                return false;   // 找到第一个非零点就可以收工
        }
    }
    return true;
}

// ===========================================================================
//  鼠标事件
// ===========================================================================

ToolResult SelectionTool::mousePress(const ToolEvent &e)
{
    if (!isReady())
        return ToolResult::ignored();

    // 只响应左键。右键留给上下文菜单（"取消选择""反选""羽化"等）。
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    // ── ★ 关键：为新文档建立"尺寸参考"蒙版 ★ ──
    //
    // 不做这一步的话，在刚打开的图片上第一次拉选区会**毫无反应**：
    // Selection::applyPath() 内部发现蒙版为空、拿不到尺寸参考，
    // 就静默返回了。详见 Selection::prepareCanvas() 的注释。
    //
    // 放在 mousePress 而不是构造时：画布尺寸是随文档变化的，
    // 而工具栏只知道当前文档是谁。放在这里每次落笔都校正一遍，
    // 代价只是比较两个 QSize。
    document()->selection().prepareCanvas(document()->canvasSize());

    // ── 决定本次的运算方式（用按下瞬间的修饰键）──
    m_effectiveMode = SelectionOptions::modeFromModifiers(
        e.modifiers.shift, e.modifiers.alt, m_options->mode());

    // ── 抓"改动前"的选区快照 ──
    // 必须在任何修改发生之前抓，否则撤销时会还原成"已经被改过的状态"。
    m_before = document()->selection().clone();

    m_startPos    = e.imagePos;
    m_previewRect = QRectF();
    m_dragging    = true;
    setBusy(true);

    // 注意：这里**不**绘制任何东西、不改 Document。
    // 预览是在 mouseMove 里填 m_previewRect，然后由 paintOverlay 画的。
    return ToolResult::repaint();
}

ToolResult SelectionTool::mouseMove(const ToolEvent &e)
{
    if (!m_dragging)
        return ToolResult::ignored();

    m_previewRect = shapeRect(e);

    // 只请求重绘，不碰文档 —— 见头文件顶部"最重要的设计决定"。
    return ToolResult::repaint();
}

ToolResult SelectionTool::mouseRelease(const ToolEvent &e)
{
    if (!m_dragging)
        return ToolResult::ignored();

    // ★ 这里刻意**不**判断 e.button 是不是左键 ★
    //
    //   如果按"只有左键释放才收尾"来写，一旦出现下面两种情况就会卡死：
    //     · 用户按住左键拖动时又点了右键，Qt 先派发右键释放，
    //       那个事件被忽略 → m_dragging 一直是 true → 工具永远处于
    //       忙状态，之后所有鼠标操作都被当成"继续拖动"。
    //     · 某些平台上鼠标在拖动中被系统抢走（弹出菜单、切窗口），
    //       释放事件带着 Qt::NoButton 回来。
    //
    //   判断依据已经足够可靠：**只有 mousePress 成功过才会有
    //   m_dragging == true**。所以只要在拖动中收到任何释放事件，
    //   就应当收尾。宁可多提交一次（用户会看到选区出现），
    //   也不要让工具卡在忙状态（那是必须重启程序才能恢复的故障）。
    //
    // 用松手那一刻的位置再算一次，而不是直接用 mouseMove 留下的值 ——
    // 某些平台上最后一次 move 事件与 release 之间有微小位移，
    // 用 release 的位置能让"框的右下角"落在用户手指松开的地方。
    m_previewRect = shapeRect(e);

    const QRectF rect = m_previewRect;

    m_dragging    = false;
    m_previewRect = QRectF();
    setBusy(false);

    applySelection(rect);

    return ToolResult::committed();
}

ToolResult SelectionTool::keyPress(const ToolEvent &e)
{
    // Esc 的常规路径其实是 CanvasView::keyPressEvent 直接拦下来的
    // （它先于工具处理 Esc，因为"取消"是所有工具的通用语义），
    // 那里最终会调到本类的 cancel()。
    //
    // 这里保留一份同样的处理，是为了让工具在"不经过 CanvasView"的场景下
    // 也自洽 —— 例如将来把工具逻辑接进别的事件源，或者写自动化测试时
    // 直接给工具喂一个 Escape 事件。两条路径都只读 m_dragging，重复调用无害。
    if (e.key == Qt::Key_Escape && m_dragging) {
        cancel();
        return ToolResult::repaint();
    }
    return ToolResult::ignored();
}

// ===========================================================================
//  提交
// ===========================================================================

void SelectionTool::applySelection(const QRectF &rect)
{
    if (!isReady())
        return;

    Document  *doc = document();
    Selection &sel = doc->selection();
    const QSize canvasSize = doc->canvasSize();

    // 再校正一次尺寸（mousePress 时已经做过）。
    // 万一同一次拖动期间画布尺寸变了（例如用户用快捷键触发了裁剪），
    // 没有这一步 applyMask() 会因为"两份蒙版尺寸不一致"而静默返回，
    // 表现为"松手后什么都没发生"。这个调用本身只是个 QSize 比较，很便宜。
    sel.prepareCanvas(canvasSize);

    const Selection::Op op =
        static_cast<Selection::Op>(SelectionOptions::toSelectionOp(m_effectiveMode));

    // ── 第 1 步：生成形状蒙版 ──
    QImage shape = buildShapeMask(rect, canvasSize);
    if (shape.isNull())
        return;

    // ── 第 2 步：羽化 ★ 作用在形状蒙版上，不是最终选区 ★ ──
    //
    // ★ 这个顺序是必须的，不能反过来 ★
    //   Selection::feather() 是对整份蒙版做模糊，是"收尾步骤"。
    //   如果先与已有选区合入、再羽化，那么**老选区的边缘也会被试一次**，
    //   用户每做一次"添加"都让旧边界更糊一层，做三次就烂成一团。
    //
    //   先羽化"新加进来的那块"再合入，则老边界保持原样 ——
    //   这也正是 Photoshop 的行为。
    const qreal featherRadius = m_options->featherRadius();
    if (featherRadius > 0.0)
        shape = filters::featherGrayMask(shape, featherRadius);

    // ── 第 3 步：与已有选区按运算方式合入 ──
    //
    // ★ 四种运算共用这一条路径 ★
    //   没有为 Op::Replace 单独走 setMask()，是因为 applyMask()
    //   内部对 Replace 就是"直接赋值"，语义完全一致，
    //   而且省掉一次格式/尺寸的校验分支。
    sel.applyMask(shape, op);

    // ── 第 4 步：退化情况的归一化 ──
    //
    // "新建 + 结果全空"只可能来自一件事：用户点了一下但没拖动，
    // 或拖出的框小到不足一个像素。Photoshop 里这等价于"取消选择"。
    //
    // ★ 为什么要归一化成 clear() 而不是留一份全 0 的活动蒙版 ★
    //   全 0 蒙版在语义上是"什么都没选中"，但 isActive() 会返回 true。
    //   于是每个工具在写入像素前都会走一遍"逐像素乘蒙版"的慢路径，
    //   结果是每次操作都白算 1200 万次乘法，而最终什么都不会被写。
    //   归一化之后 isActive() 回到 false，所有快路径重新生效。
    if (op == Selection::Op::Replace && isMaskEmpty(sel.mask()))
        sel.clear();

    doc->markDirty();

    // ── 第 5 步：判断值不值得记一条撤销 ──
    //
    // 用户只是想"点一下取消选择"，而当前本来就没有选区时，
    // 往撤销栈里塞一条"取消选择"毫无意义，只会让 Ctrl+Z 多按一次。
    const Selection after = sel.clone();
    const bool beforeIsAllSelected = !m_before.isActive();
    const bool afterIsAllSelected  = !after.isActive();
    if (beforeIsAllSelected && afterIsAllSelected)
        return;

    // ── 第 6 步：压栈 ──
    //
    // ★ 每次拖动只产生一条命令 ★
    //   这就是"拖动期间不碰 Document"换来的直接好处，
    //   否则拖一下就会产生上百条撤销记录。
    const QString text = (m_kind == Kind::Rectangle) ? tr("矩形选区") : tr("椭圆选区");
    auto *cmd = new SelectionCommand(doc, m_before, after, text);
    commandStack()->push(cmd);
}

// ===========================================================================
//  叠加层
// ===========================================================================

void SelectionTool::paintOverlay(QPainter &painter, CanvasView *view)
{
    Q_UNUSED(view)

    // 没有在拖动就没有预览 —— 已经存在的选区由 CanvasView 自己画
    // （第 9 步），工具不该重复画一遍，否则两条蚂蚁线的相位不同步，
    // 看起来会像重影。
    if (!m_dragging || m_previewRect.isEmpty())
        return;

    // ── 用与最终选区完全相同的画法 ──
    //
    // ★ 这是有意的"所见即所得" ★
    //   若预览用另一种样式（比如常见的"细实线框"），松手瞬间线条
    //   风格会突变，用户会怀疑"是不是提交失败了"。
    //   复用 drawMarchingAnts 后，松手前后看到的完全是同一条线。
    //
    // ★ 相位取 0（不动）★
    //   拖动期间如果也让虚线流动，会和 CanvasView 的动画定时器
    //   抢视觉注意力，反而显得画面在抖。静止的虚线在拖动时更清晰。
    QPainterPath path;
    if (m_kind == Kind::Ellipse)
        path.addEllipse(m_previewRect);
    else
        path.addRect(m_previewRect);

    drawMarchingAnts(painter, path, 0);
}

// ===========================================================================
//  生命周期
// ===========================================================================

void SelectionTool::commit()
{
    // AbstractTool 约定的语义：切换工具/关闭文档时把未完成的操作提交掉。
    //
    // 但选区工具的情况特殊 —— 拖动过程中**文档还没有被改过**，
    // 一切都只在 m_previewRect 里。所以"提交"的正确做法不是
    // 把半成品写进去，而是**把它丢掉**：
    //   用户拖到一半突然点了别的工具，那个没松手的框本来就不算数。
    //   Photoshop 也是这个行为。
    //
    // 唯一需要收尾的是清理忙标志，否则下一个工具会看到 isBusy() == true。
    if (!m_dragging)
        return;

    m_dragging    = false;
    m_previewRect = QRectF();
    setBusy(false);

    requestRepaint();
}

void SelectionTool::cancel()
{
    // Esc：丢弃这次拖动。
    //
    // ★ 为什么这里可以做到"零成本回滚" ★
    //   因为整个拖动过程完全没碰 Document —— 既没有改选区蒙版，
    //   也没有压撤销栈。所以"取消"就是清掉预览矩形。
    //   这是"拖动不写数据"这个设计的第二个收益
    //   （第一个收益是撤销栈不会被刷屏）。
    if (!m_dragging)
        return;

    m_dragging    = false;
    m_previewRect = QRectF();
    setBusy(false);

    requestRepaint();
}

} // namespace zl
