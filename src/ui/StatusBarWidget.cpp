#include "ui/StatusBarWidget.h"

#include "core/Document.h"
#include "commands/CommandStack.h"

#include <QLabel>
#include <QComboBox>
#include <QTimer>
#include <QHBoxLayout>
#include <QPainter>
#include <QPixmap>
#include <QFontMetrics>
#include <QLineEdit>   // QComboBox 处于可编辑状态时用它才是输入控件，需要完整类型
#include <QGuiApplication>   // makeColorSwatch 取应用 palette 定边框色
#include <QPalette>

namespace zl {

namespace {

// 取样色块尺寸。
//  16×16 是"能看清颜色"与"不喧宾夺主"之间的折中 ——
//  再小就看不出颜色差异，再大就会把状态栏撑高（状态栏高度通常 22~26 像素）。
constexpr int kSwatchSize = 16;

// 把 QImage 缩成一个色块图标
QPixmap makeColorSwatch(const QColor &c, int size = kSwatchSize)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    // 不抗锯齿：色块是正方形，抗锯齿只会让边缘变糊
    // 边框取 palette(mid)：写死 110 的灰在深色状态栏上没有轮廓，
    // 色块会与底色连成一片。这里是自由函数，取应用级 palette。
    const QColor border = QGuiApplication::palette().color(QPalette::Mid);
    p.setPen(border.isValid() ? border : QColor(110, 110, 110));
    // 背景透明时用棋盘格表示 —— 否则用户看不出"这是透明而不是白色"
    if (!c.isValid() || c.alpha() == 0) {
        const int half = size / 2;
        p.fillRect(0, 0, half, half, QColor(255, 255, 255));
        p.fillRect(half, 0, size - half, half, QColor(200, 200, 200));
        p.fillRect(0, half, half, size - half, QColor(200, 200, 200));
        p.fillRect(half, half, size - half, size - half, QColor(255, 255, 255));
    } else {
        p.fillRect(0, 0, size, size, c);
    }
    p.drawRect(0, 0, size - 1, size - 1);
    p.end();

    return pm;
}

// 把字节数格式化成人类可读的字符串
QString formatBytes(qint64 bytes)
{
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 2);
    if (mb < 1.0)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

} // anonymous namespace

// ============================================================================
//  构造
// ============================================================================

StatusBarWidget::StatusBarWidget(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
}

StatusBarWidget::~StatusBarWidget() = default;

void StatusBarWidget::buildUi()
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(12);

    // ── 坐标 ──
    m_coordLabel = new QLabel(this);
    m_coordLabel->setMinimumWidth(100);
    m_coordLabel->setToolTip(tr("Cursor position within the image, in pixels"));
    m_coordLabel->setText(QStringLiteral("—, —"));
    layout->addWidget(m_coordLabel);

    // ── 取样色块 + 数值 ──
    //
    // ★ 色块用 QLabel 显示 QPixmap，而不是用样式表设 background-color ★
    //   样式表方案的 border 会与 QLabel 的默认边框叠加，
    //   导致高度不一致 —— 整个状态栏看起来高低不齐。
    //   用 QPixmap 则尺寸完全可控。
    m_colorSwatch = new QLabel(this);
    m_colorSwatch->setFixedSize(kSwatchSize, kSwatchSize);
    m_colorSwatch->setPixmap(makeColorSwatch(QColor()));
    layout->addWidget(m_colorSwatch);

    m_colorLabel = new QLabel(this);
    m_colorLabel->setMinimumWidth(100);
    m_colorLabel->setToolTip(tr("Color of the composited image under the cursor (R, G, B)"));
    m_colorLabel->setText(QStringLiteral("—"));
    layout->addWidget(m_colorLabel);

    // ── 文档尺寸 ──
    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setMinimumWidth(130);
    layout->addWidget(m_sizeLabel);

    // ── 内存 ──
    m_memoryLabel = new QLabel(this);
    m_memoryLabel->setMinimumWidth(180);
    m_memoryLabel->setToolTip(tr("Document pixel memory (undo-stack estimate in parentheses)"));
    layout->addWidget(m_memoryLabel);

    // ── 弹簧：把消息推到右侧 ──
    layout->addStretch(1);

    // ── 消息 ──
    m_messageLabel = new QLabel(this);
    m_messageLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(m_messageLabel);

    layout->addStretch(1);

    // ── 缩放 ──
    auto *zoomLabel = new QLabel(tr("Zoom"), this);
    layout->addWidget(zoomLabel);

    m_zoomCombo = new QComboBox(this);
    // ★ 可编辑 ★
    //   用户可以直接输入 133% 这样的任意比例 ——
    //   只给预设级别的话，"缩放到刚好装下某个细节"就做不到了。
    m_zoomCombo->setEditable(true);
    m_zoomCombo->setFixedWidth(92);

    // 预设级别。这是 Photoshop 的默认级别集合。
    const int levels[] = { 12, 25, 33, 50, 66, 100, 150, 200, 300, 400, 600, 800, 1600 };
    for (int level : levels)
        m_zoomCombo->addItem(QStringLiteral("%1%").arg(level), qreal(level));

    m_zoomCombo->setCurrentText(QStringLiteral("100%"));

    layout->addWidget(m_zoomCombo);

    connect(m_zoomCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &StatusBarWidget::onZoomComboChanged);

    // ★ 可编辑的 QComboBox 在用户输入回车时不会发 currentIndexChanged ★
    //   它发的是 lineEdit 的 returnPressed。不接这个信号的话，
    //   用户输入 133% 回车后什么都不会发生 —— 会以为输入没生效。
    if (m_zoomCombo->lineEdit()) {
        connect(m_zoomCombo->lineEdit(), &QLineEdit::returnPressed,
                this, &StatusBarWidget::onZoomComboChanged);
    }

    // ── 消息定时器 ──
    m_messageTimer = new QTimer(this);
    m_messageTimer->setSingleShot(true);
    connect(m_messageTimer, &QTimer::timeout, this, &StatusBarWidget::onMessageTimeout);

    refreshDocumentInfo();
}

// ============================================================================
//  文档绑定
// ============================================================================

void StatusBarWidget::setDocument(Document *doc)
{
    if (m_doc == doc)
        return;

    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);

    m_doc = doc;

    if (m_doc) {
        // 文档内容或尺寸变化时刷新"尺寸"与"内存"两栏。
        //
        // ★ 不做节流会不会太频繁 ★
        //   画笔拖动时 contentChanged 每帧都发，状态栏每帧重算一次
        //   memoryUsage() 与 estimatedMemoryUsage()。
        //   这两个函数都是 O(图层数) 的轻量计算（不是 O(像素)），
        //   实测开销在微秒级，因此不需要节流。
        //   但如果将来 memoryUsage 变重了，就要在这里加一个 200ms 的定时器。
        connect(m_doc, &Document::contentChanged,
                this, &StatusBarWidget::refreshDocumentInfo);
        connect(m_doc, &Document::canvasSizeChanged,
                this, &StatusBarWidget::refreshDocumentInfo);
        connect(m_doc, &Document::structureChanged,
                this, &StatusBarWidget::refreshDocumentInfo);
    }

    clearCursorInfo();
    refreshDocumentInfo();
}

void StatusBarWidget::setCommandStack(CommandStack *stack)
{
    m_stack = stack;
    refreshDocumentInfo();
}

// ============================================================================
//  光标信息
// ============================================================================

void StatusBarWidget::setCursorPos(const QPointF &imagePos)
{
    // 显示为整数坐标。
    // 图像编辑里亚像素坐标对用户没有意义 —— 他不能操作半个像素，
    // 而显示 "1234.67" 只会让数字变长、可读性下降。
    m_coordLabel->setText(QStringLiteral("%1, %2")
                              .arg(qRound(imagePos.x()))
                              .arg(qRound(imagePos.y())));
}

void StatusBarWidget::setSampledColor(const QColor &color, const QPointF &imagePos)
{
    setCursorPos(imagePos);
    m_colorSwatch->setPixmap(makeColorSwatch(color));

    // 带 alpha 时额外显示 alpha —— 否则用户看到"255, 128, 0"
    // 无法判断这是个不透明的橙色还是个半透明的橙。
    if (color.alpha() < 255) {
        m_colorLabel->setText(QStringLiteral("%1, %2, %3 (%4)")
                                  .arg(color.red())
                                  .arg(color.green())
                                  .arg(color.blue())
                                  .arg(color.alpha()));
    } else {
        m_colorLabel->setText(QStringLiteral("%1, %2, %3")
                                  .arg(color.red())
                                  .arg(color.green())
                                  .arg(color.blue()));
    }
}

void StatusBarWidget::clearCursorInfo()
{
    m_coordLabel->setText(QStringLiteral("—, —"));
    m_colorSwatch->setPixmap(makeColorSwatch(QColor()));
    m_colorLabel->setText(QStringLiteral("—"));
}

// ============================================================================
//  缩放
// ============================================================================

void StatusBarWidget::setZoom(qreal zoom)
{
    // ★ 屏蔽信号，避免程序化设置触发 onZoomComboChanged ★
    m_updatingZoomCombo = true;

    const qreal percent = zoom * 100.0;

    // 保留小数只在必要时 —— "150%" 比 "150.0%" 更清爽，
    // 而 "66.7%" 必须保留一位小数（取整会变成 67%，与实际不符）。
    QString text;
    if (qAbs(percent - qRound(percent)) < 0.05)
        text = QStringLiteral("%1%").arg(qRound(percent));
    else
        text = QStringLiteral("%1%").arg(percent, 0, 'f', 1);

    m_zoomCombo->setCurrentText(text);

    m_updatingZoomCombo = false;
}

void StatusBarWidget::onZoomComboChanged()
{
    if (m_updatingZoomCombo)
        return;

    // ── 解析用户输入 ──
    //
    // 允许的输入形式：
    //   "150%"  → 150
    //   "150"   → 150
    //   "1.5"   → 当作倍数 → 150
    //
    // ★ 为什么支持倍数形式 ★
    //   用户从别处看到"放大 2 倍"的表述后，直接输入 "2" 是很自然的。
    //   判据：小于 20 的数值视为倍数（因为 20% 以下的输入在实践中没有意义，
    //   图像会小到看不见）。
    QString text = m_zoomCombo->currentText().trimmed();
    text.remove(QLatin1Char('%'));
    text = text.trimmed();

    bool ok = false;
    qreal value = text.toDouble(&ok);

    if (!ok || value <= 0.0) {
        // 输入无效：什么都不做。
        // 不弹错误提示的原因 —— 用户正在编辑输入框（可能只是中途状态，
        // 例如刚删掉旧内容还没输入新内容），弹提示会打断他。
        // 下一次视图变化时 setZoom 会把显示恢复成正确值。
        return;
    }

    if (value < 20.0)
        value *= 100.0;

    // 夹到与 CanvasView 一致的范围
    value = qBound(1.0, value, 6400.0);

    emit zoomLevelRequested(value);
}

// ============================================================================
//  消息
// ============================================================================

void StatusBarWidget::showMessage(const QString &message, int timeoutMs)
{
    m_messageLabel->setText(message);

    if (timeoutMs > 0)
        m_messageTimer->start(timeoutMs);
    else
        m_messageTimer->stop();
}

void StatusBarWidget::onMessageTimeout()
{
    m_messageLabel->clear();
}

// ============================================================================
//  刷新文档信息
// ============================================================================

void StatusBarWidget::refreshDocumentInfo()
{
    if (!m_doc) {
        m_sizeLabel->setText(QString());
        m_memoryLabel->setText(QString());
        return;
    }

    // ── 尺寸 ──
    const QSize sz = m_doc->canvasSize();
    m_sizeLabel->setText(tr("Size: %1 × %2").arg(sz.width()).arg(sz.height()));

    // ── 内存 ──
    //
    // ★ 分开显示文档内存与撤销栈内存 ★
    //   合并成一个数字会丢失重要信息：用户看到"总计 350MB"时不知道该
    //   删图层还是清撤销历史。分开显示则一目了然 ——
    //   如果是撤销栈占了 300MB，清一次历史就省下来了。
    const qint64 docBytes   = m_doc->memoryUsage();
    const qint64 stackBytes = m_stack ? m_stack->estimatedMemoryUsage() : 0;

    m_memoryLabel->setText(tr("Memory: %1 (Undo %2)")
                               .arg(formatBytes(docBytes))
                               .arg(formatBytes(stackBytes)));
}

} // namespace zl
