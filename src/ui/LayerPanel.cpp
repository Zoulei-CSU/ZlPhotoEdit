#include "ui/LayerPanel.h"

#include "core/Document.h"
#include "core/Layer.h"
#include "core/LayerList.h"
#include "ui/IconLibrary.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QDropEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace zl {

// ============================================================================
//  行数据的角色
// ============================================================================
//
//  放在匿名命名空间而不是头文件：这些角色只在"写行"和"读行"之间传递，
//  外部（MainWindow）不需要知道它们。公开它们只会让别处也去直接读行数据，
//  从而绕开"文档才是真相"这条约定。
namespace {

enum RowRole {
    LayerIndexRole = Qt::UserRole + 1,  // 该行对应哪个图层（文档索引）
    VisibleRole,                        // 该图层当前是否可见
    LockedRole,                         // 该图层当前是否锁定
    SubtitleRole,                       // 副标题（混合模式 · 不透明度），默认值下为空
    ThumbnailRole                       // 缩略图（QImage）
};

/// 图层副标题：只在"非默认"时才显示。
/// 每一行都写"正常 · 100%"会变成噪音，反而看不出哪个图层被改过。
QString subtitleFor(const Layer &layer)
{
    const int pct = qRound(layer.opacity() * 100.0 / 255.0);
    const bool defaultBlend  = (layer.blendMode() == BlendMode::Normal);
    const bool defaultOpac   = (pct >= 100);

    if (defaultBlend && defaultOpac)
        return QString();

    return QStringLiteral("%1 · %2%").arg(blendModeName(layer.blendMode()))
                                     .arg(pct);
}

/// 棋盘格贴图（缩略图的透明底）。
/// 不做这一步的话，透明图层在浅色面板上就是一片白，和"白色图层"分不出来。
const QPixmap &checkerTile()
{
    static QPixmap tile;
    if (tile.isNull()) {
        const int cell = 6;
        tile = QPixmap(cell * 2, cell * 2);
        tile.fill(QColor(255, 255, 255));
        QPainter p(&tile);
        p.fillRect(0, 0, cell, cell, QColor(214, 214, 214));
        p.fillRect(cell, cell, cell, cell, QColor(214, 214, 214));
        p.end();
    }
    return tile;
}

} // namespace

// ============================================================================
//  纯函数
// ============================================================================

namespace layerpanel {

int rowToLayerIndex(int row, int layerCount)
{
    if (layerCount <= 0 || row < 0 || row >= layerCount)
        return -1;
    return layerCount - 1 - row;
}

int layerIndexToRow(int layerIndex, int layerCount)
{
    if (layerCount <= 0 || layerIndex < 0 || layerIndex >= layerCount)
        return -1;
    return layerCount - 1 - layerIndex;
}

QList<int> rowsToLayerIndices(const QList<int> &rows, int layerCount)
{
    QList<int> out;
    out.reserve(rows.size());
    for (int row : rows) {
        const int index = rowToLayerIndex(row, layerCount);
        if (index >= 0)
            out.append(index);
    }
    return out;
}

QRect thumbnailRect(const QRect &rowRect)
{
    return QRect(rowRect.left() + kThumbLeft,
                 rowRect.top() + (rowRect.height() - kThumbSize) / 2,
                 kThumbSize, kThumbSize);
}

QRect eyeRect(const QRect &rowRect)
{
    const int top = rowRect.top() + (rowRect.height() - kHitWidth) / 2;
    return QRect(rowRect.right() - kThumbLeft - kHitWidth + 1,
                 top, kHitWidth, kHitWidth);
}

QRect lockRect(const QRect &rowRect)
{
    const QRect eye = eyeRect(rowRect);
    return QRect(eye.left() - kHitWidth - 2, eye.top(), kHitWidth, kHitWidth);
}

QRect nameRect(const QRect &rowRect)
{
    const QRect thumb = thumbnailRect(rowRect);
    const QRect lock  = lockRect(rowRect);
    const int left  = thumb.right() + 7;
    const int right = lock.left() - 6;
    return QRect(left, rowRect.top(), qMax(0, right - left + 1), rowRect.height());
}

Hotspot hotspotAt(const QRect &rowRect, const QPoint &pos)
{
    // ★ 顺序即优先级 ★
    //   眼睛与锁的矩形跟"名字区"在数学上是互斥的（不会有重叠像素），
    //   但由于名字区是"兜底分支"，先判小目标才符合直觉 ——
    //   将来若为了手感把点击区放大到与名字区重叠，这个顺序仍然是对的。
    if (!rowRect.contains(pos))
        return Hotspot::Outside;
    if (eyeRect(rowRect).contains(pos))
        return Hotspot::Visible;
    if (lockRect(rowRect).contains(pos))
        return Hotspot::Lock;
    return Hotspot::Thumbnail;
}

int dropRowToTargetIndex(int srcIndex, int dropBeforeRow, int layerCount)
{
    if (layerCount <= 0 || srcIndex < 0 || srcIndex >= layerCount)
        return -1;

    const int srcRow = layerIndexToRow(srcIndex, layerCount);
    if (srcRow < 0)
        return -1;

    int insertRow = qBound(0, dropBeforeRow, layerCount);

    // ① 源项先被"取出"，它后面的东西整体前移一位。
    //    如果插入点在源项之下，插槽也就跟着前移一位。
    if (srcRow < insertRow)
        --insertRow;

    insertRow = qBound(0, insertRow, layerCount - 1);

    // ② 换回文档索引
    return rowToLayerIndex(insertRow, layerCount);
}

} // namespace layerpanel

using namespace layerpanel;

// ============================================================================
//  LayerItemDelegate
// ============================================================================

LayerItemDelegate::LayerItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize LayerItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    Q_UNUSED(option)
    Q_UNUSED(index)
    // 高度写死：热点的矩形是按 kRowHeight 算出来的，
    // 行高一旦可变，hotspotAt 的假设就不成立了。
    return QSize(180, kRowHeight);
}

void LayerItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    // 文字自己画（要两行、要中段省略、要按可见性变淡），
    // 所以先清空再让样式只负责背景与选中高亮 —— 这样主题换了也不用改这里。
    opt.text.clear();

    const QWidget *widget = opt.widget;
    QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const QRect rowRect(option.rect);
    const bool selected = (opt.state & QStyle::State_Selected) != 0;
    const QColor textColor = selected
            ? opt.palette.color(QPalette::HighlightedText)
            : opt.palette.color(QPalette::Text);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    // ── ① 缩略图：棋盘格底 + 图 ──
    const QRect thumbRect = thumbnailRect(rowRect);
    painter->fillRect(thumbRect, QBrush(checkerTile()));

    const QImage thumb = index.data(ThumbnailRole).value<QImage>();
    if (!thumb.isNull()) {
        painter->drawImage(thumbRect, thumb);
    }

    painter->setPen(QPen(QColor(0, 0, 0, 48), 1));
    painter->setBrush(Qt::NoBrush);
    painter->drawRect(thumbRect.adjusted(0, 0, -1, -1));

    // ── ② 名字 + 副标题 ──
    const QRect nameR = nameRect(rowRect);
    const QFontMetrics fm(opt.font);

    const QString name = index.data(Qt::DisplayRole).toString();
    const QString subtitle = index.data(SubtitleRole).toString();
    const bool visible = index.data(VisibleRole).toBool();

    // 不可见图层整行变淡 —— 保留旧版最小列表的这条视觉约定，
    // 用户已经习惯了"灰掉的就是隐藏的"。
    QColor nameColor = textColor;
    if (!visible)
        nameColor.setAlpha(120);

    if (subtitle.isEmpty()) {
        painter->setPen(nameColor);
        painter->drawText(nameR, Qt::AlignLeft | Qt::AlignVCenter,
                          fm.elidedText(name, Qt::ElideMiddle, nameR.width()));
    } else {
        const int nameH = qMin(20, nameR.height() / 2 + 2);
        painter->setPen(nameColor);
        painter->drawText(QRect(nameR.left(), nameR.top() + 2,
                                nameR.width(), nameH),
                          Qt::AlignLeft | Qt::AlignBottom,
                          fm.elidedText(name, Qt::ElideMiddle, nameR.width()));

        QColor subColor = textColor;
        subColor.setAlpha(selected ? 190 : 135);
        painter->setPen(subColor);
        painter->drawText(QRect(nameR.left(), nameR.top() + nameH + 2,
                                nameR.width(), nameR.height() - nameH - 4),
                          Qt::AlignLeft | Qt::AlignTop,
                          fm.elidedText(subtitle, Qt::ElideRight, nameR.width()));
    }

    // ── ③ 眼睛 / 锁 ──
    const qreal dpr = widget ? widget->devicePixelRatioF() : 1.0;

    const auto drawRowIcon = [&](const QString &iconName, const QRect &target,
                                 qreal alpha) {
        const int px = qMax(1, qRound(target.width() * dpr));
        QPixmap pm = icons::pixmap(iconName, px);
        if (pm.isNull())
            return;                     // 图标缺失：静默跳过，单元格仍然可用
        pm.setDevicePixelRatio(dpr);
        painter->setOpacity(alpha);
        painter->drawPixmap(QPointF(target.left(), target.top()), pm);
        painter->setOpacity(1.0);
    };

    const bool locked = index.data(LockedRole).toBool();
    drawRowIcon(visible ? QStringLiteral("layer_visible")
                        : QStringLiteral("layer_hidden"),
                eyeRect(rowRect),
                visible ? 0.92 : 0.45);

    // 未锁定时不画"开锁"图，而是把它画得很淡 ——
    // 完全隐藏的话用户找不到这个功能，画满则每行都很吵。
    drawRowIcon(locked ? QStringLiteral("layer_lock")
                       : QStringLiteral("layer_unlock"),
                lockRect(rowRect),
                locked ? 0.92 : 0.20);

    painter->restore();
}

void LayerItemDelegate::updateEditorGeometry(QWidget *editor,
                                             const QStyleOptionViewItem &option,
                                             const QModelIndex &index) const
{
    Q_UNUSED(index)
    // 编辑器只盖住名字那一块，缩略图与两个小图标保持可见 ——
    // 否则"正在改哪一行的名字"会看不出来。
    editor->setGeometry(nameRect(QRect(option.rect)));
}

void LayerItemDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                     const QModelIndex &index) const
{
    Q_UNUSED(model)
    auto *line = qobject_cast<QLineEdit *>(editor);
    if (!line)
        return;

    const QString name = line->text().trimmed();
    if (name.isEmpty())
        return;                 // 空名字直接丢弃：行上看不出是什么，比不改还糟

    // ★ 刻意不写回 model ★
    //   行的显示内容来自 Document，写回 model 就有两份真相：
    //   撤销一次改名，行上还是新名字（因为没人再擦一次 model）。
    //   这里只把意图发出去，等 Document 变 → structureChanged → 重建行。
    //
    //   emit 需要非 const 的 this，而 setModelData 是 const 的虚函数。
    //   信号本身不修改任何状态，这个 const_cast 是安全的。
    emit const_cast<LayerItemDelegate *>(this)->commitRename(index.row(), name);
}

// ============================================================================
//  LayerListWidget
// ============================================================================

LayerListWidget::LayerListWidget(QWidget *parent)
    : QListWidget(parent)
{
    setItemDelegate(new LayerItemDelegate(this));

    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setUniformItemSizes(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 内部拖动排序
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::InternalMove);
    setDefaultDropAction(Qt::MoveAction);

    // 双击 / F2 进入重命名。
    // ★ 刻意不加 SelectedClicked ★
    //   加了之后"单击一个已选中的行"会变成开始编辑，
    //   而单击在这里最主要的用途是"开始拖动排序"，两者会打架。
    setEditTriggers(QAbstractItemView::DoubleClicked
                    | QAbstractItemView::EditKeyPressed);
}

void LayerListWidget::beginRename(int row)
{
    QListWidgetItem *it = item(row);
    if (!it)
        return;
    setCurrentItem(it);
    editItem(it);
}

void LayerListWidget::mousePressEvent(QMouseEvent *event)
{
    const QModelIndex idx = indexAt(event->pos());

    if (idx.isValid() && event->button() == Qt::LeftButton) {
        m_pressRow = idx.row();

        const QRect rowRect(0, visualRect(idx).top(),
                            viewport()->width(), kRowHeight);
        const Hotspot spot = hotspotAt(rowRect, event->pos());
        const int layerIndex = idx.data(LayerIndexRole).toInt();

        if (spot == Hotspot::Visible) {
            // ★ 点眼睛不改变选中态 ★
            //   与 Photoshop 一致：用户可以连续关掉好几个图层的可见性，
            //   而不用担心每点一次活动图层就跑掉。
            emit visibilityToggled(layerIndex, !idx.data(VisibleRole).toBool());
            return;
        }
        if (spot == Hotspot::Lock) {
            emit lockToggled(layerIndex, !idx.data(LockedRole).toBool());
            return;
        }
    }

    QListWidget::mousePressEvent(event);
}

void LayerListWidget::dropEvent(QDropEvent *event)
{
    // ★ 不调用基类的实现 ★
    //
    //  基类的 InternalMove 会**就地搬动行**，而行的真身在 Document 里。
    //  先让它搬完再同步文档，就等于有了两份真相 —— 撤销一次会看到行序
    //  弹回旧位置（因为 Document 的撤销把图层换回去了，而视图还停在
    //  "自己搬完"的顺序上，直到下一次重建）。
    //  正确做法：只把"想怎么动"翻译成一次 Document::moveLayer，
    //  行序交给 Document::structureChanged → rebuildRows() 重建。
    const QModelIndex dropIndex = indexAt(event->pos());
    const int dropRow = dropIndex.isValid() ? dropIndex.row() : count();

    int dropBeforeRow = dropRow;
    switch (dropIndicatorPosition()) {
    case QAbstractItemView::BelowItem:
        dropBeforeRow = dropRow + 1;
        break;
    case QAbstractItemView::AboveItem:
    case QAbstractItemView::OnItem:
        dropBeforeRow = dropRow;
        break;
    case QAbstractItemView::OnViewport:
        dropBeforeRow = count();     // 落在列表空白处 → 拽到最底
        break;
    }

    const QListWidgetItem *srcItem = (m_pressRow >= 0) ? item(m_pressRow) : nullptr;
    if (srcItem) {
        const int srcIndex = srcItem->data(LayerIndexRole).toInt();
        const int toIndex = dropRowToTargetIndex(srcIndex, dropBeforeRow, count());
        if (toIndex >= 0 && toIndex != srcIndex)
            emit reorderRequested(srcIndex, toIndex);
    }

    // 吃掉这次拖放：真正的移动已经发出去了，视图这一侧不许自己动。
    event->setDropAction(Qt::IgnoreAction);
    event->accept();
}

// ============================================================================
//  LayerPanel —— 构造与装配
// ============================================================================

LayerPanel::LayerPanel(int thumbnailThrottleMs, QWidget *parent)
    : QWidget(parent)
{
    setupUi();

    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setSingleShot(true);
    m_thumbTimer->setInterval(qMax(0, thumbnailThrottleMs));
    connect(m_thumbTimer, &QTimer::timeout, this, &LayerPanel::refreshThumbnailsNow);
}

int LayerPanel::throttleIntervalMs() const
{
    return m_thumbTimer ? m_thumbTimer->interval() : -1;
}

void LayerPanel::setThrottleIntervalMs(int ms)
{
    if (!m_thumbTimer)
        return;
    // QTimer 在运行中改 interval 不会影响本次已排定的触发，先停再设更可预期。
    m_thumbTimer->stop();
    m_thumbTimer->setInterval(qMax(0, ms));
}

bool LayerPanel::throttlePending() const
{
    return m_thumbTimer && m_thumbTimer->isActive();
}

void LayerPanel::setupUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(5);

    // ─────────────────────────────────────────────────────────────────
    //  属性区：混合模式 + 不透明度（作用于**活动图层**）
    // ─────────────────────────────────────────────────────────────────
    //
    //  为什么放在列表**上方**而不是下方：
    //    图层面板的列表会随图层数增长，放下面的话属性控件会被挤到看不见的
    //    地方（要滚动到底）。属性是高频操作，必须在固定位置。
    m_syncing = true;   // 填充下拉框会触发 currentIndexChanged，先抑制

    const auto makeLabel = [this](const QString &iconName, const QString &tip) {
        auto *label = new QLabel(this);
        const QPixmap pm = icons::pixmap(iconName, 16);
        if (!pm.isNull()) {
            label->setPixmap(pm);
            label->setToolTip(tip);
            label->setFixedWidth(18);
        } else {
            label->setText(tip.left(2));
        }
        return label;
    };

    auto *blendRow = new QHBoxLayout;
    blendRow->setSpacing(5);
    blendRow->addWidget(makeLabel(QStringLiteral("layer_blend"), tr("混合模式")));

    m_blendCombo = new QComboBox(this);
    m_blendCombo->setObjectName(QStringLiteral("layerBlendCombo"));
    m_blendCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    const QList<BlendMode> modes = allBlendModes();
    for (BlendMode m : modes)
        m_blendCombo->addItem(blendModeName(m), int(m));
    blendRow->addWidget(m_blendCombo, 1);
    root->addLayout(blendRow);

    auto *opacityRow = new QHBoxLayout;
    opacityRow->setSpacing(5);
    opacityRow->addWidget(makeLabel(QStringLiteral("layer_opacity"), tr("不透明度")));

    m_opacitySlider = new QSlider(Qt::Horizontal, this);
    m_opacitySlider->setObjectName(QStringLiteral("layerOpacitySlider"));
    m_opacitySlider->setRange(0, 100);
    m_opacitySlider->setPageStep(10);
    opacityRow->addWidget(m_opacitySlider, 1);

    m_opacityLabel = new QLabel(QStringLiteral("100%"), this);
    m_opacityLabel->setObjectName(QStringLiteral("layerOpacityLabel"));
    m_opacityLabel->setMinimumWidth(36);
    m_opacityLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    opacityRow->addWidget(m_opacityLabel);
    root->addLayout(opacityRow);

    m_syncing = false;

    // ─────────────────────────────────────────────────────────────────
    //  列表
    // ─────────────────────────────────────────────────────────────────
    m_list = new LayerListWidget(this);
    m_list->setObjectName(QStringLiteral("layerList"));
    m_list->setAlternatingRowColors(true);
    root->addWidget(m_list, 1);

    // ─────────────────────────────────────────────────────────────────
    //  按钮行
    // ─────────────────────────────────────────────────────────────────
    //
    //  ★ 用 lambda 包装而不是 SIGNAL/SLOT 宏 ★
    //    QToolButton::clicked 的签名是 void clicked(bool = false)，
    //    而这里调的槽都是无参的。写成字符串式连接能编译，但**运行时
    //    连接失败且没有任何提示** —— 按钮点了完全没反应。
    //    项目里的既有约定：一律新式连接。
    auto *btnRow = new QHBoxLayout;
    btnRow->setSpacing(2);

    const auto addButton = [&](const QString &iconName, const QString &tip,
                               const std::function<void()> &handler) {
        auto *btn = new QToolButton(this);
        btn->setAutoRaise(true);
        btn->setToolTip(tip);
        btn->setIconSize(QSize(18, 18));
        btn->setFixedSize(26, 26);
        btn->setIcon(icons::icon(iconName));
        if (btn->icon().isNull())
            btn->setText(tip.left(1));       // 图标缺失时用首字兜底，不留白按钮
        connect(btn, &QToolButton::clicked, this, [handler](bool) { handler(); });
        btnRow->addWidget(btn);
        m_buttons.append(btn);
        return btn;
    };

    addButton(QStringLiteral("layer_new"), tr("新建图层 (Ctrl+Shift+N)"),
              [this]() { emit addLayerRequested(); });

    addButton(QStringLiteral("layer_duplicate"), tr("复制图层 (Ctrl+J)"),
              [this]() {
        // 有选中就用选中的（支持多选批量复制）；没有就退回活动图层。
        QList<int> sel = selectedLayerIndices();
        if (sel.isEmpty() && m_doc)
            sel.append(m_doc->activeLayerIndex());
        emit duplicateRequested(sel);
    });

    addButton(QStringLiteral("layer_delete"), tr("删除选中的图层"),
              [this]() {
        QList<int> sel = selectedLayerIndices();
        if (sel.isEmpty() && m_doc)
            sel.append(m_doc->activeLayerIndex());
        emit deleteRequested(sel);
    });

    addButton(QStringLiteral("layer_up"), tr("上移图层 (Ctrl+])"),
              [this]() { emit moveUpRequested(); });

    addButton(QStringLiteral("layer_down"), tr("下移图层 (Ctrl+[)"),
              [this]() { emit moveDownRequested(); });

    addButton(QStringLiteral("layer_merge_down"), tr("向下合并 (Ctrl+E)"),
              [this]() {
        if (m_doc)
            emit mergeDownRequested(m_doc->activeLayerIndex());
    });

    addButton(QStringLiteral("layer_merge_visible"), tr("合并可见图层"),
              [this]() { emit mergeVisibleRequested(); });

    addButton(QStringLiteral("layer_flatten"), tr("拼合图像"),
              [this]() { emit flattenRequested(); });

    btnRow->addStretch(1);
    root->addLayout(btnRow);

    m_countLabel = new QLabel(this);
    m_countLabel->setObjectName(QStringLiteral("layerCountLabel"));
    m_countLabel->setAlignment(Qt::AlignRight);
    m_countLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    root->addWidget(m_countLabel);

    // ─────────────────────────────────────────────────────────────────
    //  信号接线
    // ─────────────────────────────────────────────────────────────────
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
        if (m_syncing || !current || !m_doc)
            return;
        const int index = current->data(LayerIndexRole).toInt();
        // 活动图层是**纯 UI 状态**（不进撤销栈），面板可以直接写。
        m_doc->setActiveLayerIndex(index);
    });

    connect(m_list, &QListWidget::itemSelectionChanged, this,
            [this]() { updateCountLabel(); });

    connect(m_list, &LayerListWidget::visibilityToggled,
            this, &LayerPanel::visibilityToggled);
    connect(m_list, &LayerListWidget::lockToggled,
            this, &LayerPanel::lockToggled);
    connect(m_list, &LayerListWidget::reorderRequested,
            this, &LayerPanel::reorderRequested);

    // 重命名：delegate 提交 → 发出去（文档改完会重建行）
    if (auto *delegate = qobject_cast<LayerItemDelegate *>(m_list->itemDelegate())) {
        connect(delegate, &LayerItemDelegate::commitRename, this,
                [this](int row, const QString &name) {
            if (!m_doc)
                return;
            const int index = layerpanel::rowToLayerIndex(row, m_doc->layerCount());
            if (index >= 0)
                emit renameRequested(index, name);
        });
    }

    connect(m_blendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i) {
        if (m_syncing || !m_doc || !m_doc->activeLayer())
            return;
        const int before = int(m_doc->activeLayer()->blendMode());
        const int after  = m_blendCombo->itemData(i).toInt();
        if (before == after)
            return;
        emit blendModeChanged(m_doc->activeLayerIndex(), before, after);
    });

    connect(m_opacitySlider, &QSlider::sliderPressed, this, [this]() {
        m_opacityAtPress = (m_doc && m_doc->activeLayer())
                ? int(m_doc->activeLayer()->opacity()) : -1;
    });

    connect(m_opacitySlider, &QSlider::valueChanged, this, [this](int pct) {
        m_opacityLabel->setText(QStringLiteral("%1%").arg(pct));

        if (m_syncing || !m_doc || !m_doc->activeLayer())
            return;

        const int index = m_doc->activeLayerIndex();
        const int after = qBound(0, qRound(pct * 255.0 / 100.0), 255);

        if (m_opacitySlider->isSliderDown()) {
            // 拖动中：只改内存，不压栈。
            // 不这么做的话拖一次滑块会在撤销栈里塞几十条命令，
            // 用户要按几十次 Ctrl+Z 才能退回去。
            emit opacityPreviewed(index, after);
        } else {
            // 键盘方向键 / 滚轮 / 点轨道：一步就是一次完整改动。
            const int before = int(m_doc->activeLayer()->opacity());
            if (before != after)
                emit opacityCommitted(index, before, after);
        }
    });

    connect(m_opacitySlider, &QSlider::sliderReleased, this, [this]() {
        if (m_syncing || !m_doc || !m_doc->activeLayer()) {
            m_opacityAtPress = -1;
            return;
        }
        const int index = m_doc->activeLayerIndex();
        const int after = int(m_doc->activeLayer()->opacity());
        // 拖动期间已经实时改过内存，这里只补一条**命令**让它可以撤销。
        // 命令的 redo 会把同一个值再写一次，幂等，无副作用。
        if (m_opacityAtPress >= 0 && m_opacityAtPress != after)
            emit opacityCommitted(index, m_opacityAtPress, after);
        m_opacityAtPress = -1;
    });

    updateCountLabel();
}

// ============================================================================
//  文档绑定
// ============================================================================

void LayerPanel::setDocument(Document *doc)
{
    if (m_doc == doc)
        return;

    // ★ 先断开旧文档 ★
    //   否则旧文档的 structureChanged 仍然连着本面板 ——
    //   切到第二个文档后，改第一个文档会把面板刷成它的内容。
    //   （连接都以 this 为上下文，所以按"发送者 + 上下文"断开是精准的。）
    if (m_doc)
        disconnect(m_doc, nullptr, this, nullptr);

    m_doc = doc;

    if (m_doc) {
        connect(m_doc, &Document::structureChanged,
                this, &LayerPanel::rebuildRows);

        // 像素变了 → 只重画缩略图（**节流**），不重建整个列表。
        // 重建会把选中态、滚动位置、正在进行的重命名统统清掉，
        // 而"画笔每画一笔"都会走到这里。
        connect(m_doc, &Document::contentChanged,
                this, [this]() { scheduleThumbnailRefresh(); });

        connect(m_doc, &Document::activeLayerChanged,
                this, [this](int) {
            syncSelectionToActiveLayer();
            syncControlsToActiveLayer();
        });
    }

    // ★ 显式补推一次初始状态 ★
    //   上面的 connect 只保证"之后的变化会同步"，而这个文档是在面板
    //   建好之后才设进来的 —— 此刻它的当前状态没有任何信号会发出来。
    //   少了这一步，面板会显示成"空列表 + 属性控件停在默认值"，
    //   而文档里明明有图层（顶部加载路径上就会这样）。
    rebuildRows();
    syncControlsToActiveLayer();
}

void LayerPanel::rebuildRows()
{
    if (!m_list)
        return;

    const bool wasSyncing = m_syncing;
    m_syncing = true;
    m_list->blockSignals(true);      // 重建不触发 currentItemChanged，不误切活动图层

    // 记住选中集：批量操作（删除 3 个图层 / 合并）之后要尽量保持多选状态，
    // 否则用户每次操作后都要重新框选一遍。
    const QList<int> keepSelection = selectedLayerIndices();

    m_list->clear();

    const int count = m_doc ? m_doc->layerCount() : 0;
    for (int row = 0; row < count; ++row) {
        const int index = rowToLayerIndex(row, count);
        const Layer *layer = m_doc ? m_doc->layers().layer(index) : nullptr;
        if (!layer)
            continue;

        auto *item = new QListWidgetItem;
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        item->setData(Qt::DisplayRole, layer->name());
        item->setData(Qt::UserRole, index);          // 与旧版最小列表保持兼容
        item->setData(LayerIndexRole, index);
        item->setData(VisibleRole, layer->isVisible());
        item->setData(LockedRole, layer->isLocked());
        item->setData(SubtitleRole, subtitleFor(*layer));
        item->setData(ThumbnailRole, layer->thumbnail(QSize(kThumbSize, kThumbSize)));
        ++m_thumbnailRenderCount;

        m_list->addItem(item);
    }

    // 恢复选中（保留仍然存在的那些）
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        const int index = item->data(LayerIndexRole).toInt();
        if (keepSelection.contains(index))
            item->setSelected(true);
    }

    m_list->blockSignals(false);

    syncSelectionToActiveLayer();    // 活动图层那一行始终是"当前行"
    syncControlsToActiveLayer();
    updateCountLabel();

    m_syncing = wasSyncing;
}

void LayerPanel::syncSelectionToActiveLayer()
{
    if (!m_list || !m_doc)
        return;

    const bool wasSyncing = m_syncing;
    m_syncing = true;
    m_list->blockSignals(true);

    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item->data(LayerIndexRole).toInt() == m_doc->activeLayerIndex()) {
            // ★ 只 setCurrentItem，不动选中集 ★
            //   多选状态下切活动图层不应该把别人的选中清掉。
            m_list->setCurrentItem(item, QItemSelectionModel::NoUpdate);
            break;
        }
    }

    m_list->blockSignals(false);
    m_syncing = wasSyncing;
}

void LayerPanel::syncControlsToActiveLayer()
{
    if (!m_blendCombo || !m_opacitySlider)
        return;

    const Layer *layer = m_doc ? m_doc->activeLayer() : nullptr;

    const bool wasSyncing = m_syncing;
    m_syncing = true;

    if (!layer) {
        m_blendCombo->setEnabled(false);
        m_opacitySlider->setEnabled(false);
        m_opacityLabel->setText(QStringLiteral("--"));
    } else {
        // ★ 锁定不禁用这两项 ★
        //   Layer.h 里明确：锁定只拦"改像素"的操作，改名称 / 不透明度 /
        //   混合模式仍然允许。禁用了反而是功能倒退。
        m_blendCombo->setEnabled(true);
        m_opacitySlider->setEnabled(true);

        const int modeIndex = m_blendCombo->findData(int(layer->blendMode()));
        if (modeIndex >= 0)
            m_blendCombo->setCurrentIndex(modeIndex);

        const int pct = qRound(layer->opacity() * 100.0 / 255.0);
        m_opacitySlider->setValue(pct);
        m_opacityLabel->setText(QStringLiteral("%1%").arg(pct));
    }

    m_syncing = wasSyncing;
}

void LayerPanel::updateRowDisplay(int layerIndex)
{
    if (!m_list || !m_doc)
        return;
    const Layer *layer = m_doc->layers().layer(layerIndex);
    if (!layer)
        return;

    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item->data(LayerIndexRole).toInt() != layerIndex)
            continue;

        const bool wasSyncing = m_syncing;
        m_syncing = true;
        item->setData(Qt::DisplayRole, layer->name());
        item->setData(VisibleRole, layer->isVisible());
        item->setData(LockedRole, layer->isLocked());
        item->setData(SubtitleRole, subtitleFor(*layer));
        m_syncing = wasSyncing;
        break;
    }

    m_list->viewport()->update();
}

void LayerPanel::updateCountLabel()
{
    if (!m_countLabel)
        return;

    const int count = m_list ? m_list->count() : 0;
    const int selected = m_list ? m_list->selectedItems().size() : 0;

    QString text = tr("共 %1 个图层").arg(count);
    if (selected > 1)
        text += tr(" · 选中 %1 个").arg(selected);
    m_countLabel->setText(text);
}

QList<int> LayerPanel::selectedLayerIndices() const
{
    if (!m_list || !m_doc)
        return QList<int>();

    QList<int> rows;
    const QList<QListWidgetItem *> items = m_list->selectedItems();
    rows.reserve(items.size());
    for (QListWidgetItem *item : items)
        rows.append(m_list->row(item));

    std::sort(rows.begin(), rows.end());

    QList<int> indices = rowsToLayerIndices(rows, m_doc->layerCount());
    std::sort(indices.begin(), indices.end());
    return indices;
}

void LayerPanel::beginRenameActiveLayer()
{
    if (!m_list || !m_doc)
        return;
    const int row = layerIndexToRow(m_doc->activeLayerIndex(), m_list->count());
    if (row >= 0)
        m_list->beginRename(row);
}

// ============================================================================
//  缩略图刷新（节流）
// ============================================================================

void LayerPanel::scheduleThumbnailRefresh()
{
    if (!m_thumbTimer || !m_list)
        return;

    // ★ 这里是"节流"，不是"防抖" ★
    //
    //  节流：计时器已经在跑就什么都不做，到点刷一次。
    //        连续画 20 笔 → 最多每 300ms 刷一次，画笔过程本身不受影响。
    //  防抖：每来一次就 restart()。画笔不停手，计时器就永远被推后，
    //        缩略图**在整个绘制过程中一次都不刷新** ——
    //        症状是"松手之后缩略图才动"，比不刷新更让人困惑。
    //
    //  本函数是节流。若哪天有人把它改成 m_thumbTimer->start()，
    //  tests/layer_panel_test.cpp 里的"连打 20 次 contentChanged 只刷一次"
    //  仍会通过（因为两者都只刷一次），但"拖动过程中会周期性刷新"
    //  这条会退化 —— 所以注释必须写清是哪一个。
    if (m_thumbTimer->isActive())
        return;
    m_thumbTimer->start();
}

void LayerPanel::refreshThumbnailsNow()
{
    if (!m_list || !m_doc)
        return;

    const bool wasSyncing = m_syncing;
    m_syncing = true;

    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        const int index = item->data(LayerIndexRole).toInt();
        const Layer *layer = m_doc->layers().layer(index);
        if (!layer)
            continue;
        item->setData(ThumbnailRole, layer->thumbnail(QSize(kThumbSize, kThumbSize)));
        ++m_thumbnailRenderCount;
    }

    m_list->viewport()->update();
    m_syncing = wasSyncing;
}

} // namespace zl
