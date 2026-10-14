#include "tools/BucketTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "engine/Fill.h"
#include "commands/CommandStack.h"
#include "commands/DrawCommand.h"

#include <QImage>

namespace zl {

// ===========================================================================
//  构造 / 身份
// ===========================================================================
BucketTool::BucketTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
{
    m_options = new BucketOptions(this);
}

BucketTool::~BucketTool() = default;

QString BucketTool::id() const
{
    return QStringLiteral("bucket");
}

QString BucketTool::displayName() const
{
    return tr("Paint Bucket");
}

int BucketTool::shortcutKey() const
{
    // Photoshop 的键位：G = 油漆桶 / 渐变。本工程不做渐变，G 独占。
    return Qt::Key_G;
}

ToolOptions *BucketTool::options() const
{
    return m_options;
}

void BucketTool::setForegroundColor(const QColor &color)
{
    if (!m_options || !color.isValid())
        return;
    m_options->setColor(color);
}

// ===========================================================================
//  填充
// ===========================================================================
bool BucketTool::fillAt(const QPoint &imagePos)
{
    if (!isReady())
        return false;

    // 锁定/隐藏图层：canWriteToActiveLayer 已经负责发状态栏消息，
    // 这里不重复提示（重复提示会在状态栏里闪两下）。
    if (!canWriteToActiveLayer())
        return false;

    Document *doc = document();
    CommandStack *stack = commandStack();

    // ★ 没有撤销栈时直接拒绝，不"先改后说" ★
    //   若先改了像素再发现没有撤销栈，用户就得到一处永远撤不掉的痕迹。
    //   （画笔在这一点上不同：它的预览本来就要写像素，收尾时才压栈。）
    if (!stack)
        return false;

    const int layerIndex = doc->activeLayerIndex();
    Layer *layer = doc->layers().layer(layerIndex);
    if (!layer || layer->isNull())
        return false;

    const QSize canvas = doc->canvasSize();
    if (canvas.isEmpty())
        return false;

    // 画布外点击：静默忽略（不报错 —— 用户手抖点在画布外是常事）
    if (!doc->canvasRect().contains(imagePos))
        return false;

    // ── ① 判据取自活动图层（理由见头文件）──
    const QImage source = layer->image();      // 浅拷贝，只在需要时才会真拷

    FloodFillParams params;
    params.seed       = imagePos;
    params.tolerance  = m_options ? m_options->tolerance() : 32;
    params.contiguous = m_options ? m_options->contiguous() : true;
    params.antialias  = m_options ? m_options->antialias() : true;

    const FillResult fill = floodFill(source, params);

    // ★ 一个像素都没填到 → 连命令都不压 ★
    //   压一条空命令会让撤销栈里多出一条"什么都没变"的记录，
    //   用户按 Ctrl+Z 时看起来像"撤销没反应"。
    if (fill.isEmpty()) {
        showStatus(tr("There is no fillable area here"));
        return false;
    }

    const QRect region = fill.bounds;

    // ── ② 取三个 patch（都是独立副本）──
    //
    //  ★ 必须是 QImage::copy(rect) 得到的独立副本 ★
    //    DrawCommand 会把 before/after 长期留在撤销栈里。
    //    若传的是"引用同一块内存的子视图"，图层下次被改写时
    //    撤销数据会跟着一起变 —— 撤销就失效了。
    const QImage basePatch = source.copy(region);
    const QImage maskPatch = fill.mask.copy(region);

    // ── 选区 ──
    //   只在"真的有一个尺寸匹配的 Grayscale8 选区"时才用；
    //   其余情况（没有选区 / 选区还是旧画布尺寸的残留）一律当全选。
    //   尺寸不匹配的选区绝不能拿来用：那会把填充裁到错误的位置上。
    QImage selPatch;
    const Selection &sel = doc->selection();
    if (sel.isActive()) {
        const QImage &m = sel.mask();
        if (!m.isNull()
            && m.size() == canvas
            && m.format() == QImage::Format_Grayscale8) {
            selPatch = m.copy(region);
        }
    }

    // ── ③ 填充色：不透明度乘进 alpha ──
    //
    //  ★ 用 QColor(r,g,b,a) 而不是 QColor(qRgba(...)) ★
    //    QColor(QRgb) 会把 alpha 丢掉、置成 255（实测确认）。
    //    于是"填充不透明度 0%"会变成 100%，图被填满而且毫无征兆。
    QColor color = m_options ? m_options->color() : QColor(Qt::black);
    const int opacity = m_options ? qBound(0, m_options->opacity(), 100) : 100;
    color = QColor(color.red(), color.green(), color.blue(),
                   color.alpha() * opacity / 100);

    // 不透明度为 0：填了等于没填，不必压命令
    if (color.alpha() == 0) {
        showStatus(tr("Fill opacity is 0% - nothing changed"));
        return false;
    }

    const QImage afterPatch = composeFill(basePatch, maskPatch, color, selPatch);
    if (afterPatch.isNull())
        return false;

    // ── ④ 压栈。真正的写入由 DrawCommand::redo() 完成 ──
    //
    // ★ 用 standalone() 而不是自己 new ★
    //   它会给这条命令取一个**新的**描边编号，于是"点一下 = 一条撤销记录"
    //   这条承诺才有保障。自己 new 并随手传个编号的话，连点两下油漆桶
    //   会与上一条撞号、被 QUndoStack 合并成一条 ——
    //   Ctrl+Z 一次把两处都撤掉，且没有任何提示。
    //   见 DrawCommand.h 构造函数上方那段注释。
    stack->push(DrawCommand::standalone(doc, layerIndex, region,
                                        basePatch, afterPatch,
                                        tr("Paint Bucket Fill")));

    showStatus(tr("Filled %1 × %2 pixels").arg(region.width()).arg(region.height()));
    return true;
}

// ===========================================================================
//  输入事件
// ===========================================================================
ToolResult BucketTool::mousePress(const ToolEvent &e)
{
    if (!e.isPress() || e.button != Qt::LeftButton)
        return ToolResult::ignored();
    if (!isReady())
        return ToolResult::ignored();

    // 成功与否都返回 repaint：失败时状态栏消息已经变了，
    // 返回 ignored 会让画布以为"没人处理"，可能触发画布自己的默认行为。
    fillAt(e.imagePoint());
    return ToolResult::repaint();
}

} // namespace zl
