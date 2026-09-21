#include "tools/TextTool.h"

#include "tools/ToolOptions.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "engine/Brush.h"           // composeStroke（与画笔、油漆桶、形状共用同一条合成路径）
#include "engine/Shape.h"           // embedMask（把局部蒙版嵌进区域坐标系）
#include "commands/CommandStack.h"
#include "commands/DrawCommand.h"

#include <QPainter>

namespace zl {

// ===========================================================================
//  构造 / 身份
// ===========================================================================
TextTool::TextTool(Document *doc, CommandStack *stack, QObject *parent)
    : AbstractTool(doc, stack, parent)
    , m_options(new TextOptions(this))       // 以 this 为 parent：生命周期随工具
{
}

TextTool::~TextTool() = default;

QString TextTool::id() const
{
    // ★ id 同时是 QSettings 分组名与"上次使用的工具"持久化键，定了就别改 ★
    return QStringLiteral("text");
}

QString TextTool::displayName() const
{
    return tr("文字");
}

int TextTool::shortcutKey() const
{
    // T = Text，与 Photoshop / GIMP 一致（肌肉记忆）。
    return Qt::Key_T;
}

QCursor TextTool::cursor() const
{
    // ★ 工字形光标，而不是十字 ★
    //   文字是"在某一点插入"，工字形表达的正是"插入点"；
    //   十字表达的是"框一个范围"（选框 / 裁剪用）。
    //   一个工具的光标就是在告诉用户"这一步会发生什么"。
    return QCursor(Qt::IBeamCursor);
}

ToolOptions *TextTool::options() const
{
    return m_options;
}

void TextTool::setForegroundColor(const QColor &color)
{
    if (!color.isValid() || !m_options)
        return;
    // 前景色就是文字颜色。不透明度就是颜色自带的 alpha。
    m_options->setColor(color);
}

QFont TextTool::editorFont() const
{
    // ★ DPI 从文档取，不从屏幕取 ★
    //   换算规则的唯一实现在 TextOptions::toFont() 里（注释写了为什么
    //   必须用像素字号而不是磅值字号）。这里只负责把"文档的物理分辨率"
    //   喂给它 —— 目标像素算错的话，同一份工程在两台机器上字号会不同。
    const qreal dpi = document() ? document()->dpiX() : 72.0;
    return m_options->toFont(dpi);
}

// ===========================================================================
//  编辑会话
// ===========================================================================
bool TextTool::beginEditAt(const QPointF &imagePos, bool notifyUi)
{
    if (!isReady())
        return false;

    // 图层隐藏 / 锁定时这会发状态栏消息，所以这里不重复提示。
    // ★ 但提示的时机要放在**开浮层之前** ★ —— 否则用户敲完一段字、
    //   确认之后才发现"当前图层不能写"，白敲一场。
    if (!canWriteToActiveLayer())
        return false;

    Document *doc = document();
    if (doc->canvasSize().isEmpty())
        return false;                       // 退化文档，防除零

    // ★ 没有撤销栈就干脆不让开始 ★
    //   否则用户敲完确认，图层上多了一块**永远撤不掉**的痕迹。
    //   油漆桶与形状工具是同一条纪律。
    if (!commandStack()) {
        showStatus(tr("没有可用的撤销栈，已取消输入"));
        return false;
    }

    m_anchor  = imagePos;
    m_text.clear();
    m_editing = true;
    setBusy(true);

    if (notifyUi) {
        emit requestTextEditor(m_anchor, editorFont(), m_options->color(),
                               int(m_options->align()), m_options->lineHeight());
    }
    return true;
}

void TextTool::clearEditState()
{
    m_editing = false;
    m_text.clear();
    m_anchor = QPointF();
    setBusy(false);
}

void TextTool::setEditingText(const QString &text)
{
    // ★ 只在编辑会话进行中接受 ★
    //   浮层被销毁时会再发一次 textChanged（Qt 在控件析构前会清空内容），
    //   若不挡住，就会出现"已经 commit 了又被推一次文本" —— 状态看起来
    //   无害（只是给一个空闲的工具写了 m_text），但下一次 beginEditAt
    //   如果忘了 clear，上一段文字就会**残留**到新会话里。
    //   这类"晚到的信号"是界面里最常见的时序问题，用状态判定挡在门口最省事。
    if (!m_editing)
        return;
    m_text = text;
}

bool TextTool::hasInlineEditor() const
{
    return m_editing;
}

void TextTool::commit()
{
    if (!m_editing)
        return;

    // 先把状态摘下来再落像素：renderText 会 push()，而 push 会触发
    // QUndoStack 的多个信号 → 画布重绘 → 理论上可能回到本工具。
    // 带着"半截状态"重入是很多古怪 bug 的来源（形状工具同样先清状态）。
    const QString text   = m_text;
    const QPointF anchor = m_anchor;
    clearEditState();

    if (text.isEmpty()) {
        // 开了浮层但一个字都没敲（用户点了画布就 Esc/点了别处）。
        // **不压命令、不提示、不改像素** —— 提示"什么都没发生"反而像报错。
        return;
    }

    renderText(text, anchor);
}

void TextTool::cancel()
{
    clearEditState();
    AbstractTool::cancel();
}

bool TextTool::isBusy() const
{
    // 与画笔/形状同一条理由：状态标志才是"确实有一次操作在进行"的权威依据，
    // 而不是基类那个可能被别处置位的 m_busy。
    return m_editing;
}

void TextTool::setDocument(Document *doc)
{
    // 换文档时浮层必须收掉：它是挂在**旧**画布上的，而新文档的画布尺寸、
    // 图层结构都不一样，留着它只会让锚点落在一个不存在的位置上。
    // 这里按"取消"处理而不是"提交" —— 用户切换文档时多半没打算确认那段文字，
    // 而写入旧文档又会被随后的文档切换掩盖掉（不可见、不可撤销，最难排查）。
    if (m_editing)
        cancel();

    AbstractTool::setDocument(doc);
}

// ===========================================================================
//  落像素
// ===========================================================================
bool TextTool::renderText(const QString &text, const QPointF &anchor)
{
    if (!isReady())
        return false;
    if (!canWriteToActiveLayer())
        return false;
    if (text.isEmpty())
        return false;

    Document *doc = document();
    const QSize canvas = doc->canvasSize();
    if (canvas.isEmpty())
        return false;

    const int index = doc->activeLayerIndex();
    Layer *layer = doc->layers().layer(index);
    if (!layer || layer->isNull())
        return false;

    CommandStack *stack = commandStack();
    if (!stack) {
        showStatus(tr("没有可用的撤销栈，文字未写入"));
        return false;
    }

    // ── 排版 + 光栅化（引擎）──
    const TextMask tm = rasterizeText(text, editorFont(), m_options->align(),
                                      m_options->lineHeight(), anchor, canvas);
    if (tm.isEmpty()) {
        // 两种情况：文字全是空白行（没有墨），或整块都在画布外。
        // 后者要给提示 —— 否则用户会以为"确认按钮坏了"。
        if (!text.trimmed().isEmpty())
            showStatus(tr("文字在画布之外，未产生改动"));
        return false;
    }

    // 区域 = 蒙版包围盒 ∩ 画布。rasterizeText 已经与画布求过交，
    // 这里再求一次是**防御性**的：将来若改了引擎的裁剪策略，
    // 下面的 copy()/drawImage() 也不会越界。
    const QRect region = tm.bounds & QRect(QPoint(0, 0), canvas);
    if (region.isEmpty())
        return false;

    // ── 选区快照 ──
    //   三条有效性条件与画笔/形状完全一致（非空 / 尺寸等于画布 /
    //   格式为 Grayscale8）。尺寸对不上的选区是旧画布尺寸的残留，
    //   拿来用会把文字裁到错误的位置上。
    QImage selectionMask;
    const Selection &sel = doc->selection();
    if (sel.isActive()) {
        const QImage &mask = sel.mask();
        if (!mask.isNull()
            && mask.size() == canvas
            && mask.format() == QImage::Format_Grayscale8) {
            selectionMask = mask;           // 浅拷贝
        }
    }

    // ── 合成：复用 composeStroke，不新写一行合成代码 ──
    //   于是"选区限制""预乘空间运算""颜色 alpha 即透明度"三条规则
    //   在整个工程里仍然只有一份实现（这是引擎只产出蒙版的意义）。
    const QImage before = layer->image().copy(region);
    if (before.isNull())
        return false;

    const QImage embedded = embedMask(tm.mask, tm.bounds, region);
    if (embedded.isNull())
        return false;

    const QImage selPatch = selectionMask.isNull() ? QImage()
                                                  : selectionMask.copy(region);

    const QImage after = composeStroke(before, embedded, m_options->color(),
                                       false, selPatch);
    if (after.isNull() || after.size() != region.size())
        return false;

    // ── 写回 ──
    //   用 CompositionMode_Source：要的是"把这块区域替换成重算结果"。
    //   SourceOver 会把"比原来更透明"当成"不覆盖"。
    {
        QImage *target = &layer->mutableImage();
        QPainter painter(target);
        if (!painter.isActive())
            return false;
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(region.topLeft(), after);
        painter.end();
    }

    // ★ standalone()：一次输入 = 一条撤销记录 ★
    //   它取一个新的描边编号，所以"写一段、再写一段"一定是两条记录。
    //   自己 new 并传固定编号的话，连续两段文字会被 QUndoStack 静默合并成一步。
    stack->push(DrawCommand::standalone(doc, index, region, before, after,
                                        tr("文字")));
    doc->markDirty();
    return true;
}

bool TextTool::placeText(const QString &text, const QPointF &anchor)
{
    if (!beginEditAt(anchor, /*notifyUi=*/false))
        return false;

    m_text = text;
    const QString captured = m_text;
    const QPointF capturedAnchor = m_anchor;

    // 与鼠标路径完全一致地收尾（包括"空文本什么都不做"与"画布外给提示"）。
    clearEditState();
    if (captured.isEmpty())
        return false;
    return renderText(captured, capturedAnchor);
}

// ===========================================================================
//  输入事件
// ===========================================================================
ToolResult TextTool::mousePress(const ToolEvent &e)
{
    if (!e.isPress())
        return ToolResult::ignored();

    // 只响应左键（右键归画布上下文菜单，中键归画布平移）
    if (e.button != Qt::LeftButton)
        return ToolResult::ignored();

    // ★ 已经有一段文字在编辑 → 先把它提交 ★
    //   与 Photoshop 一致：点别处 = 上一段写完了。
    //   注意顺序：必须先 commit（它会把浮层里的文本落到图层），
    //   再 beginEditAt —— 反过来会把新会话的 m_text 清掉之后
    //   又用旧文本 commit 一次。
    if (m_editing)
        commit();

    if (!beginEditAt(e.imagePos, /*notifyUi=*/true))
        return ToolResult::ignored();

    // 浮层由画布去开，画面本身没变（文字还没落到图层上），
    // 但**光标 / 浮层出现**需要一次重绘，所以标 repaint。
    return ToolResult::repaint();
}

} // namespace zl
