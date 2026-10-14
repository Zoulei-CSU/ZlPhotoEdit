// ============================================================================
//  CanvasSizeDialog.cpp —— "画布大小"对话框的实现
// ============================================================================

#include "ui/CanvasSizeDialog.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QColorDialog>
#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QStringList>
#include <QStyle>
#include <QStyleOption>
#include <QToolButton>
#include <QVBoxLayout>

namespace zl {

namespace {

// ---------------------------------------------------------------------------
//  九宫格锚点按钮
// ---------------------------------------------------------------------------
//  ★ 图形靠画，不靠字体 ★
//    常见做法是在按钮上写 ↖ ↑ ↗ ← ⊙ → ↙ ↓ ↘。但那九个字形在不少
//    中文字体里缺失或宽度不一致，Windows 上会退化成方框 —— 而这是一个
//    "用户必须看懂才能操作"的控件。
//
//    这里改成画一个"画布外框 + 内容所在方位的实心块"，与控件的语义
//    完全对应，且与字体、语言、DPI 都无关。
// ---------------------------------------------------------------------------
class AnchorButton : public QToolButton
{
public:
    AnchorButton(Anchor anchor, QWidget *parent = nullptr)
        : QToolButton(parent)
        , m_anchor(anchor)
    {
        setCheckable(true);
        setFixedSize(30, 30);
        setFocusPolicy(Qt::NoFocus);   // 九宫格不该抢 Tab 焦点，否则键盘导航要按九次
        setToolTip(anchorName(anchor));
    }

    // ★ context 修复（2026-10-14，见 docs/界面语言.md §4.3）★
    //   AnchorButton 在匿名 namespace 里且无 Q_OBJECT：lupdate 记 context
    //   "zl::AnchorButton"，运行期 tr() 查 "QObject" —— 对不上。
    //   不能加 Q_OBJECT（moc 按 zl::AnchorButton 生成符号，编译直接报
    //   "zl": 不是类或命名空间名称）。改法：translate + 显式 context。
    //   ★ 字面量必须直接传给 translate，不能经 QT_TRANSLATE_NOOP 中转 ★
    //     实测 lupdate 5.15.16 不提取 static 成员函数里的 QT_TRANSLATE_NOOP
    //     （且会连带触发 "Qualifying with unknown namespace/class" 警告，
    //     把后面的 tr() 全记成 ::CanvasSizeDialog context）。
    static QString anchorName(Anchor a)
    {
        switch (a) {
        case Anchor::TopLeft:      return QCoreApplication::translate("zl::AnchorButton", "Top Left");
        case Anchor::TopCenter:    return QCoreApplication::translate("zl::AnchorButton", "Top");
        case Anchor::TopRight:     return QCoreApplication::translate("zl::AnchorButton", "Top Right");
        case Anchor::MiddleLeft:   return QCoreApplication::translate("zl::AnchorButton", "Middle Left");
        case Anchor::Center:       return QCoreApplication::translate("zl::AnchorButton", "Center");
        case Anchor::MiddleRight:  return QCoreApplication::translate("zl::AnchorButton", "Middle Right");
        case Anchor::BottomLeft:   return QCoreApplication::translate("zl::AnchorButton", "Bottom Left");
        case Anchor::BottomCenter: return QCoreApplication::translate("zl::AnchorButton", "Bottom");
        case Anchor::BottomRight:  return QCoreApplication::translate("zl::AnchorButton", "Bottom Right");
        }
        return QCoreApplication::translate("zl::AnchorButton", "Center");
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // ★ 自己画底板，不走 QToolButton::initStyleOption ★
        //   那个方法是 protected 且在 QT_NO_MENU 下会被条件编译掉，
        //   依赖它会让"能不能编过"取决于构建配置。这里用公开的
        //   QStyle::drawPrimitive 自行组装状态位 —— 效果一样，
        //   但依赖的接口都是稳定的公开 API。
        QPainter p(this);

        QStyleOption opt;
        opt.initFrom(this);
        opt.state = QStyle::State_None;
        if (isEnabled())
            opt.state |= QStyle::State_Enabled;
        if (isDown())
            opt.state |= QStyle::State_Sunken;
        if (isChecked())
            opt.state |= QStyle::State_On;
        if (underMouse())
            opt.state |= QStyle::State_MouseOver;

        style()->drawPrimitive(QStyle::PE_PanelButtonTool, &opt, &p, this);

        // 具体图形画在内容区（四周留 6px 内边距，与其它工具按钮观感一致）
        const QRect r = rect().adjusted(6, 6, -6, -6);
        if (r.width() < 6 || r.height() < 6)
            return;

        p.setRenderHint(QPainter::Antialiasing, false);

        // 外框 = 画布
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(palette().color(QPalette::Text), 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));

        // 实心块 = 内容所处的方位（九宫格的哪一格）
        const int col = int(m_anchor) % 3;
        const int row = int(m_anchor) / 3;

        // ★ 用 (i*w)/3 分配三格，而不是先整除再补余数 ★
        //   r.width() 未必能被 3 整除，用这种做法三格之和恰好等于
        //   r.width()，右侧不会留下一条空隙。
        const int x0 = (col * r.width()) / 3;
        const int x1 = ((col + 1) * r.width()) / 3;
        const int y0 = (row * r.height()) / 3;
        const int y1 = ((row + 1) * r.height()) / 3;

        const QRect inner(r.x() + x0, r.y() + y0,
                          qMax(1, x1 - x0), qMax(1, y1 - y0));

        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Highlight));
        p.drawRect(inner);
    }

private:
    Anchor m_anchor;
};

// 把一个矩形包成 QPainterPath。
//
// ★ 为什么需要这个小函数 ★
//   QPainterPath **没有**接受 QRectF 的构造函数（只有默认构造与
//   QPointF 起点构造），矩形必须用 addRect() 加进去。写成
//   `QPainterPath(rect)` 会得到 "无法从 QRectF 转换为 QPainterPath"。
//   下面的相交/相减运算要用它，包一层比到处写三行清晰。
QPainterPath rectPath(const QRectF &r)
{
    QPainterPath path;
    path.addRect(r);
    return path;
}

// 生成颜色小样。透明/半透明时先用棋盘格打底 ——
// 否则"透明"和"白色"在浅色界面上看起来一模一样。
QPixmap makeSwatch(const QColor &color, const QSize &size)
{
    QPixmap pm(size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    if (color.alpha() < 255) {
        p.fillRect(pm.rect(), Qt::white);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(200, 200, 200));
        const int g = 5;
        for (int y = 0; y < size.height(); y += g)
            for (int x = 0; x < size.width(); x += g)
                if (((x / g) + (y / g)) % 2)
                    p.drawRect(x, y, g, g);
    }

    p.fillRect(pm.rect(), color);

    p.setBrush(Qt::NoBrush);
    p.setPen(QColor(90, 90, 90));
    p.drawRect(0, 0, size.width() - 1, size.height() - 1);
    return pm;
}

} // namespace

// ---------------------------------------------------------------------------
//  示意图
// ---------------------------------------------------------------------------
//
//  画什么：
//    · 新画布      —— 实线粗框，内部用扩展色（或棋盘格）填充
//    · 原画布范围   —— 虚线框
//    · 将被移除的区域 —— 原画布在新画布之外的部分，淡红罩一层
//    · 内容        —— 蓝色实心块；落在新画布之外的**内容**用深红标出
//
//  最后一项是关键：用户需要一眼分清"透明边被裁掉"（无所谓）与
//  "我真的画了东西的地方被裁掉"（会心疼）。两者用不同颜色区分开，
//  比一句"内容将被裁掉 120 像素"有效得多。
// ---------------------------------------------------------------------------
class CanvasPreviewWidget : public QWidget
{
public:
    explicit CanvasPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(280, 168);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAutoFillBackground(false);
    }

    QSize sizeHint() const override { return QSize(300, 168); }

    void setState(const QSize &oldSize, const QSize &newSize,
                  const QRect &contentRect, const QPoint &offset,
                  const QColor &fill)
    {
        m_oldSize   = oldSize;
        m_newSize   = newSize;
        m_content   = contentRect;
        m_offset    = offset;
        m_fill      = fill;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const QRect newRect(QPoint(0, 0), m_newSize);
        const QRect oldInNew = QRect(QPoint(0, 0), m_oldSize).translated(m_offset);
        const QRect contentInNew = m_content.isEmpty()
                                 ? QRect()
                                 : m_content.translated(m_offset);

        // ── 归一化：把新旧画布都包进来，等比缩放到控件里并居中 ──
        QRect world = newRect;
        if (!oldInNew.isEmpty())
            world = world.united(oldInNew);
        if (!contentInNew.isEmpty())
            world = world.united(contentInNew);

        constexpr int kMargin = 10;
        const qreal availW = qMax(1, width()  - 2 * kMargin);
        const qreal availH = qMax(1, height() - 2 * kMargin);

        // 取较小的比例，保证两个方向都装得下（等比，不拉伸）
        const qreal s = qMin(availW / qMax(1, world.width()),
                             availH / qMax(1, world.height()));

        const qreal ox = (width()  - world.width()  * s) / 2.0 - world.left() * s;
        const qreal oy = (height() - world.height() * s) / 2.0 - world.top()  * s;

        const auto m = [&](const QRect &r) {
            return QRectF(ox + r.left() * s, oy + r.top() * s,
                          r.width() * s, r.height() * s);
        };

        const QRectF newR = m(newRect);

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // ── 1) 新画布底色 ──
        p.save();
        p.setClipRect(newR);
        if (m_fill.alpha() < 255) {
            // 透明底：棋盘格。让用户看清"这块是新加出来的、什么内容都没有"
            p.fillRect(newR, Qt::white);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(214, 214, 214));
            const QRect ir = newR.toAlignedRect();
            constexpr int g = 8;
            for (int y = ir.top(); y <= ir.bottom(); y += g)
                for (int x = ir.left(); x <= ir.right(); x += g)
                    if ((((x - ir.left()) / g) + ((y - ir.top()) / g)) % 2)
                        p.drawRect(x, y, g, g);
        } else {
            p.fillRect(newR, m_fill);
        }
        p.restore();

        // ── 2) 原画布中"将被移除"的部分 ──
        //
        //  只罩**原画布**在新画布之外的部分。新画布扩大出来的区域不算
        //  移除，那里本来就没有东西。
        const QRectF oldR = m(oldInNew);
        if (!oldInNew.isEmpty()) {
            const QPainterPath removed = rectPath(oldR) - rectPath(newR);
            if (!removed.isEmpty())
                p.fillPath(removed, QColor(200, 60, 60, 40));
        }

        // ── 3) 内容 ──
        if (!contentInNew.isEmpty()) {
            // ★ 必须 m() 映射成控件坐标 ★
            //   contentInNew 是**图像坐标**下的矩形（例如 (0,0,3840,2160)）。
            //   少了这一步映射，路径就按图像坐标画在"控件像素"空间里 ——
            //   3840×2160 的矩形直接盖住整个控件，于是"内容"铺满画布、
            //   "被裁内容"红块溢出到画布之外，而虚线（用了映射值）看起来
            //   又是对的。整个示意图都在骗人，而且不崩、不报错。
            const QPainterPath contentPath = rectPath(m(contentInNew));
            const QPainterPath canvasPath  = rectPath(newR);

            // 3a) 留在新画布里的部分：蓝
            const QPainterPath kept = contentPath.intersected(canvasPath);
            if (!kept.isEmpty()) {
                p.fillPath(kept, QColor(42, 110, 180, 120));
                p.setPen(QPen(QColor(28, 80, 140), 1.0));
                p.setBrush(Qt::NoBrush);
                p.drawPath(kept);
            }

            // 3b) 落到新画布之外的部分：深红（这才是真正被裁掉的内容）
            const QPainterPath cropped = contentPath.subtracted(canvasPath);
            if (!cropped.isEmpty()) {
                p.fillPath(cropped, QColor(200, 40, 40, 150));
                p.setPen(QPen(QColor(150, 20, 20), 1.0));
                p.setBrush(Qt::NoBrush);
                p.drawPath(cropped);
            }
        }

        // ── 4) 原画布范围（虚线） ──
        //
        //  ★ 必须画在内容色块**之后** ★
        //    内容包围盒常常就等于整幅旧画布（不透明照片）。若虚线先画、
        //    内容后画，蓝色实心块会把虚线整条盖住 —— 图例上写着
        //    "虚线 = 原画布范围"，用户却在图上找不到虚线。
        if (!oldInNew.isEmpty() && oldInNew != newRect) {
            QPen pen(QColor(90, 90, 90), 1.0, Qt::DashLine);
            pen.setDashPattern({ 4, 3 });
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRect(oldR.adjusted(0.5, 0.5, -0.5, -0.5));
        }

        // ── 5) 新画布边框（最后画，压在其它元素之上） ──
        p.setPen(QPen(palette().color(QPalette::Text), 2.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(newR.adjusted(1.0, 1.0, -1.0, -1.0));
    }

private:
    QSize  m_oldSize{1, 1};
    QSize  m_newSize{1, 1};
    QRect  m_content;
    QPoint m_offset;
    QColor m_fill = Qt::transparent;
};

// ---------------------------------------------------------------------------
//  对话框
// ---------------------------------------------------------------------------

CanvasSizeDialog::CanvasSizeDialog(const QSize &original,
                                   const QRect &contentBounds,
                                   Anchor initialAnchor,
                                   QWidget *parent)
    : SizeChangeDialogBase(original, tr("Canvas Size"),
                           /*allowRatioLock=*/false,   // 画布大小没有"保持长宽比"的概念：
                                                       // 拉伸画布不改变内容，比例无意义
                           /*allowRelative=*/true,
                           parent)
    , m_contentBounds(contentBounds)
{
    auto *extra = new QWidget(this);
    auto *v = new QVBoxLayout(extra);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    // ── 定位行：九宫格 + 扩展颜色 ──
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(new QLabel(tr("Anchor(&A):"), extra));

    auto *grid = new QGridLayout;
    grid->setSpacing(2);

    m_anchorGroup = new QButtonGroup(this);
    m_anchorGroup->setExclusive(true);

    // ★ 严格按 Anchor 枚举的顺序建按钮 ★
    //   下标 i 直接 static_cast 成 Anchor，再由 i/3、i%3 决定网格位置。
    //   这样"枚举顺序"与"九宫格布局"只有这一处对应关系，将来往枚举里
    //   插值会立刻在这里编译报错，而不是静默地错位。
    for (int i = 0; i < 9; ++i) {
        const Anchor a = static_cast<Anchor>(i);
        auto *btn = new AnchorButton(a, extra);
        btn->setObjectName(QStringLiteral("anchorButton%1").arg(i));
        m_anchorGroup->addButton(btn, i);
        grid->addWidget(btn, i / 3, i % 3);
    }

    if (QAbstractButton *b = m_anchorGroup->button(int(initialAnchor)))
        b->setChecked(true);

    row->addLayout(grid);
    row->addSpacing(18);

    row->addWidget(new QLabel(tr("Fill Color(&C):"), extra));

    m_colorButton = new QToolButton(extra);
    m_colorButton->setObjectName(QStringLiteral("buttonFillColor"));
    m_colorButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_colorButton->setIconSize(QSize(26, 16));
    m_colorButton->setToolTip(tr("Color used to fill the new area when the "
                                 "canvas grows. Transparent by default - stays "
                                 "transparent in PNG exports, becomes white in "
                                 "JPEG exports."));
    connect(m_colorButton, &QToolButton::clicked, this, &CanvasSizeDialog::onPickColor);
    row->addWidget(m_colorButton);

    row->addStretch(1);
    v->addLayout(row);

    // ── 示意图 ──
    m_preview = new CanvasPreviewWidget(extra);
    m_preview->setObjectName(QStringLiteral("canvasPreview"));
    v->addWidget(m_preview);

    m_legend = new QLabel(extra);
    m_legend->setObjectName(QStringLiteral("labelLegend"));
    m_legend->setTextFormat(Qt::RichText);
    m_legend->setText(tr("<span style='color:#333333'>Solid</span> = new canvas　"
                         "<span style='color:#5a5a5a'>Dashed</span> = original canvas extent　"
                         "<span style='color:#2a6eb4'>■</span> content　"
                         "<span style='color:#c82828'>■</span> will be cropped"));
    v->addWidget(m_legend);

    // ── 交给基类完成整体布局 ──
    //
    // ★ 必须在九宫格建好之后调用 ★
    //   buildLayout() 内部会调用 infoText() 与 onSizeEdited()，两者都
    //   依赖 anchor()。此时按钮组已就位，读到的是构造参数指定的锚点。
    buildLayout(extra);

    // 按钮组的接线放在最后：构造期间 setChecked 不会触发点击信号，
    // 但把接线统一放到初始化之后，能保证"任何一次刷新都发生在
    // 控件齐全之后"这个不变式只有一个入口。
    connect(m_anchorGroup, QOverload<int>::of(&QButtonGroup::idClicked),
            this, &CanvasSizeDialog::onAnchorClicked);

    updateColorButton();
}

CanvasSizeDialog::~CanvasSizeDialog() = default;

Anchor CanvasSizeDialog::anchor() const
{
    if (!m_anchorGroup)
        return Anchor::Center;

    const int id = m_anchorGroup->checkedId();
    if (id < 0 || id > 8)
        return Anchor::Center;

    return static_cast<Anchor>(id);
}

QPoint CanvasSizeDialog::contentOffset() const
{
    // ★ 和 Document::resizeCanvas 走同一个函数 ★
    //   smart 恒为 true —— 智能适配在 Document 那边本来就只在缩小时
    //   才生效，放大时传 true 也是纯锚点行为。
    return canvasResizeOffset(m_contentBounds, originalSize(), newSize(),
                              anchor(), /*smart=*/true);
}

CanvasSizeDialog::CropInfo CanvasSizeDialog::cropInfo(const QSize &newSize,
                                                      const QPoint &offset) const
{
    CropInfo info;
    if (m_contentBounds.isEmpty())
        return info;      // 没有内容，裁不到东西

    const QRect contentInNew = m_contentBounds.translated(offset);

    info.left   = qMax(0, -contentInNew.left());
    info.top    = qMax(0, -contentInNew.top());
    info.right  = qMax(0, contentInNew.right()  - (newSize.width()  - 1));
    info.bottom = qMax(0, contentInNew.bottom() - (newSize.height() - 1));
    return info;
}

QString CanvasSizeDialog::infoText(const QSize &newSize) const
{
    const QSize oldSize = originalSize();
    const QPoint offset = contentOffset();

    QString html;
    html += tr("Original canvas: %1 × %2 px").arg(oldSize.width()).arg(oldSize.height());
    html += QStringLiteral("<br>");
    html += tr("New canvas: <b>%1 × %2 px</b>").arg(newSize.width()).arg(newSize.height());

    // ── 面积变化 ──
    //
    // ★ 用百分比而不是"百万像素差" ★
    //   绝对差值在画布小的时候会退化成 0.02 MP 这种没什么信息量的数字
    //   （甚至被阈值直接吞掉，让用户看不到"到底变大了还是变小了"）。
    //   百分比与画布尺寸无关，任何量级下都读得懂。
    const double oldArea = double(oldSize.width()) * oldSize.height();
    const double newArea = double(newSize.width()) * newSize.height();

    // 半个百分点以下不值得一说：那只是取整带来的抖动
    if (oldArea > 0.0 && qAbs(newArea - oldArea) > 0.005 * oldArea) {
        const double pct = 100.0 * (newArea - oldArea) / oldArea;
        const QString color = (pct > 0.0) ? QStringLiteral("#c86400")
                                          : QStringLiteral("#1f7a1f");
        html += QStringLiteral("　<span style='color:%1'>（面积%2 %3%）</span>")
                    .arg(color)
                    .arg(pct > 0.0 ? tr("Enlarge") : tr("Shrink"))
                    .arg(QString::number(qAbs(pct), 'f', 0));
    }

    // ── 智能适配提示 ──
    //
    // ★ 只在**真的发生了平移**时才提示 ★
    //   否则每打开一次对话框都看到一句"内容将自动偏移 (0, 0) 像素"，
    //   用户会先学会忽略这句话 —— 而它恰恰是最需要被看见的一条。
    const QPoint naive = anchorOffset(anchor(), oldSize, newSize);
    if (offset != naive) {
        html += QStringLiteral("<br><span style='color:#1f5fa9'>")
              + tr("Smart fit: the content will be offset by (%1, %2) pixels "
                   "so that nothing sticking out gets cropped.")
                    .arg(offset.x() - naive.x())
                    .arg(offset.y() - naive.y())
              + QStringLiteral("</span>");
    }

    // ── 裁切警告 ──
    const CropInfo crop = cropInfo(newSize, offset);
    if (crop.any()) {
        QStringList parts;
        if (crop.left)   parts << tr("Left %1 px").arg(crop.left);
        if (crop.right)  parts << tr("Right %1 px").arg(crop.right);
        if (crop.top)    parts << tr("Top %1 px").arg(crop.top);
        if (crop.bottom) parts << tr("Bottom %1 px").arg(crop.bottom);

        html += QStringLiteral("<br><span style='color:#b00000'><b>")
              + tr("Crop: content will be cropped (%1)").arg(parts.join(tr(", ")))
              + QStringLiteral("</b></span>");
    } else if (m_contentBounds.isEmpty()) {
        html += QStringLiteral("<br><span style='color:#666666'>")
              + tr("There is no visible content in the current canvas; resizing will not crop anything.")
              + QStringLiteral("</span>");
    } else {
        html += QStringLiteral("<br><span style='color:#1f7a1f'>")
              + tr("The content is fully preserved.")
              + QStringLiteral("</span>");
    }

    return html;
}

void CanvasSizeDialog::onSizeEdited(const QSize &newSize)
{
    Q_UNUSED(newSize)   // 预览直接用 newSize() 读，不再传一遍
    updatePreview();
}

void CanvasSizeDialog::onAnchorClicked(int /*id*/)
{
    // ★ 尺寸没变，但信息面板与示意图全都要重算 ★
    //   基类只在尺寸变化时自动刷新，锚点变化它不知道 —— 这正是
    //   refreshInfo() 被开放给子类的原因。
    refreshInfo();
    updatePreview();
}

void CanvasSizeDialog::onPickColor()
{
    QColorDialog dlg(this);
    dlg.setWindowTitle(tr("Choose Canvas Fill Color"));
    dlg.setCurrentColor(m_fillColor);
    // 必须开放 alpha 通道：默认的"透明"就是 alpha = 0，
    // 不给 alpha 滑块的话用户选不回透明。
    dlg.setOption(QColorDialog::ShowAlphaChannel, true);

    if (dlg.exec() != QDialog::Accepted)
        return;

    m_fillColor = dlg.selectedColor();
    updateColorButton();
    updatePreview();
}

void CanvasSizeDialog::updateColorButton()
{
    if (!m_colorButton)
        return;

    const QSize iconSize(26, 16);
    m_colorButton->setIcon(QIcon(makeSwatch(m_fillColor, iconSize)));
    m_colorButton->setIconSize(iconSize);

    if (m_fillColor.alpha() == 0) {
        m_colorButton->setText(tr("Transparent"));
    } else if (m_fillColor.alpha() == 255) {
        m_colorButton->setText(m_fillColor.name().toUpper());
    } else {
        // 半透明时把 alpha 也写出来，否则 #RRGGBB 会掩盖"这其实没填实"
        m_colorButton->setText(m_fillColor.name(QColor::HexArgb).toUpper());
    }
}

void CanvasSizeDialog::updatePreview()
{
    if (m_preview) {
        m_preview->setState(originalSize(), newSize(), m_contentBounds,
                            contentOffset(), m_fillColor);
    }
}

} // namespace zl
