#include "ui/CanvasView.h"

#include "compat/QtCompat.h"
#include "core/Document.h"
#include "core/Layer.h"
#include "tools/AbstractTool.h"
#include "tools/MarchingAnts.h"
#include "tools/ToolEvent.h"
#include "tools/ToolOptions.h"
#include "tools/ToolParams.h"   // TextAlign（浮层摆框时要按对齐方向算位置）

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QAction>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QtMath>
#include <QVector>
#include <QDebug>

// ── 文字输入浮层 ──
//   QTextEdit 属于 QtWidgets，只有 ui 层能创建它（tools 层刻意不依赖
//   QtWidgets，那条边界由 run_tests.sh 里"工具自检不链 ui/"守着）。
#include <QTextEdit>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QFontMetricsF>

namespace zl {

// 参考线的拖动命中容差（**控件**像素）。
//   5 是"手指感的极限"：再小就点不中（触摸板尤甚），再大就会把
//   "贴着参考线画一笔"的正常落笔吃掉。
static constexpr qreal kGuideGrabPx = 5.0;

// ============================================================================
//  构造 / 析构
// ============================================================================

CanvasView::CanvasView(QWidget *parent)
    : QWidget(parent)
{
    // ★ 为什么必须设置这两个属性 ★
    //
    //   StrongFocus —— 让控件能接收键盘事件（方向键滚动、Esc 取消）。
    //     默认的 TabFocus 不接收方向键，会导致"按方向键毫无反应"。
    //
    //   MouseTracking —— 不按键时也收到 mouseMoveEvent。
    //     没有它，状态栏的坐标显示只在拖动时更新，用户移动鼠标时数字不动。
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // 允许接受拖放（阶段七的"拖文件到窗口打开"）
    setAcceptDrops(true);

    // 自动填充背景由我们自己绘制（棋盘格 + 中性灰），
    // 关掉 Qt 的默认填充避免闪烁。
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    m_offset = QPointF(0, 0);

    // ── 工具 → 视图 的默认接线 ──
    //
    //  ★ 这一条是"自己连自己"：工具发 requestZoom，本类在 setActiveTool()
    //    里把它转成本信号，这里再落回 setZoomAt()。 ★
    //    看着绕，但它保住了分层：工具只表达"用户想放大"，
    //    "放大多少倍、锚点怎么换算"全归视图层。将来若要在 MainWindow 侧
    //    旁听（例如做"缩放历史"或状态提示），连 zoomRequested 即可，
    //    不必去改工具。
    connect(this, &CanvasView::zoomRequested, this, &CanvasView::setZoomAt);

    // ── 文字浮层跟随视图 ──
    //
    //  ★ 为什么要"自己连自己"★
    //    浮层是**控件坐标**里的一个子控件，而它锚定的是**图像坐标**里的
    //    一个点。用户一缩放/平移，那个图像点对应的控件位置就变了 ——
    //    不重新摆位的话，浮层会僵在原处，而"确定"之后文字却出现在别的地方。
    //    这是"两套坐标系 + 一个跨越它们的中间产物"的必然产物，
    //    与视图导航那批工具踩的是同一类坑（见铁律第十四章）。
    //
    //    走 viewChanged 而不是在每个 setZoom/panBy 里各调一次：视图状态的
    //    变更点有很多（滚轮、快捷键、菜单、抓手、缩放工具…），而
    //    viewChanged 是它们的**共同出口**。挂在出口上就不会漏。
    connect(this, &CanvasView::viewChanged,
            this, &CanvasView::updateTextEditorLayout);

    // 自动滚动定时器。60ms 的间隔是经验值：
    //   太短（如 16ms）会让滚动过快，用户很难精确停在目标位置；
    //   太长（如 150ms）会感觉迟滞、断断续续。
    m_autoScrollTimer = new QTimer(this);
    m_autoScrollTimer->setInterval(60);
    connect(m_autoScrollTimer, &QTimer::timeout, this, &CanvasView::onAutoScrollTimeout);

    // 光标形状：默认箭头
    setCursor(Qt::ArrowCursor);

    rebuildViewTransform();
}

CanvasView::~CanvasView() = default;

// ============================================================================
//  文档切换
// ============================================================================

void CanvasView::setDocument(Document *doc)
{
    if (m_doc == doc)
        return;

    // ── 断开旧文档的连接 ──
    if (m_doc) {
        disconnect(m_doc, nullptr, this, nullptr);
    }

    // ★ 文字浮层必须最先收掉 ★
    //   它锚定的是**旧文档**的一个图像坐标；留着它，用户会在新文档上
    //   看到一个浮在无关位置的输入框，而"确定"之后文字落进的是新文档。
    //   按"取消"处理：切文档时用户多半没打算确认那段文字。
    closeTextEditor(/*commit=*/false);

    // ★ 切换文档前先让工具收尾 ★
    //   画笔正在画一笔时用户切换了标签页，如果不 cancel，
    //   这一笔的内容会留在旧文档的图层里（预览是直接写位图的），
    //   但撤销命令没有压栈 —— 结果是"多了一道擦不掉的痕迹"。
    if (m_activeTool && m_activeTool->isBusy())
        m_activeTool->cancel();

    m_doc = doc;
    m_compositeCache = QImage();
    m_compositeDirty = true;

    // 换了文档，上一个文档的选区轮廓必须丢掉 ——
    // 否则会在新文档上看到旧文档的蚂蚁线（尺寸、内容全都不对）。
    invalidateSelectionOutline();
    updateAntsTimer();

    if (m_doc) {
        connect(m_doc, &Document::contentChanged,
                this, &CanvasView::onDocumentContentChanged);
        connect(m_doc, &Document::structureChanged,
                this, &CanvasView::onDocumentStructureChanged);
        connect(m_doc, &Document::canvasSizeChanged,
                this, &CanvasView::onCanvasSizeChanged);

        // 新文档打开后自动适合窗口 —— 否则用户可能面对一片空白
        // （图像在视野之外），会以为打开失败了。
        //
        // ★ 但控件可能还没有真实尺寸 ★
        //   主窗口的构造顺序是"先建文档 → 再 show()"，而布局要到 show()
        //   之后才把画布的最终尺寸分配下来。此处的 width()/height() 是
        //   QWidget 的默认值（640×480），照着它算出来的 fitZoom 必然偏小
        //   —— 3840×2160 的照片会被算成 16%（适合 640×480）。
        //
        //   这里先按当前尺寸算一次；zoomToFit() 会把"用户尚未调整视图"
        //   复位，之后 resizeEvent 在尺寸变化时会继续自动适合窗口，
        //   直到用户真的缩放或平移过为止。
        zoomToFit();
    } else {
        m_viewTransform.reset();
        m_zoom = 1.0;
        m_offset = QPointF(0, 0);
        rebuildViewTransform();
        emit viewChanged();
    }

    update();
}

// ============================================================================
//  工具切换
// ============================================================================

void CanvasView::setActiveTool(AbstractTool *tool)
{
    if (m_activeTool == tool)
        return;

    // ★ 顺序：先收文字浮层，再让旧工具收尾 ★
    //   浮层里的文本只有画布知道（它持有 QTextEdit），而"把这段文字落到
    //   图层上"只有工具做得到。所以必须先经过 closeTextEditor(true)：
    //   它把文本推给旧工具 → 调 commit() → 压一条撤销命令，
    //   之后下面那句 m_activeTool->commit() 才是真正的空操作。
    //   顺序反过来的话，用户敲完字、直接点另一个工具，
    //   那段文字就**凭空消失**了（既没落图层，也没有任何提示）。
    closeTextEditor(/*commit=*/true);

    // ★ 旧工具必须先收尾 ★
    //   顺序很重要：先 commit（把正在进行的操作提交），
    //   再换指针。如果先换指针再 commit，commit 就会被施加到新工具上。
    if (m_activeTool) {
        m_activeTool->commit();

        // 断开旧工具的连接，避免"工具已切换但信号还连在画布上"
        disconnect(m_activeTool, nullptr, this, nullptr);
    }

    m_activeTool = tool;

    if (m_activeTool) {
        m_activeTool->setDocument(m_doc);

        // ★ 立即把当前颜色推给新工具 ★
        //   否则新激活的画笔会用 BrushOptions 里上次保存的旧颜色，
        //   而不是用户此刻设置的前景色 —— 表现为"换了颜色再切到画笔，
        //   画出来还是旧颜色"，直到用户再动一次颜色才同步。
        m_activeTool->setForegroundColor(m_foregroundColor);
        m_activeTool->setBackgroundColor(m_backgroundColor);

        // ── 连接工具的信号 ──
        // 用 Qt::UniqueConnection 并不能防止 lambda 重复，
        // 但由于我们在断开时用了 disconnect(tool, nullptr, this, nullptr)，
        // 切换工具时旧连接一定已经清干净了，不会累积。
        connect(m_activeTool, &AbstractTool::statusMessage,
                this, &CanvasView::statusMessage);
        connect(m_activeTool, &AbstractTool::overlayChanged,
                this, QOverload<>::of(&QWidget::update));

        // 工具请求缩放 —— 转成画布的缩放请求信号
        //   （本信号在构造函数里已接回 setZoomAt()，见那里的说明）
        connect(m_activeTool, &AbstractTool::requestZoom,
                this, &CanvasView::zoomRequested);

        // ── 视图导航类工具的其余请求：直接落到视图操作上 ──
        //
        //  ★ 与 requestZoom 走两条路，但同样是"控制权留在 ui 层" ★
        //    抓手只发一个**控件位移**，panBy() 才是真正的平移实现；
        //    缩放工具只发一个**图像矩形**，zoomToRect() 才决定
        //    "铺满视口"到底怎么算（留多少边距、缩放比钳到多少）。
        //    将来若要给抓手加"惯性滑动"、给框选缩放加"动画过渡"，
        //    都只改画布这一处，两个工具一行都不用动。
        connect(m_activeTool, &AbstractTool::requestPan,
                this, &CanvasView::panBy);
        connect(m_activeTool, &AbstractTool::requestZoomToRect,
                this, &CanvasView::zoomToRect);

        connect(m_activeTool, &AbstractTool::requestForegroundColor,
                this, &CanvasView::setForegroundColor);
        connect(m_activeTool, &AbstractTool::requestBackgroundColor,
                this, &CanvasView::setBackgroundColor);

        // ── 文字工具：请求打开输入浮层 ──
        //
        //  ★ 这一条是"工具只表达意图、实现归界面层"的第三个例子 ★
        //    （前两个是 requestZoom→setZoomAt、requestZoomToRect→zoomToRect）
        //    工具说"用户在这里点了，要输入文字，字体/颜色/对齐是这样"；
        //    "浮层长什么样、放在屏幕哪里、字号乘多少"全归画布。
        connect(m_activeTool, &AbstractTool::requestTextEditor,
                this, &CanvasView::openTextEditor);
    }

    refreshCursor();
    update();
}

void CanvasView::refreshCursor()
{
    if (m_activeTool) {
        setCursor(m_activeTool->cursor());
    } else {
        setCursor(Qt::ArrowCursor);
    }
}

void CanvasView::cancelActiveTool()
{
    if (m_activeTool && m_activeTool->isBusy()) {
        m_activeTool->cancel();
        update();
    }
}

// ============================================================================
//  文字输入浮层
// ============================================================================
//
//  ★★ 为什么浮层是画布的东西、而不是工具的 ★★
//
//  QPainter::drawText 只能"画"，没法输入中文、没法编辑光标、没法退格。
//  要输入就得有一个真的 QTextEdit —— 而它是 QtWidgets 的类，
//  且必须挂在画布上才能定位到"用户点击的那一点"。
//  tools 层不允许依赖 ui 层（这条边界由 run_tests.sh 里
//  "工具自检不链 ui/" 守着），所以浮层只能由画布创建。
//
//  于是分工是：
//      工具 —— 管**内容**（什么时候开、文本是什么、什么时候落进图层）
//      画布 —— 管**这个控件**（放哪、多大、字号多少、Esc/失焦怎么收）
//
//  两端通过 AbstractTool 上的 requestTextEditor / setEditingText 牵线，
//  **画布全程不认识 TextTool 这个类**。
//
//  ★★ 这四条路径都会收浮层，且必须收在同一个出口 ★★
//
//      Esc（浮层自己的按键）      → 取消
//      失焦（点了工具箱/菜单/别处）→ 确认
//      Ctrl+Enter                  → 确认
//      换工具 / 换文档             → 见 setActiveTool / setDocument
//
//  所以只有一个 closeTextEditor()。它有四个调用点，但只有一份语义 ——
//  写成"每处各关一次"，早晚会漏掉"把指针置空"，于是浮层被删了指针还指着。
// ============================================================================
void CanvasView::openTextEditor(const QPointF &imageAnchor, const QFont &font,
                                const QColor &color, int align, qreal lineHeight)
{
    // 已经有一个在输入 → 先收掉。
    //   工具在 mousePress 里其实已经 commit 过一次（"点别处 = 上一段写完"），
    //   这里纯粹是防御：信号若被连了两次，第二份浮层会覆盖指针，
    //   第一份就成了**永远删不掉的孤儿控件**（浮在画布上，谁也点不到）。
    if (m_textEditor)
        closeTextEditor(/*commit=*/true);

    m_textEditorAnchor     = imageAnchor;
    m_textEditorAlign      = align;
    m_textEditorLineHeight = lineHeight;
    m_textEditorFont       = font;

    QTextEdit *editor = new QTextEdit(this);
    m_textEditor = editor;

    // objectName 给截图工具与自检用（按名字找控件）。
    editor->setObjectName(QStringLiteral("canvasTextEditor"));

    // ── 让浮层"看起来就是画布上的字" ──
    //
    //  ★ 三处默认值都会让浮层里的位置与落到图层上的位置错开 ★
    //    ① documentMargin 默认 4px  → 文字整体右下偏 4px
    //    ② frameWidth 默认 1px      → 再偏 1px
    //    ③ 内边距 / 滚动条          → 视口比控件窄
    //    加起来是"看起来偏 5 像素、实际 0 像素"，用户只会觉得锚点算错了。
    //    索性把边框整个去掉：浮层不像一个"框"，而像**画布上已经出现的字**
    //    （光标闪烁就是"可以打字"的提示，Photoshop 也是这样）。
    //    这样"浮层里的位置"与"最终位置"是同一个坐标系里的同一个点。
    editor->document()->setDocumentMargin(0);
    editor->setFrameShape(QFrame::NoFrame);
    editor->viewport()->setAutoFillBackground(false);
    editor->setAttribute(Qt::WA_TranslucentBackground, true);
    editor->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    editor->setAcceptRichText(false);
    editor->setContextMenuPolicy(Qt::NoContextMenu);
    editor->setStyleSheet(QStringLiteral(
        "QTextEdit{background:transparent;border:none;padding:0;}"));

    // ★ 不折行 —— 与引擎保持一致 ★
    //   引擎只按 '\n' 断行（点子文字没有"折行宽度"这个概念）。
    //   浮层若自动折行，用户看到的是两行、落到图层上却是一整行伸出画布外，
    //   会以为工具算错了。两边都"不折行"，差异就只剩"浮层宽度"
    //   这一种，而那一眼就能看出来。
    editor->setLineWrapMode(QTextEdit::NoWrap);

    // 文字颜色即"将要写进图层的颜色"。
    editor->setTextColor(color);

    // ★ 对齐：**只由框的位置表达，框内一律左对齐** ★
    //   若"按对齐摆框"+"框内也对齐"一起做，对齐就被施加了两遍：
    //   居中的文本块会被居中到"已经居中过的框"里，越偏越多。
    //   摆框用内容宽度算（见 updateTextEditorLayout），框内永远左对齐，
    //   于是三种对齐都精确落在锚点上。
    editor->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    // 行距：QFont 里没有这个概念，只能作为**块格式**设在文档上。
    // ★ 只在开浮层时设一次 ★ —— 之后新按下的回车会继承上一块的格式，
    //   所以新行同样是这个行距。若每次 viewChanged 都 merge 一遍，
    //   会顺带把光标位置重置：用户正想往中间插字时会被弹到末尾。
    {
        QTextCursor cur(editor->document());
        cur.select(QTextCursor::Document);
        QTextBlockFormat bf;
        bf.setLineHeight(int(qRound(qMax(0.1, lineHeight) * 100.0)),
                         QTextBlockFormat::ProportionalHeight);
        cur.mergeBlockFormat(bf);
    }

    // 用户敲字 → 推给工具（工具是内容的持有者）。
    connect(editor, &QTextEdit::textChanged, this, [this]() {
        if (!m_textEditor)
            return;
        if (m_activeTool)
            m_activeTool->setEditingText(m_textEditor->toPlainText());
        updateTextEditorLayout();       // 内容变了，框要跟着长
    });

    editor->installEventFilter(this);
    editor->show();
    updateTextEditorLayout();

    // 焦点给浮层，这样用户可以直接打字；画布在这个会话里收不到键盘。
    editor->setFocus(Qt::MouseFocusReason);
}

void CanvasView::closeTextEditor(bool commit)
{
    if (!m_textEditor || m_closingTextEditor)
        return;

    m_closingTextEditor = true;

    QTextEdit *editor = m_textEditor;
    // ★ 先摘指针，再做任何会引发事件的事 ★
    //   hide() 与随后的析构都会派发 FocusOut，而 FocusOut 的处理函数
    //   又会来调本函数。指针先置空，第二次进入就被开头的卫语句挡掉 ——
    //   否则会对着一个正在关闭的控件再 commit 一次，
    //   表现是"按一次确认压了两条撤销命令"。
    m_textEditor = nullptr;

    if (m_activeTool) {
        // 把最后一版文本推给工具。
        // ★ 这一句不能省 ★：用户敲完最后一个字立刻点工具箱，
        //   那次 textChanged 可能还没派发（或已排队）——
        //   少了它就会"最后几个字丢了"，而且丢得毫无痕迹。
        m_activeTool->setEditingText(editor->toPlainText());

        if (commit)
            m_activeTool->commit();     // 落像素 + 压一条命令
        else
            m_activeTool->cancel();     // 丢弃：撤销栈里不留痕
    }

    editor->hide();
    editor->deleteLater();
    m_closingTextEditor = false;

    // 焦点交回画布：否则键盘滚动、Esc 取消这些画布级快捷键会失灵。
    setFocus(Qt::OtherFocusReason);
    update();
}

void CanvasView::updateTextEditorLayout()
{
    if (!m_textEditor)
        return;

    // ── 显示字号 = 图像像素字号 × 当前缩放 ──
    //
    //  ★ 为什么必须乘 zoom ★
    //    锚点是图像坐标上的点，落到图层上的字也是"图像像素"那么大。
    //    用户放大到 800% 时，画布上的一切都放大了 8 倍，浮层里的字却保持
    //    原样的话，用户会以为"字号设置没生效"，或者以为锚点算错了。
    //
    //  ★ 每次从 m_textEditorFont 重算，不在上一次结果上乘 ★
    //    后者是复利：滚两格滚轮 → 1.25² ≈ 1.56 倍，越滚越大。
    //    （"从源头重算而不是在结果上累加"——与形状工具拖动预览同一条纪律。）
    const int px = qMax(1, int(qRound(m_textEditorFont.pixelSize() * zoom())));
    if (m_textEditor->font().pixelSize() != px) {
        QFont display = m_textEditorFont;
        display.setPixelSize(px);
        m_textEditor->setFont(display);
    }

    // ── 尺寸：跟着内容长 ──
    //   NoWrap，所以宽度只会增不会减；高度上限 480（超过就内部滚动，
    //   而不是让浮层高过整个窗口）。
    const qreal contentW = m_textEditor->document()->size().width();
    const qreal contentH = m_textEditor->document()->size().height();
    const int boxW = int(qCeil(contentW)) + 4;      // 右侧留 4px 余量防末字被切
    const int boxH = qBound(28, int(qCeil(contentH)) + 4, 480);
    if (m_textEditor->width() != boxW)
        m_textEditor->setFixedWidth(boxW);
    if (m_textEditor->height() != boxH)
        m_textEditor->setFixedHeight(boxH);

    // ── 位置：锚点（图像坐标）→ 控件坐标，再按对齐摆框 ──
    //
    //  ★ 摆框用的是**内容宽度**，不是框宽 ★
    //    框比内容宽 4px（右侧余量）。拿框宽去算居中会整体左偏 2px、
    //    算右对齐会左偏 4px —— 这种"差几个像素"的错位正是最难发现的一类。
    const QPointF p = imageToWidget(m_textEditorAnchor);
    int x = int(qRound(p.x()));
    if (m_textEditorAlign == int(TextAlign::Center))
        x -= int(qCeil(contentW)) / 2;
    else if (m_textEditorAlign == int(TextAlign::Right))
        x -= int(qCeil(contentW));

    m_textEditor->move(x, int(qRound(p.y())));
}

bool CanvasView::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_textEditor || watched != m_textEditor)
        return QWidget::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::KeyPress: {
        QKeyEvent *ke = static_cast<QKeyEvent *>(event);

        // Esc = 放弃这段文字（与画布上按 Esc 取消当前操作是同一条产品规则）
        if (ke->key() == Qt::Key_Escape) {
            closeTextEditor(/*commit=*/false);
            return true;                // 吞掉，别再让 QTextEdit 处理
        }

        // Ctrl+Enter / Ctrl+Return = 确认。
        // ★ 为什么"确认"要带 Ctrl ★
        //   裸 Enter 必须留给"换行"（这是多行文字唯一的分行手段），
        //   所以确认只能挂修饰键组合上。这也是"多行输入框里
        //   Enter 换行、Ctrl+Enter 提交"的通行约定。
        const bool ctrl = ke->modifiers().testFlag(Qt::ControlModifier);
        if (ctrl && (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)) {
            closeTextEditor(/*commit=*/true);
            return true;
        }
        break;
    }

    case QEvent::FocusOut:
        // ★ 失焦 = 确认 ★
        //   用户点了工具箱、点了图层面板、点了菜单、点了画布别处 ——
        //   全都走这一条。这就是"点别处就写完"的实现方式。
        closeTextEditor(/*commit=*/true);
        return false;                   // 不吞：FocusOut 还要继续派发
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void CanvasView::invalidateComposite()
{
    m_compositeDirty = true;
    update();
}

// ============================================================================
//  颜色
// ============================================================================

void CanvasView::setForegroundColor(const QColor &c)
{
    if (!c.isValid() || c == m_foregroundColor)
        return;
    m_foregroundColor = c;

    // ★ 把颜色推给当前工具 ★
    //   画笔、油漆桶、直线、形状、文字这些工具都需要知道前景色。
    //   工具不能自己去查（tools 层不允许依赖 ui 层），
    //   所以由画布在颜色变化时主动推过去。
    //
    //   "当前"工具这一限定不会导致遗漏：非活动工具在被激活时，
    //   setActiveTool 会补推一次最新颜色。
    if (m_activeTool)
        m_activeTool->setForegroundColor(c);

    // 颜色变化本身不影响画布内容（颜色由工具在下次操作时使用），
    // 但如果正在画，光标预览的颜色可能要变，所以还是刷一下。
    update();
}

void CanvasView::setBackgroundColor(const QColor &c)
{
    if (!c.isValid() || c == m_backgroundColor)
        return;
    m_backgroundColor = c;

    // 同 setForegroundColor，背景色也要推给工具
    // （画笔的"右键取背景色"、橡皮擦到背景色等行为会用到）
    if (m_activeTool)
        m_activeTool->setBackgroundColor(c);

    update();
}

// ============================================================================
//  视图变换
// ============================================================================

QSize CanvasView::effectiveCanvasSize() const
{
    // 预览自带画布尺寸（旋转）时以它为准；否则用文档的。
    // 没有文档时返回空尺寸 —— 调用方的 isEmpty() 判断自然成立。
    if (!m_previewCanvasSize.isEmpty())
        return m_previewCanvasSize;
    return m_doc ? m_doc->canvasSize() : QSize();
}

void CanvasView::rebuildViewTransform()
{
    // ── ★ QTransform 的组合语义（这个函数的全部难点都在这里）★ ──
    //
    //  Qt 的 QTransform 是**右乘**：每调用一次 translate/scale/rotate，
    //  都把新变换乘到当前矩阵的**右侧**。而右乘在矩阵乘法里意味着
    //  **"最先作用于点"**。所以：
    //
    //      调用顺序： c1, c2, c3        →   M = c1 · c2 · c3
    //      作用于点：   p · M = c1 · (c2 · (c3 · p))
    //                          ↑ 最后调用的 c3 最先作用于点
    //
    //  "最先作用于点" = 定义在**最内层（图像坐标）**的空间里。
    //  这一点极易搞反，用一次实测就能确认：
    //
    //      QTransform t; t.scale(0.5,0.5); t.translate(10,40);
    //      t.map(QPointF(0,0))   →  (5, 20)      ← 平移被缩放了！
    //      QTransform u; u.translate(10,40); u.scale(0.5,0.5);
    //      u.map(QPointF(0,0))   →  (10, 40)     ← 正确
    //
    //  也就是说 **translate 必须在 scale 之前调用**，否则 m_offset 会
    //  被 zoom 乘一遍。这个错误的症状很典型：缩放到 33% 时
    //  "适合窗口"明明算出 offset=(10, 45.6)，图像却出现在 (3.3, 15.0)
    //  —— 正好是 offset 乘以了 0.328。而且**缩放比越大错得越少**，
    //  在 100% 附近完全看不出来，所以特别容易蒙混过关。
    //
    // ── 目标：控件坐标 = zoom · 图像坐标 + offset ──
    //  （m_offset 的语义是"图像左上角在控件坐标里的位置"，
    //    zoomToFit / panBy / resizeEvent 等所有调用方都按这个语义写）
    //
    //  按"从外到内"的顺序排列就要写成 M = T(offset) · S(zoom) · R · F：
    //    最后调用 → 最先作用 → 图像空间里的翻转、旋转
    //    最先调用 → 最后作用 → 控件空间里的缩放、平移
    //  所以下面的调用顺序是：translate → scale → 翻转 → 旋转。
    QTransform t;

    // ── 第 1 层（最外层，最后作用于点）：缩放 + 平移到控件上的位置 ──
    t.translate(m_offset.x(), m_offset.y());
    t.scale(m_zoom, m_zoom);

    // ── 第 2 层：翻转 ──
    //  ★ 为什么翻转要在旋转之前（即更靠内层）★
    //    如果先旋转再翻转，那么"水平翻转"翻转的是**屏幕上**的水平方向，
    //    而不是图像的水平方向。用户旋转 90 度后再点"水平翻转显示"，
    //    看到的会是垂直翻转 —— 这不符合直觉。
    //    翻转定义在图像坐标系里，所以放在更内层。
    //
    //  围绕**图像中心**翻转，而不是围绕原点。
    //  围绕原点翻转会把图像翻到坐标轴的负方向去（画面凭空消失）。
    //   ★ 尺寸一律取 effectiveCanvasSize() ★
    //     预览期间画布可能比文档的大（旋转），若这里用了文档尺寸，
    //     旋转/翻转就会围绕**另一个中心**进行，画面与取景框错开。
    const QSize canvasSize = effectiveCanvasSize();
    if (!canvasSize.isEmpty() && (m_viewFlipH || m_viewFlipV)) {
        const QSizeF sz = QSizeF(canvasSize);
        t.translate(sz.width() / 2.0, sz.height() / 2.0);
        t.scale(m_viewFlipH ? -1.0 : 1.0, m_viewFlipV ? -1.0 : 1.0);
        t.translate(-sz.width() / 2.0, -sz.height() / 2.0);
    }

    // ── 第 3 层（最内层，最先作用于点）：旋转，同样围绕图像中心 ──
    if (!qFuzzyIsNull(m_viewRotation) && !canvasSize.isEmpty()) {
        const QSizeF sz = QSizeF(canvasSize);
        t.translate(sz.width() / 2.0, sz.height() / 2.0);
        t.rotate(m_viewRotation);
        t.translate(-sz.width() / 2.0, -sz.height() / 2.0);
    }

    m_viewTransform = t;
}

qreal CanvasView::zoom() const
{
    return m_zoom;
}

void CanvasView::setViewTransform(const QTransform &t)
{
    m_viewTransform = t;

    // 从矩阵反解出逻辑值。
    // ★ 这里用 m11 与 m22 的均值作为缩放比 ★
    //   若有旋转，m11 = zoom·cos(θ)，直接取 m11 会随角度变化。
    //   用 sqrt(m11² + m12²) 才是"矩阵的缩放分量"的严格定义，
    //   它不受旋转影响。
    const qreal sx = qSqrt(t.m11() * t.m11() + t.m12() * t.m12());
    const qreal sy = qSqrt(t.m21() * t.m21() + t.m22() * t.m22());
    m_zoom = (sx + sy) / 2.0;

    // 平移量：图像原点 (0,0) 映射到控件的位置
    const QPointF origin = t.map(QPointF(0, 0));
    m_offset = origin;

    emit viewChanged();
    update();
}

void CanvasView::setZoomAt(qreal factor, const QPointF &anchorWidget)
{
    // ── 以指定控件坐标点为锚点缩放 ──
    //
    //  需求：缩放前锚点下面是图像点 P，缩放后锚点下面还是 P。
    //
    //  ★ 必须在**控件空间**里围绕锚点缩放 ★
    //
    //    变换关系是：  控件坐标 = M(图像坐标)
    //    所以"锚点不动"这个约束直接写在控件空间里：
    //        新映射 M'(p) = anchor + f · ( M(p) - anchor )
    //    即  M' = A · M，其中 A 是"在控件坐标里围绕 anchor 缩放 f 倍"。
    //
    //    曾经的实现把锚点对应的**图像**坐标算出来，然后在图像空间
    //    围绕它构造缩放矩阵，再右乘到 M 上：
    //        s = T(imgPt) · S(f) · T(-imgPt)
    //        M' = M · s
    //    这是把两个坐标系混用了 —— imgPt 是图像坐标，却被当成控件坐标
    //    去平移。可以证明它只在 anchor == -imgPt 时才成立（也就是几乎
    //    从不成立），实际效果是"滚轮一转，画面猛地窜到别处"。
    //    而且它偏移方向与光标位置强相关，会让用户觉得缩放完全失控。
    //
    //    正确写法下，锚点处的图像点在缩放前后严格不变 ——
    //    view_test 里有逐项的断言钉住这一点。
    if (factor <= 0.0)
        return;

    // 锚点省略时用控件中心。
    // ★ 判断条件是 isNull() ★
    //   调用方传 QPointF() （默认构造）表示"没指定锚点"。
    //   注意坐标 (0,0) 也是一个合法的锚点（控件左上角），
    //   但它在实践中没有使用场景，因此把 (0,0) 当作"未指定"是可以接受的
    //   —— 代价是没法以精确的左上角为锚点缩放（实际上也看不出来差别）。
    QPointF anchor = anchorWidget;
    if (anchor.isNull())
        anchor = QPointF(width() / 2.0, height() / 2.0);

    // 缩放范围限制。
    //   上限 6400%（看清楚单个像素的细节足够）
    //   下限 1%（6000×4000 的图缩到 1% 是 60×40 像素，再小就没意义了）
    const qreal newZoom = qBound(0.01, m_zoom * factor, 64.0);
    const qreal actualFactor = newZoom / m_zoom;

    if (qFuzzyCompare(actualFactor, 1.0))
        return;

    // A(x) = anchor + actualFactor · (x - anchor)
    // 注意调用顺序：translate 在最前 → 它最后作用于点。
    // （若反过来写成 scale 再 translate，平移量会被缩放，见
    //   rebuildViewTransform 里的实测记录。）
    QTransform a;
    a.translate(anchor.x(), anchor.y());
    a.scale(actualFactor, actualFactor);
    a.translate(-anchor.x(), -anchor.y());

    // ★ 乘法顺序：新矩阵 = 旧矩阵 · A ★
    //
    //  这条也靠实测确定（QTransform 的组合方向极易记反）：
    //      X * Y   表示  **先 X，后 Y**
    //  所以 `m_viewTransform * a` = 先把图像映射到控件，再在控件空间里
    //  围绕锚点缩放 —— 正是我们要的。
    //
    //  反过来写成 `a * m_viewTransform` 会把 A 作用到**图像坐标**上
    //  （anchor 被当成图像坐标去平移），缩放时画面会持续漂移：
    //  实测 offset 从应有的 (-62.5, 7.0) 变成 (-14.66, 29.21)。
    m_viewTransform = m_viewTransform * a;
    m_zoom = newZoom;

    // 同步逻辑平移量：由矩阵反解，保证两者永不脱节
    m_offset = m_viewTransform.map(QPointF(0, 0));

    // 用户亲手动过缩放了 —— 此后窗口尺寸变化不再自动适合窗口
    m_userAdjustedView = true;

    emit viewChanged();
    update();
}

void CanvasView::setZoom(qreal factor, const QPointF &anchorWidget)
{
    if (factor <= 0.0 || qFuzzyCompare(factor, m_zoom))
        return;

    // 直接把缩放比设成 factor（而不是乘以当前值）。
    // 用途：缩放级别下拉框选择了 100%，此时不管当前是多少都要变成 100%。
    const qreal targetFactor = factor / m_zoom;
    setZoomAt(targetFactor, anchorWidget);
}

void CanvasView::setZoomLevel(qreal level)
{
    // 百分比 → 倍数
    setZoom(level / 100.0, QPointF(width() / 2.0, height() / 2.0));
}

QPointF CanvasView::offset() const
{
    return m_offset;
}

void CanvasView::setOffset(const QPointF &offset)
{
    if (offset == m_offset)
        return;
    m_offset = offset;

    // 用户手动平移过 —— 此后窗口尺寸变化不再自动适合窗口
    m_userAdjustedView = true;

    rebuildViewTransform();
    emit viewChanged();
    update();
}

void CanvasView::panBy(const QPointF &deltaWidget)
{
    // 平移发生在**控件坐标**里，所以直接加到 offset 上。
    //
    // ★ 为什么这里不需要考虑旋转/缩放 ★
    //   m_offset 是"图像原点的控件坐标位置"，panBy 传入的也正是
    //   控件坐标的位移。两者在同一空间，直接相加是对的。
    //   旋转只影响"图像内容画出来朝哪个方向"，不影响原点在哪。
    setOffset(m_offset + deltaWidget);
}

void CanvasView::setViewRotation(qreal degrees)
{
    // 归一化到 [0, 360)
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0)
        degrees += 360.0;

    if (qFuzzyCompare(degrees, m_viewRotation))
        return;

    // ★ 旋转时以视口中心为锚点 ★
    //   不以中心为锚点的话，旋转会让图像甩出视野。
    //   做法：先记下视口中心对应的图像点，旋转后再把它移回中心。
    const QPointF center(width() / 2.0, height() / 2.0);
    const QPointF imgAtCenter = widgetToImage(center);

    m_viewRotation = degrees;
    m_userAdjustedView = true;
    rebuildViewTransform();

    // 把旋转前位于中心的图像点重新移回中心
    const QPointF newPos = m_viewTransform.map(imgAtCenter);
    m_offset += (center - newPos);
    rebuildViewTransform();

    emit viewChanged();
    update();
}

void CanvasView::setViewFlipped(bool horizontal, bool vertical)
{
    if (m_viewFlipH == horizontal && m_viewFlipV == vertical)
        return;

    // 与旋转同样以视口中心为锚点
    const QPointF center(width() / 2.0, height() / 2.0);
    const QPointF imgAtCenter = widgetToImage(center);

    m_viewFlipH = horizontal;
    m_viewFlipV = vertical;
    m_userAdjustedView = true;
    rebuildViewTransform();

    const QPointF newPos = m_viewTransform.map(imgAtCenter);
    m_offset += (center - newPos);
    rebuildViewTransform();

    emit viewChanged();
    update();
}

// ============================================================================
//  缩放操作
// ============================================================================

qreal CanvasView::fitZoom() const
{
    const QSize canvas = effectiveCanvasSize();
    if (canvas.isEmpty())
        return 1.0;

    // 留 20 像素边距，让图像不紧贴窗口边缘 ——
    // 紧贴时用户看不出哪里是图像的边界。
    const qreal availW = qMax(1, width() - 20);
    const qreal availH = qMax(1, height() - 20);

    const qreal scaleW = availW / canvas.width();
    const qreal scaleH = availH / canvas.height();

    // 取较小者，保证两个方向都装得下
    return qMin(scaleW, scaleH);
}

void CanvasView::zoomToFit()
{
    const QSize canvas = effectiveCanvasSize();
    if (canvas.isEmpty())
        return;

    m_viewRotation = 0.0;   // 适合窗口时顺便重置视图旋转，避免"转着看"的困惑
    m_viewFlipH = false;
    m_viewFlipV = false;

    const qreal z = fitZoom();
    m_zoom = qBound(0.01, z, 64.0);

    // 居中：图像中心对齐控件中心
    const QPointF canvasCenter(canvas.width() / 2.0, canvas.height() / 2.0);
    const QPointF widgetCenter(width() / 2.0, height() / 2.0);

    // 控件坐标 = zoom · 图像坐标 + offset
    // → offset = 控件中心 - zoom · 图像中心
    m_offset = widgetCenter - QPointF(canvasCenter.x() * m_zoom,
                                      canvasCenter.y() * m_zoom);

    // "适合窗口"是一个**模式**，不是一个一次性动作：
    // 执行之后视图重新回到"随窗口尺寸自适应"的状态，
    // 直到用户亲手缩放或平移为止。
    // 这样打开照片、初启动、点菜单里的"适合窗口"三种场景都自然。
    m_userAdjustedView = false;

    rebuildViewTransform();
    emit viewChanged();
    update();
}

void CanvasView::zoomToActualSize()
{
    if (!m_doc)
        return;

    // 100% 缩放，以视口中心为锚点
    setZoom(1.0, QPointF(width() / 2.0, height() / 2.0));
}

void CanvasView::zoomToRect(const QRectF &imageRect)
{
    if (imageRect.isEmpty() || width() <= 0 || height() <= 0)
        return;

    const qreal availW = width() - 20.0;
    const qreal availH = height() - 20.0;

    const qreal z = qMin(availW / imageRect.width(), availH / imageRect.height());
    m_zoom = qBound(0.01, z, 64.0);

    // 让 imageRect 的中心对齐控件中心
    const QPointF center = imageRect.center();
    const QPointF widgetCenter(width() / 2.0, height() / 2.0);
    m_offset = widgetCenter - QPointF(center.x() * m_zoom, center.y() * m_zoom);

    // 这是程序性的取景动作（裁剪工具用它框住裁剪区），
    // 不是"适合窗口"模式 —— 所以标记为用户已调整，避免一次 resize 就把
    // 裁剪框的取景冲掉。
    m_userAdjustedView = true;

    rebuildViewTransform();
    emit viewChanged();
    update();
}

void CanvasView::zoomIn()
{
    // 以视口中心为锚点放大一档。
    //
    // ★ 步长取共享常量 kZoomStepFactor ★
    //   缩放工具"点一下"用的是**同一个数字**。两边各写一个字面量的话，
    //   迟早出现"点工具比按快捷键放得多"这种没人发现的不一致
    //   （两个入口都不报错，只是手感不同）。见 tools/ToolParams.h。
    setZoomAt(kZoomStepFactor, QPointF(width() / 2.0, height() / 2.0));
}

void CanvasView::zoomOut()
{
    setZoomAt(1.0 / kZoomStepFactor, QPointF(width() / 2.0, height() / 2.0));
}

QRectF CanvasView::visibleImageRect() const
{
    if (!m_doc)
        return QRectF();

    // 把控件矩形逆映射到图像坐标。
    // ★ 用 mapRect 而不是 map 四个角再求包围盒 ★
    //   矩阵可能包含旋转，mapRect 内部会正确处理并返回外接矩形。
    //   手工 map 四个角再取 min/max 也能得到同样结果，但容易漏掉
    //   "旋转后上下左右会互换"的情况。
    return m_viewTransform.inverted().mapRect(QRectF(rect()));
}

void CanvasView::ensureImageRectVisible(const QRectF &imageRect)
{
    if (!m_doc || imageRect.isEmpty())
        return;

    const QRectF widgetRect = imageToWidget(imageRect);
    const QRect viewport = rect();

    // 计算需要的最小位移。只在"完全超出"或"部分超出"时才滚动，
    // 不能每次都居中 —— 否则轻微移动到边缘时画面会剧烈跳动。
    QPointF delta(0, 0);

    if (widgetRect.left() < viewport.left())
        delta.setX(viewport.left() - widgetRect.left());
    else if (widgetRect.right() > viewport.right())
        delta.setX(viewport.right() - widgetRect.right());

    if (widgetRect.top() < viewport.top())
        delta.setY(viewport.top() - widgetRect.top());
    else if (widgetRect.bottom() > viewport.bottom())
        delta.setY(viewport.bottom() - widgetRect.bottom());

    if (!delta.isNull())
        panBy(delta);
}

// ============================================================================
//  坐标换算
// ============================================================================

QPointF CanvasView::widgetToImage(const QPointF &widgetPt) const
{
    // ★ inverted() 每帧都调用会有开销，但 Qt 会缓存 QTransform 的
    //   逆矩阵（inverted() 内部有 m_det 缓存），实测成本可忽略。 ★
    //   如果将来发现是瓶颈，可以缓存一份并在 rebuildViewTransform 里更新。
    bool invertible = false;
    const QTransform inv = m_viewTransform.inverted(&invertible);
    if (!invertible)
        return QPointF();   // 奇异矩阵（缩放为 0），返回原点

    return inv.map(widgetPt);
}

QPointF CanvasView::imageToWidget(const QPointF &imagePt) const
{
    return m_viewTransform.map(imagePt);
}

QPoint CanvasView::widgetToImage(const QPoint &widgetPt) const
{
    const QPointF p = widgetToImage(QPointF(widgetPt));
    return QPoint(qFloor(p.x()), qFloor(p.y()));
}

QPoint CanvasView::imageToWidget(const QPoint &imagePt) const
{
    const QPointF p = imageToWidget(QPointF(imagePt));
    return QPoint(qRound(p.x()), qRound(p.y()));
}

QRectF CanvasView::widgetToImage(const QRectF &widgetRect) const
{
    bool invertible = false;
    const QTransform inv = m_viewTransform.inverted(&invertible);
    if (!invertible)
        return QRectF();
    return inv.mapRect(widgetRect);
}

QRectF CanvasView::imageToWidget(const QRectF &imageRect) const
{
    return m_viewTransform.mapRect(imageRect);
}

// ============================================================================
//  显示选项
// ============================================================================

void CanvasView::setGridVisible(bool visible)
{
    if (m_gridVisible == visible)
        return;
    m_gridVisible = visible;
    update();
}

void CanvasView::setGridSize(int pixels)
{
    pixels = qBound(4, pixels, 2000);
    if (m_gridSize == pixels)
        return;
    m_gridSize = pixels;
    if (m_gridVisible)
        update();
}

void CanvasView::setCheckerboardVisible(bool visible)
{
    if (m_checkerboardVisible == visible)
        return;
    m_checkerboardVisible = visible;
    update();
}

// ============================================================================
//  参考线
// ============================================================================
//
//  三条约定（与图层面板/工具同族：**改动只有一条路径**）：
//    · 所有变更都走本节的几个函数，它们统一 update()，不散落在各处；
//    · 索引只在本类内部流转（外部拿到的索引立刻用，不长期持有）；
//    · 越界索引一律**静默忽略**（拖动到一半列表被清空是可能的：
//      例如拖参考线时用户按了 Esc / 切了页）。

void CanvasView::setGuidesVisible(bool visible)
{
    if (m_guidesVisible == visible)
        return;
    m_guidesVisible = visible;
    update();
}

void CanvasView::setGuidesLocked(bool locked)
{
    if (m_guidesLocked == locked)
        return;
    m_guidesLocked = locked;
    // 锁定时若正在拖动某根线，立刻中止拖拽（否则"锁了还能拖"）。
    m_draggingGuide = -1;
    update();
}

int CanvasView::addGuide(Qt::Orientation orientation, int posImage)
{
    // ★ 同一位置同方向的重复参考线直接合并 ★
    //   拖的时候手一抖就会在同一格落两次；留着两根完全重合的线，
    //   用户"拖走一根"时会发现还有一根在原地 —— 看起来像删不掉。
    for (int i = 0; i < m_guides.size(); ++i) {
        const Guide &g = m_guides.at(i);
        if (g.orientation == orientation && g.position == posImage) {
            update();
            return i;
        }
    }

    Guide g;
    g.orientation = orientation;
    g.position    = posImage;
    m_guides.append(g);
    update();
    return m_guides.size() - 1;
}

void CanvasView::moveGuide(int index, int posImage)
{
    if (index < 0 || index >= m_guides.size())
        return;
    if (m_guides.at(index).position == posImage)
        return;
    m_guides[index].position = posImage;
    update();
}

void CanvasView::removeGuide(int index)
{
    if (index < 0 || index >= m_guides.size())
        return;
    m_guides.removeAt(index);
    if (m_draggingGuide == index)
        m_draggingGuide = -1;
    else if (m_draggingGuide > index)
        --m_draggingGuide;          // 索引整体前移，拖拽目标跟着挪
    update();
}

void CanvasView::clearGuides()
{
    if (m_guides.isEmpty() && !m_hasPendingGuide)
        return;
    m_guides.clear();
    m_hasPendingGuide = false;
    m_draggingGuide   = -1;
    update();
}

void CanvasView::setGuideLocked(int index, bool locked)
{
    if (index < 0 || index >= m_guides.size())
        return;
    m_guides[index].locked = locked;
    update();
}

void CanvasView::setPendingGuide(Qt::Orientation orientation, int posImage)
{
    m_pendingGuide.orientation = orientation;
    m_pendingGuide.position    = posImage;
    m_hasPendingGuide          = true;
    update();
}

void CanvasView::clearPendingGuide()
{
    if (!m_hasPendingGuide)
        return;
    m_hasPendingGuide = false;
    update();
}

int CanvasView::guideAtWidget(const QPointF &widgetPos, qreal tolPx) const
{
    // ★ 命中判定在**控件坐标**下做，容差用控件像素 ★
    //   图像坐标下容差会随缩放变形：放大 800% 时"5 图像像素"只有 0.6 屏幕像素，
    //   用户根本点不中；缩到 10% 时又会变成一个巨大的吸附区，
    //   把正常的点击全吃掉了（与框选阈值同一条纪律）。
    if (!m_doc || m_guides.isEmpty())
        return -1;

    int   best     = -1;
    qreal bestDist = tolPx;

    for (int i = 0; i < m_guides.size(); ++i) {
        const Guide &g = m_guides.at(i);
        // 锁定（单根或全局）的线不参与拖动 —— 但仍会被画出来。
        if (g.locked || m_guidesLocked)
            continue;

        const qreal widgetCoord =
            (g.orientation == Qt::Horizontal)
                ? imageToWidget(QPointF(0.0, g.position)).y()
                : imageToWidget(QPointF(g.position, 0.0)).x();

        const qreal dist = (g.orientation == Qt::Horizontal)
                               ? qAbs(widgetPos.y() - widgetCoord)
                               : qAbs(widgetPos.x() - widgetCoord);

        if (dist <= bestDist) {
            bestDist = dist;
            best     = i;
        }
    }

    return best;
}

// ============================================================================
//  文档信号
// ============================================================================

void CanvasView::onDocumentContentChanged()
{
    m_compositeDirty = true;

    // 选区变化也会走到这里（SelectionCommand 与各选区区段都会调
    // Document::markDirty()）。轮廓缓存必须跟着失效，否则会出现
    // "选区已经改了，画面上还是旧的蚂蚁线"。
    invalidateSelectionOutline();

    // 顺带按新的选区状态决定动画要不要跑 —— 刚建立选区就启动，
    // 刚取消选区就停下。
    updateAntsTimer();

    update();
}

void CanvasView::onDocumentStructureChanged()
{
    m_compositeDirty = true;

    // 图层增删本身不影响选区，但结构变化往往伴随一次 markDirty，
    // 顺手失效一次成本极低（只是置个标志位，重算发生在下次绘制）。
    invalidateSelectionOutline();
    updateAntsTimer();

    update();
}

void CanvasView::onCanvasSizeChanged(const QSize &)
{
    // 画布尺寸变了：合成缓存必然失效。
    // ★ 这时**不要**自动调用 zoomToFit ★
    //   用户在做"图像大小"调整时，希望保持当前的缩放比来看清效果；
    //   自动适合窗口会让画面突然跳动，判断不出"改动前后差多少"。
    //   只有打开新文档时才适合窗口。
    m_compositeDirty = true;

    // 选区蒙版会被 Document 同步到新尺寸，轮廓缓存必须跟着失效 ——
    // 否则会看到"图像已经缩小了，蚂蚁线还停在原来的位置"。
    invalidateSelectionOutline();

    update();
}

// ============================================================================
//  选区蚂蚁线
// ============================================================================

void CanvasView::invalidateSelectionOutline()
{
    m_selectionOutlineDirty = true;
}

void CanvasView::updateAntsTimer()
{
    // 判断当前"应该有蚂蚁线吗"。
    //
    // 三个条件缺一不可：
    //   ① 有文档
    //   ② 选区处于活动状态（非全选）—— 蒙版值为 0 的区域没有边界可画
    //   ③ 轮廓确实非空 —— 极端情况下（例如全 0 的活动蒙版）会得到空路径
    const bool shouldRun = m_doc
                           && m_doc->selection().isActive()
                           && !m_doc->selection().mask().isNull();

    if (shouldRun) {
        if (!m_antsTimer) {
            // 惰性创建：绝大多数会话里用户可能一直不做选区，
            // 提前 new 一个 QTimer 是没必要的开销。
            m_antsTimer = new QTimer(this);
            m_antsTimer->setInterval(120);
            connect(m_antsTimer, &QTimer::timeout, this, &CanvasView::onAntsTick);
        }
        if (!m_antsTimer->isActive())
            m_antsTimer->start();
    } else {
        if (m_antsTimer && m_antsTimer->isActive())
            m_antsTimer->stop();

        // 相位归零，下次出现选区时从整齐的状态开始
        m_antsPhase = 0;
    }
}

void CanvasView::onAntsTick()
{
    // 每 120ms 推进一格虚线相位。
    //
    // ★ 只重绘轮廓附近的区域，不整块重绘 ★
    //   虚线相位改变只影响边界那 1~2 像素宽的一条带。
    //   若直接 update() 整块画布，即使有合成缓存，也要把整张图
    //   drawImage 一遍（4000×3000 缩放到窗口中仍然是可观的开销）。
    //   这里把轮廓的外接矩形映射到控件坐标、向外扩 2 像素作为重绘区，
    //   选区很小时（比如只框了一个小图标）重绘量能降低两个数量级。
    m_antsPhase = (m_antsPhase + 1) % 8;

    if (m_selectionOutline.isEmpty()) {
        update();
        return;
    }

    const QRectF widgetBounds =
        m_viewTransform.mapRect(m_selectionOutline.boundingRect());
    const QRect dirty = widgetBounds.toAlignedRect().adjusted(-2, -2, 2, 2);

    // 兜底：区域为空或异常大时退回整块重绘，避免出现"哪都不刷"的僵局。
    if (dirty.isEmpty() || dirty.width() > width() * 2 || dirty.height() > height() * 2)
        update();
    else
        update(dirty);
}

void CanvasView::rebuildSelectionOutline()
{
    m_selectionOutline = QPainterPath();
    m_selectionOutlineDirty = false;

    if (!m_doc)
        return;

    const Selection &sel = m_doc->selection();
    if (!sel.isActive())
        return;

    const QImage &mask = sel.mask();
    if (mask.isNull() || mask.format() != QImage::Format_Grayscale8)
        return;

    // ── 只扫描选区的包围盒 ──
    //
    // ★ 这是本函数最重要的优化 ★
    //   Selection::bounds() 已经带缓存，且明确给出"非零区域"的矩形。
    //   包围盒之外全是 0（未选中），那里的边界只可能是画布边缘 ——
    //   而画布边缘本来就不需要画蚂蚁线。
    //   用户框选一个小区域时，扫描量可以从"整张图"降到"那个小方块"。
    const QRect b = sel.bounds();
    if (b.isEmpty())
        return;

    // 向外扩 1 像素：边界线位于"内/外交界"处，需要访问到外圈那一圈
    // 未选中的邻居才能判断出来。
    const QRect scanRect = b.adjusted(-1, -1, 1, 1).intersected(sel.mask().rect());
    if (scanRect.isEmpty())
        return;

    const int W = scanRect.width();
    const int H = scanRect.height();
    const int ox = scanRect.x();
    const int oy = scanRect.y();

    // 先把二值化结果摊平成一维字节数组。
    // 之所以不直接访问 mask.constScanLine()：下面要在**两个方向**上
    // 做同样的扫描（横向找上下边界、纵向找左右边界），用一维数组可以
    // 用同一个索引公式表达两个方向，代码只写一遍。
    //
    // ★ 阈值取 128 ★
    //   抗锯齿与羽化产生 1~254 的过渡值。取 128 让边界落在过渡带正中，
    //   视觉上最稳定；取 1 会让蚂蚁线跑到羽化的最外圈，取 255 则跑到内圈。
    QVector<uchar> bits(W * H);
    for (int y = 0; y < H; ++y) {
        const uchar *src = mask.constScanLine(y + oy) + ox;
        uchar *dst = bits.data() + y * W;
        for (int x = 0; x < W; ++x)
            dst[x] = (src[x] >= 128) ? uchar(1) : uchar(0);
    }

    // 两个方向统一取值的访问器。
    //   dir == 0：a0 是 x、a1 是 y
    //   dir == 1：a0 是 y、a1 是 x（相当于把图转置）
    // 越界一律视为 0（未选中），这样画布边缘处也能正确生成边界线。
    auto at = [&](int dir, int a0, int a1) -> bool {
        if (dir == 0) {
            if (a0 < 0 || a1 < 0 || a0 >= W || a1 >= H) return false;
            return bits[a1 * W + a0] != 0;
        }
        if (a0 < 0 || a1 < 0 || a0 >= H || a1 >= W) return false;
        return bits[a0 * W + a1] != 0;
    };

    for (int dir = 0; dir < 2; ++dir) {
        const int len0 = (dir == 0) ? W : H;   // 沿线方向长度
        const int len1 = (dir == 0) ? H : W;   // 垂直方向长度

        for (int a1 = 0; a1 < len1; ++a1) {
            // side == 0 → 上边界（或左边界），参考格在 a1-1
            // side == 1 → 下边界（或右边界），参考格在 a1+1
            for (int side = 0; side < 2; ++side) {
                const int refOffset = (side == 0) ? -1 : +1;
                const int lineCoord = (side == 0) ? a1 : a1 + 1;

                int runStart = -1;
                // 循环多跑一格（a0 <= len0），用于在末端把最后一段收尾。
                for (int a0 = 0; a0 <= len0; ++a0) {
                    bool isEdge = false;
                    if (a0 < len0) {
                        const bool inside    = at(dir, a0, a1);
                        const bool refInside = at(dir, a0, a1 + refOffset);
                        isEdge = inside && !refInside;
                    }

                    if (isEdge) {
                        if (runStart < 0)
                            runStart = a0;
                    } else if (runStart >= 0) {
                        // [runStart, a0) 是一段连续的边界，合并成一条线段。
                        //
                        // ★ 为什么要"合并成段"而不是每个像素加一小段 ★
                        //   两个原因：
                        //   ① 虚线相位。Qt 的虚线样式是按子路径分别起算的，
                        //      如果每条子路径只有 1 像素长，虚线图案
                        //      会在每个像素处重新开始 —— 结果是画出来
                        //      看着像一条实线，完全没有"蚂蚁"的效果。
                        //   ② 性能。一条 1000 像素的边若拆成 1000 个子路径，
                        //      光路径构建与描边就要慢一个数量级。
                        const qreal f = qreal(runStart + (dir == 0 ? ox : oy));
                        const qreal t = qreal(a0       + (dir == 0 ? ox : oy));
                        const qreal lc = qreal(lineCoord + (dir == 0 ? oy : ox));

                        if (dir == 0) {
                            m_selectionOutline.moveTo(f, lc);
                            m_selectionOutline.lineTo(t, lc);
                        } else {
                            m_selectionOutline.moveTo(lc, f);
                            m_selectionOutline.lineTo(lc, t);
                        }

                        runStart = -1;
                    }
                }
            }
        }
    }
}

void CanvasView::drawSelectionAnts(QPainter &painter, const QRectF &visibleImageRect)
{
    if (!m_doc || !m_doc->selection().isActive())
        return;

    if (m_selectionOutlineDirty)
        rebuildSelectionOutline();

    if (m_selectionOutline.isEmpty())
        return;

    painter.save();

    // ── 裁剪到可视区域 ──
    //
    // 选区可能远大于窗口（整张图的四分之一之类）。不裁剪的话，
    // 光栅引擎仍要处理那些屏幕外的路径段。
    painter.setClipRect(visibleImageRect);

    // ── 具体的线条画法复用共享实现 ──
    //
    // ★ 为什么不在这个函数里直接写绘制代码 ★
    //   同样的蚂蚁线还要由 SelectionTool 画在"正在拖出来的选区预览"上。
    //   两处若各写一份，虚线段长度/颜色/粗细迟早会不一致，
    //   表现为"松手的一瞬间线条风格突然跳变"。
    //   统一走 tools/MarchingAnts.h 的 drawMarchingAnts()，
    //   保证预览与结果是同一个观感（WYSIWYG）。
    //
    //   这里处在**图像坐标系**下（painter 的变换是 m_viewTransform），
    //   而绘制用的是 cosmetic 画笔 —— 所以路径随缩放平移正确跟随，
    //   线宽却恒定 1 设备像素，放大到 800% 也不会变成粗带子。
    drawMarchingAnts(painter, m_selectionOutline, m_antsPhase);

    painter.restore();
}

// ============================================================================
//  paintEvent —— 呈现管线的 10 步
// ============================================================================

void CanvasView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    m_lastPaintTimer.start();

    QPainter painter(this);

    // 关掉默认填充（我们自己在第 1 步画背景）
    painter.setRenderHint(QPainter::Antialiasing, false);

    // ── 第 1 步：填充背景（中性灰）──
    drawBackground(painter, rect());

    if (effectiveCanvasSize().isEmpty()) {
        // 没有文档时画一句提示，比一片空白友好
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, tr("没有打开的文档"));
        return;
    }

    // ── 第 2 步：切换到图像坐标系 ──
    //
    // ★ 这是整个呈现管线的枢纽 ★
    //   执行这一行之后，下面第 3~9 步中所有的点、矩形、尺寸
    //   都用**图像坐标**书写，不需要任何手动换算。
    //   网格、参考线、工具叠加层、蚂蚁线全部受益。
    painter.setTransform(m_viewTransform);

    // ── 第 3 步：计算可视区域对应的图像矩形 ──
    const QRectF visibleImgRect = visibleImageRect();

    // ── 第 4 步：棋盘格底纹（表示透明区域）──
    //
    // ★ 棋盘格必须画在图像矩形范围内，且格子大小恒定于屏幕 ★
    //   如果格子跟着图像缩放，放大到 800% 时格子会变成巨大的方块。
    //   做法：把棋盘格画成"控件坐标的图案"，但用 clip 限制在图像矩形内。
    //   为此这里先临时 resetTransform，画完再恢复。
    if (m_checkerboardVisible) {
        painter.save();
        painter.resetTransform();

        // 图像矩形在控件坐标下的位置
        const QRectF imgRectWidget = m_viewTransform.mapRect(
            QRectF(QPointF(0, 0), QSizeF(effectiveCanvasSize())));

        // 只画与可见区域相交的部分 —— 图像远大于窗口时能省大量绘制
        const QRectF clipRect = imgRectWidget.intersected(QRectF(rect()));
        if (!clipRect.isEmpty()) {
            painter.setClipRect(clipRect);
            drawCheckerboard(painter, clipRect);
        }

        painter.restore();
    }

    // ── 第 5 步：绘制合成结果 ──
    drawComposite(painter, visibleImgRect);

    // ── 第 6 步：网格（纯视图辅助，不进入合成）──
    if (m_gridVisible)
        drawGrid(painter, visibleImgRect);

    // ── 第 7 步：参考线（纯视图辅助，不进入合成）──
    //
    // ★ 为什么排在网格之后、工具叠加层之前 ★
    //   参考线要盖住网格（它是用户亲手摆的定位基准，比网格重要）；
    //   但必须压在工具叠加层之下 —— 正在拖出来的选框/形状应当盖过参考线，
    //   否则"贴着参考线画"的时候看不清自己在画什么。
    //
    // ★ 绝不调用 Document::composite() ★ 参考线是纯呈现，
    //   "导出时不可见"因此是结构性的，而不是靠某个 if 记得跳过。
    if ((m_guidesVisible && !m_guides.isEmpty()) || m_hasPendingGuide)
        drawGuides(painter, visibleImgRect);

    // ── 第 8 步：工具叠加层（图像坐标）──
    //
    // 此时 painter 的变换仍是 m_viewTransform，所以工具里的
    // 矩形/线条直接用图像坐标写即可 —— 缩放平移自动跟随。
    drawToolOverlay(painter);

    // ── 第 8.5 步：图像边界 ──
    // 在图像坐标系里画边框，让用户看清画布的边界在哪。
    painter.save();
    painter.setTransform(QTransform());   // 边框宽度要恒定于屏幕，所以在控件坐标画
    drawBorder(painter);
    painter.restore();

    // ── 第 9 步：选区蚂蚁线 ──
    //
    // ★ 位置很讲究：必须画在工具叠加层之后、边界框之前 ★
    //   选区是"用户的操作对象"，应该盖在图像与网格之上；
    //   但它不是工具自己画的东西（工具画的是"正在拖出来的新选区预览"），
    //   所以排在 drawToolOverlay 之后，让预览能盖住旧选区边界。
    drawSelectionAnts(painter, visibleImgRect);

    // ── 第 10 步：屏幕坐标叠加层 ──
    //
    // ★ 必须 resetTransform ★
    //   笔刷轮廓圈、裁剪框角柄、位移标签这类元素需要"屏幕尺寸恒定"。
    //   不 reset 的话，放大到 800% 时笔刷轮廓会变成一个覆盖全屏的大圆。
    painter.resetTransform();
    drawToolScreenOverlay(painter);
}

// ---------------------------------------------------------------------------
//  第 1 步：背景
// ---------------------------------------------------------------------------
void CanvasView::drawBackground(QPainter &painter, const QRect &widgetRect)
{
    // 中性灰而不是纯白或纯黑：
    //   · 纯白 —— 用户会分不清"这是画布背景"还是"这是图像的白色部分"
    //   · 纯黑 —— 看深色图像时边界不清，且长时间看易疲劳
    //   中性灰（约 30% 亮度）在两种极端情况下都能看清边界。
    painter.fillRect(widgetRect, QColor(80, 80, 80));
}

// ---------------------------------------------------------------------------
//  第 4 步：棋盘格
// ---------------------------------------------------------------------------
void CanvasView::drawCheckerboard(QPainter &painter, const QRectF &clipRect)
{
    // ★ 本函数在**控件坐标**下执行（调用者已 resetTransform）★
    //
    //  棋盘格的两个颜色是透明背景的行业标准配色。
    //  用固有色而不是 palette 的颜色：棋盘格是"图像内容的一部分语义"，
    //  不应该随系统主题变色（否则同一个文件在不同主题下看起来不一样）。
    const QColor light(255, 255, 255);
    const QColor dark(204, 204, 204);

    const int cell = m_checkerboardSize;

    // ★ 起点要对齐到全局网格，而不是 clipRect 的左上角 ★
    //   如果从 clipRect 开始画，那么随着图像平移，格子的相位会跟着变 ——
    //   表现为"拖动图像时棋盘格在图像上滑动"，很像内容本身在移动，非常晕。
    //   对齐到全局网格（以控件原点为基准的整数倍）就没这个问题。
    const int startX = qFloor(clipRect.left() / cell) * cell;
    const int startY = qFloor(clipRect.top()  / cell) * cell;

    const int endX = qCeil(clipRect.right() / cell) * cell;
    const int endY = qCeil(clipRect.bottom() / cell) * cell;

    // 先整体填浅色，再只画深色格子 —— 比每格都判断颜色少一半 drawRect 调用
    painter.fillRect(clipRect, light);

    painter.setPen(Qt::NoPen);
    painter.setBrush(dark);

    for (int y = startY; y < endY; y += cell) {
        for (int x = startX; x < endX; x += cell) {
            // (x/cell + y/cell) 的奇偶性决定颜色 —— 这是标准棋盘格算法
            const int ix = x / cell;
            const int iy = y / cell;
            if (((ix + iy) & 1) == 0)
                continue;

            // 与 clip 求交，避免画出可视区域（大图缩小时能省很多）
            const QRect cellRect(x, y, cell, cell);
            const QRect clipped = cellRect.intersected(clipRect.toAlignedRect());
            if (!clipped.isEmpty())
                painter.drawRect(clipped);
        }
    }
}

// ---------------------------------------------------------------------------
//  临时呈现图（调整对话框的降采样预览）
// ---------------------------------------------------------------------------
void CanvasView::setPreviewImage(const QImage &image)
{
    setPreviewImage(image, QSize());
}

void CanvasView::setPreviewImage(const QImage &image, const QSize &canvasSize)
{
    // ★ 画布尺寸变了（旋转预览）就必须重建视图变换 ★
    //   变换里含"围绕画布中心的翻转/旋转"，中心变了矩阵就得重算 ——
    //   否则旋转/翻转会围绕旧中心进行，画面与取景框错开。
    const bool canvasChanged = (canvasSize != m_previewCanvasSize);

    m_previewImage = image;
    m_previewCanvasSize = canvasSize;

    // ★ 不动 m_compositeCache ★
    //   预览不改变文档，缓存里的合成结果依然有效。清掉它反而会
    //   在关闭对话框时白算一次 20~50ms 的合成。
    if (canvasChanged) {
        rebuildViewTransform();

        // ★ 画布变大后必须重新适配窗口 ★
        //   旋转会把画布按包围盒撑大（45° 时约 1.41 倍）。若视图不跟着
        //   适配，转出来的四个角就落在窗口之外 —— 用户看不到填充区，
        //   只会以为"旋转把内容裁掉了"。
        //
        //   只在预览画布尺寸真的变化时做（拖动角度滑块的大部分帧里
        //   尺寸是不变的），因此不会造成画面持续抖动。
        if (!canvasSize.isEmpty())
            zoomToFit();
    }

    update();
}

void CanvasView::clearPreviewImage()
{
    const bool hadPreview = !m_previewImage.isNull() || !m_previewCanvasSize.isEmpty();
    if (!hadPreview)
        return;

    m_previewImage = QImage();

    // ★ 预览画布尺寸也要一起清 ★
    //   漏掉这一行，视图会继续按"旋转后的画布"来呈现，而内容已经回到
    //   文档的真实尺寸 —— 画面右下角凭空多出一大片棋盘格，且缩放
    //   "适合窗口"也按错误的尺寸算，用户取消对话框后会看到画布像被
    //   撑大过一样。（视图变换必须跟着重建，中心也随之还原。）
    const bool canvasChanged = !m_previewCanvasSize.isEmpty();
    m_previewCanvasSize = QSize();

    if (canvasChanged)
        rebuildViewTransform();

    // 这里顺手置脏是防御性的：正常流程下"清预览"紧接着就会压入
    // 一条命令（进而触发 onDocumentContentChanged 置脏），
    // 但万一将来有别的调用路径在对话框打开期间改动了文档，
    // 这一行能保证清掉预览后画面一定是文档的真实内容。
    m_compositeDirty = true;

    update();
}

// ---------------------------------------------------------------------------
//  第 5 步：合成结果
// ---------------------------------------------------------------------------
void CanvasView::drawComposite(QPainter &painter, const QRectF &visibleImgRect)
{
    if (!m_doc)
        return;

    // ── 呈现源：预览图优先 ──
    //
    // 调整对话框打开期间画的是"参数作用在降采样副本上的结果"，
    // 而不是文档的真实内容 —— 文档在预览期间完全没被碰过。
    const bool previewing = !m_previewImage.isNull();

    // ── 合成缓存 ──
    //
    // ★ 只在内容变化时重算，缩放平移**不**重算 ★
    //   这是大图流畅缩放的关键：合成一张 4000×3000 的图要 20~50ms，
    //   如果滚轮的每个事件都触发一次合成，缩放会卡成幻灯片。
    //   而缩放只需要改变 drawImage 的目标矩形，成本几乎为零。
    //
    //   预览期间更要跳过它：否则拖动滑块时每帧都会白算一次全尺寸合成，
    //   预览的快速通道就白设计了。
    if (!previewing && (m_compositeDirty || m_compositeCache.isNull())) {
        m_compositeCache = m_doc->composite();
        m_compositeDirty = false;
    }

    const QImage &source = previewing ? m_previewImage : m_compositeCache;
    if (source.isNull())
        return;

    // ── 渲染质量 ──
    //
    // ★ 放大用 Fast，缩小用 Smooth ★
    //   放大（zoom > 1）时用平滑插值会让像素糊成一片，用户看不清
    //   单个像素的边界 —— 而"看清像素"正是放大到 400% 以上的目的。
    //   缩小（zoom < 1）时必须平滑，否则会出现严重的摩尔纹与锯齿。
    //
    //   预览图例外：它本身就是缩小过的，画的时候是在**放大**它，
    //   若还用 Fast 就会一块一块地出现马赛克。所以预览一律平滑。
    painter.setRenderHint(QPainter::SmoothPixmapTransform, previewing || m_zoom < 1.0);

    // ── 绘制 ──
    //
    // ★ 关键性能优化：只绘制可见区域 ★
    //   源矩形取"可视区域与图像矩形的交集"，只画这一块。
    //   图像 4000×3000 而窗口只有 800×600 时，绘制量减少约 25 倍。
    //
    //   注意：源矩形要用**求整**后的矩形（QImage 的坐标是整数），
    //   且必须 clip 到图像范围内，否则会出现边缘拉伸的错误像素。
    const QRectF canvasRect(QPointF(0, 0), QSizeF(effectiveCanvasSize()));
    QRectF srcRect = visibleImgRect.intersected(canvasRect);

    if (srcRect.isEmpty())
        return;   // 图像完全在视野外

    // 对齐到整数像素，并把超出的部分裁掉
    const QRect srcAligned = srcRect.toAlignedRect().intersected(canvasRect.toAlignedRect());
    if (srcAligned.isEmpty())
        return;

    // ★ 源矩形要从"画布坐标"映射到"呈现源坐标" ★
    //   预览图是同一幅图的缩小版，把 srcAligned 按尺寸比缩一下即可。
    //   全尺寸时这个比例恰好是 1.0，于是两种情形共用同一条绘制路径 ——
    //   少一个分支，也就少一处"预览与实际不一致"的可能。
    const QSize canvasSize = effectiveCanvasSize();
    if (canvasSize.isEmpty())
        return;

    const qreal sx = qreal(source.width())  / canvasSize.width();
    const qreal sy = qreal(source.height()) / canvasSize.height();

    const QRectF sourceRectF(srcAligned.x() * sx, srcAligned.y() * sy,
                             srcAligned.width() * sx, srcAligned.height() * sy);
    const QRect sourceRect = sourceRectF.toAlignedRect().intersected(source.rect());

    if (sourceRect.isEmpty())
        return;

    // ★★ 目标矩形必须用**图像坐标**书写，不能再手工 mapRect ★★
    //
    //  这是本函数唯一容易写错的地方，代价极大，所以单独说明。
    //
    //  paintEvent 第 2 步已经把 painter 的变换设成了 m_viewTransform，
    //  并明确约定"第 3~9 步全部用图像坐标书写"。也就是说：
    //
    //      drawImage(target, ...)  的 target 会被 painter 映射到屏幕上，
    //      所以 target 应该是**图像坐标**下的矩形。
    //
    //  之前的实现先把 target 手工算成屏幕坐标（m_viewTransform.mapRect），
    //  再交给这个已经被变换过的 painter —— 于是视图变换被施加了两遍：
    //
    //      T(p) = M(M(p)) = offset·(1 + zoom) + zoom² · p
    //
    //  实测症状（view_test 的 [7] 渲染占位断言把它钉住了）：
    //  3840×2160 的照片在 33% 下，期望画成 1260×709、左上角 (10, 45.6)，
    //  实际却只有 **414×232**、左上角 **(13, 61)**：
    //      414 / 1260 = 0.3286 = zoom      ← 尺寸被多乘了一次 zoom
    //      (13, 61) ≈ (10, 45.6) × 1.328   ← 偏移被放大成 offset·(1+zoom)
    //  用户看到的就是"打开照片后显示的尺寸完全不对"，而且**缩得越小错得越多**
    //  （zoom→1 时 zoom²→zoom 且 offset·(1+zoom)→2·offset，所以 100% 附近
    //   几乎看不出来，只在缩放/居中时才明显）。
    //
    //  更隐蔽的是：棋盘格（第 4 步）与图像边框（第 8.5 步）都是先
    //  resetTransform 再按屏幕坐标画的，它们**是对的** —— 于是屏幕上
    //  会出现"正确的取景框 + 里面缩成一小块的图像"，看起来像是图像本身
    //  尺寸算错了，非常容易把人引到 zoom/offset 的数学上去排查。
    painter.drawImage(QRectF(srcAligned), source, sourceRect);

    // ★ 恢复渲染质量 ★
    //   SmoothPixmapTransform 会影响后续所有绘制（网格、叠加层），
    //   开着它画 1 像素宽的网格线会变模糊。
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
}


// ---------------------------------------------------------------------------
//  第 6 步：网格
// ---------------------------------------------------------------------------
void CanvasView::drawGrid(QPainter &painter, const QRectF &visibleImgRect)
{
    if (!m_doc || m_gridSize <= 0)
        return;

    // ★ 网格间距过密时必须跳过 ★
    //   图像 6000 像素宽、网格 8 像素、缩放 10% 时，可视区域内会有
    //   750 条竖线 —— 画出来是一片实心色块，既慢又毫无意义。
    //   这里约定：屏幕上的网格间距小于 4 像素就不画。
    const qreal screenSpacing = m_gridSize * m_zoom;
    if (screenSpacing < 4.0)
        return;

    const QRectF canvasRect(QPointF(0, 0), QSizeF(effectiveCanvasSize()));
    const QRectF area = visibleImgRect.intersected(canvasRect);
    if (area.isEmpty())
        return;

    // ── 网格线宽恒定于屏幕 ──
    //   网格线是"视图辅助"，不应该随缩放变粗。
    //   在图像坐标系下，需要把笔宽设为 1/zoom，画出来才是 1 个屏幕像素。
    QPen pen(QColor(120, 120, 120, 110));
    pen.setWidthF(1.0 / qMax(0.001, m_zoom));
    pen.setCosmetic(true);   // ★ cosmetic pen 让线宽自动恒定于设备像素，无需手算倒数 ★
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // 垂直线
    const int x0 = int(qFloor(area.left()  / m_gridSize) * m_gridSize);
    const int x1 = int(qCeil(area.right()  / m_gridSize) * m_gridSize);
    for (int x = x0; x <= x1; x += m_gridSize) {
        if (x < 0 || x > effectiveCanvasSize().width())
            continue;
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }

    // 水平线
    const int y0 = int(qFloor(area.top()    / m_gridSize) * m_gridSize);
    const int y1 = int(qCeil(area.bottom()  / m_gridSize) * m_gridSize);
    for (int y = y0; y <= y1; y += m_gridSize) {
        if (y < 0 || y > effectiveCanvasSize().height())
            continue;
        painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
}

// ---------------------------------------------------------------------------
//  第 7 步：参考线
// ---------------------------------------------------------------------------
//
//  ★ 只画在画布范围内（0..canvasSize），不铺满整个视口 ★
//    铺满视口的话，画布外的灰底上也横着一条蓝线，看起来像"图被切了"。
//    参考线的语义是"图像上的某个位置"，图像之外没有意义。
void CanvasView::drawGuides(QPainter &painter, const QRectF &visibleImgRect)
{
    Q_UNUSED(visibleImgRect)

    if (!m_doc)
        return;

    const QSizeF canvas(effectiveCanvasSize());
    if (canvas.isEmpty())
        return;

    painter.save();
    painter.setBrush(Qt::NoBrush);

    // ★ cosmetic pen：永远 1 个屏幕像素宽 ★
    //   不这样写的话，放大到 800% 会变成一条 8 像素宽的蓝带子，
    //   缩到 10% 又细到看不见 —— 与网格同一条纪律（见 drawGrid）。
    QPen pen(QColor(0, 160, 240, 210));
    pen.setCosmetic(true);
    pen.setWidthF(1.0);
    painter.setPen(pen);

    for (int i = 0; i < m_guides.size(); ++i) {
        const Guide &g = m_guides.at(i);
        if (g.orientation == Qt::Horizontal)
            painter.drawLine(QPointF(0.0, g.position),
                             QPointF(canvas.width(), g.position));
        else
            painter.drawLine(QPointF(g.position, 0.0),
                             QPointF(g.position, canvas.height()));
    }

    // ── 还没落下的那一根：橙色虚线 ──
    //   虚线 + 另一个颜色 = "它还不是一根真正的参考线"，松手前随时可能取消。
    if (m_hasPendingGuide) {
        QPen pendingPen(QColor(255, 130, 20, 230));
        pendingPen.setCosmetic(true);
        pendingPen.setWidthF(1.0);
        pendingPen.setStyle(Qt::DashLine);
        painter.setPen(pendingPen);

        const Guide &g = m_pendingGuide;
        if (g.orientation == Qt::Horizontal)
            painter.drawLine(QPointF(0.0, g.position),
                             QPointF(canvas.width(), g.position));
        else
            painter.drawLine(QPointF(g.position, 0.0),
                             QPointF(g.position, canvas.height()));
    }

    painter.restore();
}

// ---------------------------------------------------------------------------
//  图像边框
// ---------------------------------------------------------------------------
void CanvasView::drawBorder(QPainter &painter)
{
    // ★ 本函数在**控件坐标**下执行 ★
    if (!m_doc)
        return;

    const QRectF imgRectWidget = m_viewTransform.mapRect(
        QRectF(QPointF(0, 0), QSizeF(effectiveCanvasSize())));

    // 深色外描边 + 浅色内描边，保证在深色和浅色图像上都能看清边界
    painter.setBrush(Qt::NoBrush);

    QPen outer(QColor(40, 40, 40, 180));
    outer.setWidth(1);
    painter.setPen(outer);
    painter.drawRect(imgRectWidget);

    QPen inner(QColor(230, 230, 230, 140));
    inner.setWidth(1);
    inner.setStyle(Qt::DotLine);
    painter.setPen(inner);
    painter.drawRect(imgRectWidget.adjusted(1, 1, -1, -1));
}

// ---------------------------------------------------------------------------
//  第 8 步：工具叠加层（图像坐标）
// ---------------------------------------------------------------------------
void CanvasView::drawToolOverlay(QPainter &painter)
{
    if (!m_activeTool)
        return;

    // ★ 此时 painter 的变换是 m_viewTransform ★
    //   工具内部直接用图像坐标绘制，抗锯齿打开让曲线更平滑。
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    m_activeTool->paintOverlay(painter, this);
    painter.restore();
}

// ---------------------------------------------------------------------------
//  第 10 步：工具叠加层（控件坐标）
// ---------------------------------------------------------------------------
void CanvasView::drawToolScreenOverlay(QPainter &painter)
{
    if (!m_activeTool)
        return;

    // ★ 此时 painter 已 resetTransform，坐标系是控件坐标 ★
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    m_activeTool->paintScreenOverlay(painter, this);
    painter.restore();
}

// ============================================================================
//  输入：构造 ToolEvent
// ============================================================================

ToolEvent CanvasView::makeToolEvent(QMouseEvent *event, ToolEventType type) const
{
    ToolEvent e;
    e.type = type;

    // ★★ Qt5 / Qt6 差异的收敛点 ★★
    //
    //  Qt5：QMouseEvent 有 localPos()（返回 const QPointF&）
    //  Qt6：localPos() 被删除，改为 position()
    //
    //  compat::eventPos() 用模板封装了这个差异，所以这一行在
    //  Qt5 和 Qt6 下都能编译，且工具层完全看不到任何版本判断。
    //
    //  如果不做这层封装，这个 #if 就要复制到工具层的每一个
    //  mouseXxxEvent 里 —— 十几个工具 × 三个事件 = 三十多处。
    const QPointF widgetPos = zl::eventPos(event);
    e.widgetPos = widgetPos;

    // 控件坐标 → 图像坐标
    e.imagePos = widgetToImage(widgetPos);
    e.lastImagePos = m_hasCursor ? m_cursorImagePos : e.imagePos;

    e.button  = event->button();
    e.buttons = event->buttons();
    e.modifiers = ToolModifiers::fromQt(event->modifiers());

    e.viewZoom = m_zoom;

    return e;
}

// ============================================================================
//  输入：鼠标事件
// ============================================================================

bool CanvasView::shouldDispatchToTool(QMouseEvent *event) const
{
    if (!m_activeTool)
        return false;

    // ── 空格平移时，所有鼠标事件都不给工具 ──
    //   用户在按住空格拖画布时，绝不想在图像上画出东西。
    if (m_spacePressed)
        return false;

    // ── 中键永远是"临时平移"，不给工具 ──
    //   这是 Photoshop 及大部分图形软件的标准手势。
    if (event->button() == Qt::MiddleButton)
        return false;

    // ── 右键交给上下文菜单，不给工具 ──
    //   部分工具（形状工具）需要右键取消，所以这里给工具一次机会 ——
    //   由工具自己决定是否处理。但按下阶段右键不派发，
    //   因为我们要保留"右键弹出菜单"这个通用行为。
    if (event->button() == Qt::RightButton)
        return false;

    return true;
}

void CanvasView::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);   // 点击画布时把键盘焦点拿过来

    const QPointF widgetPos = zl::eventPos(event);

    // ── 中键：开始平移 ──
    if (event->button() == Qt::MiddleButton) {
        m_middlePanning = true;
        m_panning = true;
        m_panStartWidgetPos = widgetPos;
        m_panStartOffset = m_offset;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // ── 空格按住时的左键：平移 ──
    if (m_spacePressed && event->button() == Qt::LeftButton) {
        m_panning = true;
        m_panStartWidgetPos = widgetPos;
        m_panStartOffset = m_offset;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    // ── 右键：留给 contextMenuEvent ──
    if (event->button() == Qt::RightButton) {
        event->ignore();
        return;
    }

    // ── 参考线：按下点正好压在一条可拖动的参考线上 → 拖它 ──
    //
    // ★ 为什么由画布抢在工具之前处理 ★
    //   拖参考线与"空格平移 / 中键平移"是同一类东西：**视图辅助的通用手势**，
    //   与具体工具无关（让 14 个工具各实现一遍毫无意义）。
    //   容差只有 kGuideGrabPx 个屏幕像素，所以"贴着参考线画一笔"照样画得到 ——
    //   除非落笔点正好压在线上，而那本来就是用户想拖线的地方。
    //
    // ★ 拖动过程中**不派发任何工具事件** ★
    //   否则会同时"拖线"和"画画"（画笔会在拖动轨迹上留下痕迹）。
    if (m_guidesVisible && event->button() == Qt::LeftButton) {
        const int idx = guideAtWidget(widgetPos, kGuideGrabPx);
        if (idx >= 0) {
            m_draggingGuide = idx;
            event->accept();
            return;
        }
    }

    // ── 交给工具 ──
    if (shouldDispatchToTool(event)) {
        const ToolEvent e = makeToolEvent(event, ToolEventType::Press);
        const ToolResult result = m_activeTool->mousePress(e);
        handleToolResult(result, e);

        if (result.handled) {
            // ★ 工具处理完按下之后要重新问一次光标 ★
            //   有的工具的"按下态"和"空闲态"是两个光标：抓手下按时
            //   要从"张开的手"变成"握紧的手"（这是用户唯一能确认
            //   "我确实抓住了画布"的反馈）。而光标只在 refreshCursor()
            //   里设置，所以这里必须补一次 —— 否则整个拖动过程都是张开的手，
            //   看起来像"没抓住"。
            refreshCursor();
            event->accept();
            return;
        }
    }

    event->ignore();
}

void CanvasView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF widgetPos = zl::eventPos(event);

    // ── 平移中 ──
    if (m_panning) {
        const QPointF delta = widgetPos - m_panStartWidgetPos;
        setOffset(m_panStartOffset + delta);
        event->accept();
        return;
    }

    // ── 拖动参考线中 ──
    //
    // ★ 拖的是**索引所指的那一根**，不是某个拷贝 ★
    //   所以位置直接写回列表，下一帧重绘就跟着走（moveGuide 里已 update）。
    //   ★ 索引失效要能自愈 ★ 拖动过程中用户可能按 Esc 清掉全部参考线
    //     （clearGuides 会把 m_draggingGuide 置 -1，但万一走了别的路径，
    //     这里再兜一次，不然就是越界访问）。
    if (m_draggingGuide >= 0) {
        if (m_draggingGuide < m_guides.size()) {
            const Qt::Orientation o = m_guides.at(m_draggingGuide).orientation;
            const QPointF img = widgetToImage(widgetPos);
            moveGuide(m_draggingGuide,
                      (o == Qt::Horizontal) ? qRound(img.y()) : qRound(img.x()));
        } else {
            m_draggingGuide = -1;
        }
        event->accept();
        return;
    }

    // ── 更新光标位置（状态栏用）──
    const QPointF imgPos = widgetToImage(widgetPos);

    // ★ 只在光标真正移动了才发信号 ★
    //   Qt 在某些情况下（如鼠标停着但窗口重绘、按了修饰键）也会发
    //   mouseMoveEvent。无谓的信号会让状态栏频繁刷新，进而让整条
    //   状态栏重排布局 —— 在拖动过程中这是肉眼可见的闪烁。
    //
    //  ★ 比较的必须是"新位置"与"上一个位置" ★
    //    所以要先算 moved，再把 m_cursorImagePos 更新成新值。
    //    顺序写反的话比较的就是同一个变量，条件恒为 false，
    //    表现为"鼠标移动时状态栏坐标不刷新"。
    static const QPointF kEpsilon(0.01, 0.01);
    const bool moved = !m_hasCursor
                       || qAbs(imgPos.x() - m_cursorImagePos.x()) > kEpsilon.x()
                       || qAbs(imgPos.y() - m_cursorImagePos.y()) > kEpsilon.y();

    m_cursorImagePos = imgPos;   // ← 记录新位置，供下一次比较
    m_hasCursor = true;

    if (moved)
        emit cursorMoved(imgPos);

    // ── 拖拽到边缘的自动滚动 ──
    static const int kEdgeMargin = 30;
    m_autoScrollDirection = QPointF(0, 0);

    if (event->buttons() != Qt::NoButton) {
        if (widgetPos.x() < kEdgeMargin)
            m_autoScrollDirection.setX(-1);
        else if (widgetPos.x() > width() - kEdgeMargin)
            m_autoScrollDirection.setX(1);

        if (widgetPos.y() < kEdgeMargin)
            m_autoScrollDirection.setY(-1);
        else if (widgetPos.y() > height() - kEdgeMargin)
            m_autoScrollDirection.setY(1);
    }

    if (!m_autoScrollDirection.isNull()) {
        if (!m_autoScrollTimer->isActive())
            m_autoScrollTimer->start();
    } else {
        m_autoScrollTimer->stop();
    }

      // ── 交给工具 ──
      if (m_activeTool) {
          const ToolEvent e = makeToolEvent(event, ToolEventType::Move);

          // ★ 区分"悬停"与"拖动" ★
          //
          //   没有按键按下 → hoverMove，工具据此更新**纯预览**状态
          //     （画笔的轮廓圆、吸管的取样框、形状工具的落点提示）。
          //   有按键按下   → mouseMove，才是真正在改数据。
          //
          //   为什么必须分开，而不是都走 mouseMove：
          //     画笔的 mouseMove 需要以 e.buttons != NoButton 为前提
          //     （否则鼠标划过画布就会留下痕迹）。于是"不按键时的轮廓
          //     跟随"就没有地方可以实现了 —— 除非在 mouseMove 里
          //     再分叉一次，那等于把两个语义塞进一个函数。
          //     用独立的 hoverMove 语义清晰，且各工具可以按需重写。
          const ToolResult result = (event->buttons() == Qt::NoButton)
                                        ? m_activeTool->hoverMove(e)
                                        : m_activeTool->mouseMove(e);
          handleToolResult(result, e);

          if (result.handled) {
              event->accept();
              // 注意：这里不能 return —— 状态栏的取样色还要更新。
              // 但工具已经消费了事件，所以后面的"默认行为"要跳过。
          }
      }

    // ── 悬停在参考线上 → 换成"可以拖"的光标 ──
    //
    // ★ 与"抓手工具两态光标"同一条纪律：状态在这里变了，就必须**当场补推光标** ★
    //   只改内部状态不推光标，用户看到的仍是箭头，"这里能拖"就完全传达不出去。
    //   只在形状真的不同时才 setCursor —— 每次都 set 会让光标被反复销毁重建。
    if (event->buttons() == Qt::NoButton) {
        const int gi = m_guidesVisible ? guideAtWidget(widgetPos, kGuideGrabPx) : -1;

        Qt::CursorShape want;
        if (gi >= 0) {
            // 水平线上下拖 → 垂直方向的调整光标；竖直线左右拖 → 水平方向。
            want = (m_guides.at(gi).orientation == Qt::Horizontal)
                       ? Qt::SizeVerCursor
                       : Qt::SizeHorCursor;
        } else {
            want = m_activeTool ? m_activeTool->cursor().shape() : Qt::ArrowCursor;
        }

        if (cursor().shape() != want)
            setCursor(want);
    }

    // ── 取样光标下的颜色（状态栏 RGB 显示）──
    sampleCursorColor(imgPos);

    event->accept();
}

void CanvasView::mouseReleaseEvent(QMouseEvent *event)
{
    // ── 结束平移 ──
    if (m_panning) {
        m_panning = false;
        m_middlePanning = false;
        m_autoScrollTimer->stop();
        refreshCursor();
        event->accept();
        return;
    }

    // ── 结束拖动参考线 ──
    //
    // ★ 松在画布之外 = 删掉这根线 ★（等价于"把它拖回标尺上"，
    //   与 Photoshop 的习惯一致）
    //   判定用 rect().contains()：Qt 在按下时做了**隐式鼠标抓取**，
    //   所以就算光标已经跑到标尺或窗口外，这里照样收得到松手事件。
    if (m_draggingGuide >= 0) {
        const int idx = m_draggingGuide;
        m_draggingGuide = -1;
        if (idx < m_guides.size()
            && !rect().contains(zl::eventPos(event).toPoint())) {
            removeGuide(idx);
        }
        event->accept();
        return;
    }

    if (m_autoScrollTimer->isActive())
        m_autoScrollTimer->stop();

    // ── 交给工具 ──
    if (m_activeTool && !m_spacePressed && event->button() != Qt::MiddleButton) {
        const ToolEvent e = makeToolEvent(event, ToolEventType::Release);
        const ToolResult result = m_activeTool->mouseRelease(e);
        handleToolResult(result, e);

        if (result.handled) {
            // 松手 → 工具回到空闲态，光标也要跟着回去（抓手：握紧 → 张开）
            refreshCursor();
            event->accept();
            return;
        }
    }

    event->ignore();
}

void CanvasView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_activeTool && !m_spacePressed && event->button() == Qt::LeftButton) {
        const ToolEvent e = makeToolEvent(event, ToolEventType::DoubleClick);
        const ToolResult result = m_activeTool->mouseDoubleClick(e);
        handleToolResult(result, e);

        if (result.handled) {
            event->accept();
            return;
        }
    }

    // 双击空白区域的默认行为：适合窗口。
    // 这是很多图形软件的通用手势，方便用户快速回到全局视图。
    if (event->button() == Qt::LeftButton && !m_activeTool) {
        zoomToFit();
        event->accept();
        return;
    }

    QWidget::mouseDoubleClickEvent(event);
}

// ============================================================================
//  输入：滚轮
// ============================================================================

void CanvasView::wheelEvent(QWheelEvent *event)
{
    // ★ Qt5 / Qt6 差异：滚轮事件必须用专门的 compat 函数 ★
    //
    //   QWheelEvent 不能用通用的 eventPos() 模板：
    //     · Qt6 把它并入了 QSinglePointEvent，位置接口叫 position()
    //     · Qt5 的 QWheelEvent 从来就没有 localPos()（那是 QMouseEvent 的），
    //       它用的是 pos()
    //   所以用 eventPos() 会报 "localPos 不是 QWheelEvent 的成员"。
    //   兼容层为此提供了 wheelEventPos()，版本分支只存在于那里。
    const QPointF widgetPos = zl::wheelEventPos(event);

    // ★ 修饰键的处理优先级 ★
    //   Ctrl + 滚轮 → 强制缩放（即使设置了"滚轮滚动"）
    //   这是几乎所有绘图软件的共同约定，用户肌肉记忆很强。
    const bool forceZoom = event->modifiers().testFlag(Qt::ControlModifier);

    if (m_wheelZooms || forceZoom) {
        // ── 缩放模式 ──
        //
        // ★ 累积 delta 而不是每个事件都缩放 ★
        //   高精度触控板一次滑动会发出几十个 delta=1~3 的事件。
        //   每个都缩放 kZoomStepFactor 倍的话，一次轻扫就会从 100% 跳到 3000%。
        //   累积到 120（一个传统滚轮格）才动一次，手感才正常。
        m_wheelAccumulator += event->angleDelta().y();

        const int step = 120;
        while (m_wheelAccumulator >= step) {
            m_wheelAccumulator -= step;
            setZoomAt(kZoomStepFactor, widgetPos);
        }
        while (m_wheelAccumulator <= -step) {
            m_wheelAccumulator += step;
            setZoomAt(1.0 / kZoomStepFactor, widgetPos);
        }

        event->accept();
        return;
    }

    // ── 滚动模式（垂直滚动，Shift 切换为水平）──
    const bool horizontal = event->modifiers().testFlag(Qt::ShiftModifier);
    const int delta = event->angleDelta().y() != 0
                          ? event->angleDelta().y()
                          : event->angleDelta().x();

    if (horizontal)
        panBy(QPointF(delta / 2.0, 0));
    else
        panBy(QPointF(0, delta / 2.0));

    event->accept();
}

// ============================================================================
//  输入：键盘
// ============================================================================

void CanvasView::keyPressEvent(QKeyEvent *event)
{
    // ★ 空格：临时平移 ★
    //   用 Press 而不是"检测自动重复" —— 长按空格会不断发 Press，
    //   但 m_spacePressed 已经为 true，重复设置无害。
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spacePressed = true;
        // 空格期间光标变成抓手，即使当前是画笔工具
        setCursor(Qt::OpenHandCursor);
        event->accept();
        return;
    }

    // ── Esc：取消当前工具操作 ──
    if (event->key() == Qt::Key_Escape) {
        cancelActiveTool();
        event->accept();
        return;
    }

    // ── 先给工具一次机会 ──
    //   移动工具的方向键精确移动必须优先于画布滚动。
    //   顺序很重要：如果先处理滚动，移动工具就永远收不到方向键。
    if (m_activeTool) {
        ToolEvent e;
        e.type = ToolEventType::KeyPress;
        e.key = event->key();
        e.text = event->text();
        e.modifiers = ToolModifiers::fromQt(event->modifiers());
        e.imagePos = m_cursorImagePos;
        e.lastImagePos = m_cursorImagePos;
        e.viewZoom = m_zoom;

        const ToolResult result = m_activeTool->keyPress(e);
        handleToolResult(result, e);

        if (result.handled) {
            event->accept();
            return;
        }
    }

    // ── 键盘滚动与缩放 ──
    //
    // 滚动的步长用"视口尺寸的 10%"，而不是固定像素 ——
    // 这样在 4K 全屏窗口与 400×300 的小窗口下手感一致。
    const qreal stepX = width()  * 0.10;
    const qreal stepY = height() * 0.10;

    switch (event->key()) {
    case Qt::Key_Left:
        panBy(QPointF(stepX, 0));
        event->accept();
        return;

    case Qt::Key_Right:
        panBy(QPointF(-stepX, 0));
        event->accept();
        return;

    case Qt::Key_Up:
        panBy(QPointF(0, stepY));
        event->accept();
        return;

    case Qt::Key_Down:
        panBy(QPointF(0, -stepY));
        event->accept();
        return;

    case Qt::Key_PageUp:
        // 翻页时留 10% 重叠，避免用户"翻过头找不到刚才看到的内容"
        panBy(QPointF(0, height() * 0.9));
        event->accept();
        return;

    case Qt::Key_PageDown:
        panBy(QPointF(0, -height() * 0.9));
        event->accept();
        return;

    case Qt::Key_Home:
        // Home：滚到左上角
        setOffset(QPointF(0, 0));
        event->accept();
        return;

    case Qt::Key_End:
        // End：滚到右下角
        {
            const QSizeF s(effectiveCanvasSize());
            setOffset(QPointF((width()  - s.width()  * m_zoom),
                              (height() - s.height() * m_zoom)));
        }
        event->accept();
        return;

    default:
        break;
    }

    QWidget::keyPressEvent(event);
}

void CanvasView::keyReleaseEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space && !event->isAutoRepeat()) {
        m_spacePressed = false;
        m_panning = false;
        refreshCursor();
        event->accept();
        return;
    }

    // 交给工具
    if (m_activeTool) {
        ToolEvent e;
        e.type = ToolEventType::KeyRelease;
        e.key = event->key();
        e.text = event->text();
        e.modifiers = ToolModifiers::fromQt(event->modifiers());
        e.imagePos = m_cursorImagePos;
        e.lastImagePos = m_cursorImagePos;
        e.viewZoom = m_zoom;
        handleToolResult(m_activeTool->keyRelease(e), e);
    }

    QWidget::keyReleaseEvent(event);
}

// ============================================================================
//  输入：窗口尺寸变化
// ============================================================================

void CanvasView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

    if (!m_doc)
        return;

    // ★ 尺寸变化时，若用户还没亲手动过视图，就继续保持"适合窗口" ★
    //
    //   见 m_userAdjustedView 的说明。这一条专门解决"文档在控件拿到真实
    //   尺寸之前就被设置好"的启动场景：show() 与随后的布局会各派发一次
    //   resize，每次都按新尺寸重新适合窗口，初启动的画面因此总是恰好铺满。
    //
    //   用户一旦缩放或平移过，就走下面的"保持中心不动"逻辑 ——
    //   不再擅自改动缩放，免得跟用户的意图打架。
    if (!m_userAdjustedView) {
        zoomToFit();
        return;
    }

    // ★ 缩放时保持"视口中心对应的图像点"不动 ★
    //
    //  简单做法是"什么都不做"，但那样窗口拉大时图像会偏向左上角。
    //  保持中心不动符合直觉：用户在窗口中心看的那个位置，拉大窗口后
    //  应该还在中心。
    //
    //  用旧尺寸算中心对应的图像点，再用新尺寸把这个点放回中心。
    const QSize oldSize = event->oldSize();
    const QSize newSize = event->size();

    if (oldSize.width() <= 0 || oldSize.height() <= 0)
        return;   // 首次显示，无需调整

    // 旧视口中心对应的图像点（用旧矩阵算）。
    // ★ 注意：此时 m_offset 已经代表旧布局，但控件尺寸已经变成新的了 ★
    //   所以不能直接调 widgetToImage —— 它用的是新尺寸下的矩阵。
    //   这里手工按旧尺寸推算：
    const QPointF oldCenterWidget(oldSize.width() / 2.0, oldSize.height() / 2.0);

    // m_viewTransform 里的平移是基于旧尺寸算出的 offset 的，
    // 所以 map(oldCenterWidget) 只在尺寸没变时才正确。
    // 由于 rebuildViewTransform 还没被调用，矩阵仍是旧的，
    // 用它来反解旧中心对应的图像点是正确的。
    bool invertible = false;
    const QTransform inv = m_viewTransform.inverted(&invertible);
    if (!invertible)
        return;

    const QPointF imgAtOldCenter = inv.map(oldCenterWidget);

    // 把该图像点放回新视口中心
    const QPointF newCenterWidget(newSize.width() / 2.0, newSize.height() / 2.0);
    const QPointF currentPos = m_viewTransform.map(imgAtOldCenter);
    m_offset += (newCenterWidget - currentPos);

    rebuildViewTransform();
    emit viewChanged();
}

void CanvasView::leaveEvent(QEvent *event)
{
    // 光标离开画布时清掉"有光标"标志，
    // 免得状态栏一直显示最后那个位置的坐标。
    m_hasCursor = false;
    m_autoScrollTimer->stop();
    QWidget::leaveEvent(event);
}

// ============================================================================
//  右键菜单
// ============================================================================
//
//  ★ 放哪些项 ★
//    只放"与视图/画布上下文相关"的操作。像"保存""滤镜"这类
//    全局操作应该待在菜单栏，塞进右键菜单只会让它变得冗长。
//
//  ★ 这些动作如何执行 ★
//    当前由 CanvasView 自己实现（都是视图操作）。
//    编辑类动作（复制/粘贴）通过 parent 的 QAction 触发 ——
//    这样 CanvasView 不需要知道 MainWindow 的存在。
// ============================================================================

void CanvasView::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);

    // ── 视图类 ──
    QAction *zoomInAct = menu.addAction(tr("放大"));
    zoomInAct->setShortcut(QKeySequence::ZoomIn);

    QAction *zoomOutAct = menu.addAction(tr("缩小"));
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);

    menu.addSeparator();

    QAction *fitAct = menu.addAction(tr("适合窗口"));
    fitAct->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_0));

    QAction *actualAct = menu.addAction(tr("实际大小 (100%)"));
    actualAct->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_1));

    menu.addSeparator();

    // ── 显示辅助 ──
    QAction *gridAct = menu.addAction(tr("显示网格"));
    gridAct->setCheckable(true);
    gridAct->setChecked(m_gridVisible);

    QAction *checkerAct = menu.addAction(tr("显示透明背景棋盘格"));
    checkerAct->setCheckable(true);
    checkerAct->setChecked(m_checkerboardVisible);

    menu.addSeparator();

    // ── 视图旋转 ──
    QMenu *rotateMenu = menu.addMenu(tr("视图旋转"));
    QAction *rot0   = rotateMenu->addAction(tr("不旋转"));
    QAction *rot90  = rotateMenu->addAction(tr("顺时针 90°"));
    QAction *rot180 = rotateMenu->addAction(tr("180°"));
    QAction *rot270 = rotateMenu->addAction(tr("逆时针 90°"));

    menu.addSeparator();

    // ── 编辑类动作 ──
    //
    // ★ 从父窗口的 QAction 里找 ★
    //   这样做的原因：复制/粘贴的实现在 MainWindow 里（需要访问
    //   剪贴板、文档管理器），CanvasView 不应该反向依赖 ui 层的
    //   MainWindow。通过 objectName 查找是一种松耦合的做法 ——
    //   找不到就禁用，不会崩溃。
    QAction *copyAct = nullptr;
    QAction *pasteAct = nullptr;
    QAction *deselectAct = nullptr;

    if (QWidget *p = parentWidget()) {
        if (QWidget *window = p->window()) {
            copyAct     = window->findChild<QAction *>(QStringLiteral("actionCopy"));
            pasteAct    = window->findChild<QAction *>(QStringLiteral("actionPaste"));
            deselectAct = window->findChild<QAction *>(QStringLiteral("actionDeselect"));
        }
    }

    if (copyAct) {
        menu.addAction(copyAct);
    }
    if (pasteAct) {
        menu.addAction(pasteAct);
    }
    if (deselectAct) {
        menu.addAction(deselectAct);
    }

    // ── 弹出并处理结果 ──
    QAction *chosen = menu.exec(event->globalPos());
    if (!chosen)
        return;

    if (chosen == zoomInAct) {
        zoomIn();
    } else if (chosen == zoomOutAct) {
        zoomOut();
    } else if (chosen == fitAct) {
        zoomToFit();
    } else if (chosen == actualAct) {
        zoomToActualSize();
    } else if (chosen == gridAct) {
        setGridVisible(gridAct->isChecked());
    } else if (chosen == checkerAct) {
        setCheckerboardVisible(checkerAct->isChecked());
    } else if (chosen == rot0) {
        setViewRotation(0);
    } else if (chosen == rot90) {
        setViewRotation(m_viewRotation + 90);
    } else if (chosen == rot180) {
        setViewRotation(m_viewRotation + 180);
    } else if (chosen == rot270) {
        setViewRotation(m_viewRotation - 90);
    }

    event->accept();
}

// ============================================================================
//  工具结果处理
// ============================================================================

void CanvasView::handleToolResult(const ToolResult &result, const ToolEvent &event)
{
    Q_UNUSED(event)

    if (result.needsRepaint) {
        update();
    }

    // needsCommit 目前不需要画布做什么 ——
    // 命令的压栈已经在工具内部完成了。
    // 保留这个字段是为了将来可能需要的"延迟提交"优化。
    if (result.needsCommit) {
        // ★ 提交后合成缓存一定失效 ★
        //   工具在拖动过程中直接改了图层位图（预览），但可能没有
        //   每次都调用 markDirty。这里保守地强制失效一次，
        //   保证松手后的画面与数据完全一致。
        m_compositeDirty = true;
    }
}

// ============================================================================
//  取样光标颜色
// ============================================================================

void CanvasView::sampleCursorColor(const QPointF &imagePos)
{
    if (!m_doc)
        return;

    // 只在光标位于图像范围内时才取样。
    // 在图像外取样会返回黑色或无效值，状态栏显示"0,0,0"会让用户困惑。
    const QRect canvasRect = m_doc->canvasRect();
    if (!canvasRect.contains(imagePos.toPoint())) {
        return;
    }

    // ★ 用合成结果取样，而不是活动图层 ★
    //   用户看到的是合成后的画面，状态栏显示的应该是"看到的那个颜色"。
    //   用活动图层取样的话，在有上层遮挡时会显示错误的颜色。
    const QImage &comp = m_doc->composite();
    if (comp.isNull())
        return;

    const QPoint pt = imagePos.toPoint();
    if (!comp.rect().contains(pt))
        return;

    const QColor c = comp.pixelColor(pt);
    emit cursorColorSampled(c, imagePos);
}

// ============================================================================
//  自动滚动
// ============================================================================

void CanvasView::onAutoScrollTimeout()
{
    if (m_autoScrollDirection.isNull()) {
        m_autoScrollTimer->stop();
        return;
    }

    // 每次滚动 8 个像素。
    //   太快会失控（用户来不及停），太慢会感觉"拖到边缘了怎么不动"。
    //   8 像素 @ 60ms ≈ 133 像素/秒，是一个比较舒适的速度。
    const qreal speed = 8.0;

    // ★ 方向要取反 ★
    //   m_autoScrollDirection 表示"鼠标在哪个边缘"（例如 x=1 表示右边缘）。
    //   鼠标在右边缘时，我们希望看到**右边**的内容，也就是图像要向左移 ——
    //   即 offset 减小。所以这里乘 -1。
    panBy(QPointF(-m_autoScrollDirection.x() * speed,
                  -m_autoScrollDirection.y() * speed));
}

} // namespace zl
