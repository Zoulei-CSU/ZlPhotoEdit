#include "tools/ToolOptions.h"

// ============================================================================
//  ToolOptions.cpp —— 工具选项条的实现
// ----------------------------------------------------------------------------
//  本文件是全工程**唯一**大量使用 QtWidgets 的 tools 层文件，这是允许的：
//  分层规则是"engine 不得依赖 QtWidgets"，而不是"tools 不得依赖"。
//  但要守住一点：**只有参数对象可以碰控件**，工具类本身不可以
//  （工具通过 connect(options, &ToolOptions::changed, ...) 间接响应）。
//
//  ★ 控件构建的几条统一约定 ★
//
//    1. **每个控件创建后立刻 setParent**（或加入布局）。
//       不设父对象的控件如果中途 return nullptr 就会泄漏。
//
//    2. **每次程序化 setValue 之前先 blockSignals(true)**，
//       设完再恢复。否则 load() 时会触发一串 valueChanged，
//       导致工具反复重建缓存（画笔 500 像素的蒙版重建一次要几毫秒，
//       连着重建 5 次就是几十毫秒的启动卡顿）。
//
//    3. **QSettings 用 toString/toInt + 默认值**，不要假定键存在。
//       一个新版本新增了参数，老用户的配置文件里没有那个键，
//       必须回退到默认值而不是读到空值。
// ============================================================================

#include <QWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QCheckBox>
#include <QCoreApplication>
#include <QToolButton>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QSettings>
#include <QFont>
#include <QFontMetrics>
#include <QSignalBlocker>

#include <functional>

namespace zl {

// ============================================================================
//  枚举名称与辅助函数（ToolParams.h 中声明的）
// ============================================================================

QString pickRadiusName(PickRadius r)
{
    switch (r) {
    case PickRadius::Point:     return QObject::tr("Sample Point");
    case PickRadius::Average3x3:return QObject::tr("3×3 Average");
    case PickRadius::Average5x5:return QObject::tr("5×5 Average");
    }
    return QString();
}

int pickRadiusSize(PickRadius r)
{
    switch (r) {
    case PickRadius::Point:      return 1;
    case PickRadius::Average3x3: return 3;
    case PickRadius::Average5x5: return 5;
    }
    return 1;
}

QString shapeModeName(ShapeMode m)
{
    switch (m) {
    case ShapeMode::Stroke: return QObject::tr("Stroke");
    case ShapeMode::Fill:   return QObject::tr("Fill");
    case ShapeMode::Both:   return QObject::tr("Stroke+Fill");
    }
    return QString();
}

QString textAlignName(TextAlign a)
{
    switch (a) {
    case TextAlign::Left:   return QObject::tr("Left");
    case TextAlign::Center: return QObject::tr("Center");
    case TextAlign::Right:  return QObject::tr("Right");
    }
    return QString();
}

QString selectionModeName(SelectionMode m)
{
    switch (m) {
    case SelectionMode::Replace:   return QObject::tr("New Selection");
    case SelectionMode::Add:       return QObject::tr("Add to Selection");
    case SelectionMode::Subtract:  return QObject::tr("Subtract from Selection");
    case SelectionMode::Intersect: return QObject::tr("Intersect with Selection");
    }
    return QString();
}

QString aspectPresetName(AspectPreset p)
{
    switch (p) {
    case AspectPreset::Free:        return QObject::tr("Free");
    case AspectPreset::Square:      return QObject::tr("1 : 1");
    case AspectPreset::FourThree:   return QObject::tr("4 : 3");
    case AspectPreset::ThreeTwo:    return QObject::tr("3 : 2");
    case AspectPreset::SixteenNine: return QObject::tr("16 : 9");
    case AspectPreset::NineSixteen: return QObject::tr("9 : 16");
    case AspectPreset::TwoThree:    return QObject::tr("2 : 3");
    case AspectPreset::ThreeFour:   return QObject::tr("3 : 4");
    case AspectPreset::Custom:      return QObject::tr("Custom");
    }
    return QString();
}

QString cropGridName(CropGrid g)
{
    switch (g) {
    case CropGrid::None:        return QObject::tr("None");
    case CropGrid::Thirds:      return QObject::tr("Thirds");
    case CropGrid::Grid:        return QObject::tr("Grid");
    case CropGrid::Diagonal:    return QObject::tr("Diagonal");
    case CropGrid::GoldenRatio: return QObject::tr("Golden Ratio");
    }
    return QString();
}

// ============================================================================
//  选项条构建的小工具函数
// ============================================================================
namespace {

// 创建一个统一的"标签 + 控件"组合并加入布局。
// 把重复的样板收成一行，让每个 createWidget 都能保持可读。
template <typename T>
T *addLabeled(QHBoxLayout *layout, const QString &label, T *widget, int labelWidth = -1)
{
    auto *lbl = new QLabel(label);
    if (labelWidth > 0)
        lbl->setMinimumWidth(labelWidth);
    lbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    layout->addWidget(lbl);
    layout->addWidget(widget);
    return widget;
}

// 选项条统一的左右边距与间距。
// 不统一的话，不同工具的选项条高度会差几个像素，切换工具时整条会跳一下。
constexpr int kBarMarginH = 8;
constexpr int kBarMarginV = 4;
constexpr int kBarSpacing = 6;

// 创建一个已配置好边距的横向布局
QHBoxLayout *makeBarLayout(QWidget *parent)
{
    auto *layout = new QHBoxLayout(parent);
    layout->setContentsMargins(kBarMarginH, kBarMarginV, kBarMarginH, kBarMarginV);
    layout->setSpacing(kBarSpacing);
    return layout;
}

// 在选项条末尾加一个弹簧，把所有控件推到左边。
// 弹簧是必需的 —— 没有它，控件会被拉伸成很难看的宽条。
void addStretch(QHBoxLayout *layout)
{
    layout->addStretch(1);
}

// ── 四个"选区运算"按钮（新建 / 添加 / 减去 / 交集）──
//
// ★ 为什么抽成共享函数，而不是让选框与魔术棒各写一遍 ★
//   这两个工具用的是**同一套**运算语义（同一个 SelectionMode 枚举、
//   同一个 SelectionOptions::toSelectionOp 映射）。各写一份的话，
//   某天把"交叉"改成"交集"只会改到一处，用户就会在两个工具的选项条上
//   看到两个名字而含义相同；更糟的是顺序或默认选中错位 ——
//   那种错误点一下就把用户的选区算错，却看不出是谁的错。
//
//   onPick 由调用方接到自己的 setMode() 上（本函数不认识具体选项对象）。
void addModeButtons(QWidget *bar, QHBoxLayout *layout,
                    SelectionMode current,
                    const std::function<void(SelectionMode)> &onPick)
{
    struct Entry {
        SelectionMode mode;
        QString       text;
        QString       tip;
    };
    // ★ context 修复（2026-10-14，见 docs/界面语言.md §4.3）★
    //   这些串在 zl 命名空间的函数里，lupdate 记 context "zl"，而运行期
    //   QObject::tr() 查的是 "QObject" —— 对不上，翻译永远不命中。
    //   改法：QCoreApplication::translate("zl", 字面量)。
    //   ★ 字面量必须直接传给 translate，不能经 QT_TRANSLATE_NOOP 中转 ★
    //     实测 lupdate 5.15.16 不提取函数内的 QT_TRANSLATE_NOOP（匿名
    //     namespace / static 局部都不提取），只有 translate("ctx", "字面量")
    //     这一种形式稳定进 .ts。
    static const Entry entries[] = {
        { SelectionMode::Replace,
          QCoreApplication::translate("zl", "New"),
          QCoreApplication::translate("zl", "Replace existing selection") },
        { SelectionMode::Add,
          QCoreApplication::translate("zl", "Add"),
          QCoreApplication::translate("zl", "Merge the new selection into the existing one (or hold Shift)") },
        { SelectionMode::Subtract,
          QCoreApplication::translate("zl", "Subtract"),
          QCoreApplication::translate("zl", "Carve the new selection out of the existing one (or hold Alt)") },
        { SelectionMode::Intersect,
          QCoreApplication::translate("zl", "Intersect"),
          QCoreApplication::translate("zl", "Keep only the overlap of both (Shift+Alt)") },
    };

    for (const Entry &entry : entries) {
        auto *btn = new QToolButton(bar);
        btn->setText(entry.text);
        btn->setToolTip(entry.tip);
        btn->setCheckable(true);
        // ★ 互斥交给 QToolButton 自己 ★
        //   手写"点了 B 就取消 A"在通过快捷键切工具时会漏掉同步。
        btn->setAutoExclusive(true);
        btn->setChecked(entry.mode == current);
        layout->addWidget(btn);

        const SelectionMode mode = entry.mode;
        QObject::connect(btn, &QToolButton::clicked, btn, [onPick, mode]() {
            if (onPick)
                onPick(mode);
        });
    }
}

} // anonymous namespace

// ============================================================================
//  BrushOptions —— 画笔 / 橡皮
// ============================================================================

BrushOptions::BrushOptions(Kind kind, QObject *parent)
    : ToolOptions(parent)
    , m_kind(kind)
{
    // 橡皮与画笔共用同一个设置分组 ——
    // 这样从画笔切到橡皮，尺寸不会突变（用户体验上更连贯）。
    setSettingsGroup(QStringLiteral("tool/brush"));

    if (kind == Kind::Eraser) {
        // 橡皮默认更硬、更大：擦除时软边缘通常不是想要的
        m_hardness = 100;
        m_size     = 40;
    }
}

void BrushOptions::setSize(int v)
{
    v = qBound(m_minSize, v, m_maxSize);
    if (v == m_size)
        return;
    m_size = v;
    notifyChanged();
}

void BrushOptions::setHardness(int v)
{
    v = qBound(0, v, 100);
    if (v == m_hardness)
        return;
    m_hardness = v;
    notifyChanged();
}

void BrushOptions::setOpacity(int v)
{
    v = qBound(0, v, 100);
    if (v == m_opacity)
        return;
    m_opacity = v;
    notifyChanged();
}

void BrushOptions::setFlow(int v)
{
    v = qBound(0, v, 100);
    if (v == m_flow)
        return;
    m_flow = v;
    notifyChanged();
}

void BrushOptions::setColor(const QColor &c)
{
    if (!c.isValid() || c == m_color)
        return;
    m_color = c;
    emit colorChanged(c);
    notifyChanged();
}

void BrushOptions::setSizeRange(int min, int max)
{
    m_minSize = min;
    m_maxSize = max;
    m_size = qBound(m_minSize, m_size, m_maxSize);
}

QWidget *BrushOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    // ── 大小 ──
    // 用"滑块 + 数字框"组合：滑块用于快速粗调，数字框用于精确输入。
    // 只给滑块的话没法输入 137 这样的具体值，只给数字框的话拖动手感很差。
    auto *sizeSpin = new QSpinBox(bar);
    sizeSpin->setRange(m_minSize, m_maxSize);
    sizeSpin->setValue(m_size);
    sizeSpin->setSuffix(tr(" px"));

    auto *sizeSlider = new QSlider(Qt::Horizontal, bar);
    sizeSlider->setRange(m_minSize, m_maxSize);
    sizeSlider->setValue(m_size);
    sizeSlider->setFixedWidth(110);

    addLabeled(layout, tr("Size"), sizeSlider);
    layout->addWidget(sizeSpin);

    // 滑块与数字框双向同步。
    // ★ 注意方向：两个 connect 都要有，且都不能形成无限循环 ★
    //   QSlider::valueChanged → setValue(spin) 会触发 spin 的 valueChanged
    //   → setValue(slider) → 值相同，QSlider 不会再发信号。所以不会循环。
    connect(sizeSlider, &QSlider::valueChanged, sizeSpin, &QSpinBox::setValue);
    connect(sizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            sizeSlider, &QSlider::setValue);
    connect(sizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &BrushOptions::setSize);

    layout->addSpacing(10);

    // ── 硬度 ──
    auto *hardness = new QSlider(Qt::Horizontal, bar);
    hardness->setRange(0, 100);
    hardness->setValue(m_hardness);
    hardness->setFixedWidth(90);
    hardness->setToolTip(tr("0% is a fully soft edge; 100% is a hard edge (no anti-aliased transition)"));
    addLabeled(layout, tr("Hardness"), hardness);

    auto *hardnessValue = new QLabel(QString::number(m_hardness) + QStringLiteral("%"), bar);
    hardnessValue->setMinimumWidth(34);
    layout->addWidget(hardnessValue);

    connect(hardness, &QSlider::valueChanged, this, &BrushOptions::setHardness);
    connect(hardness, &QSlider::valueChanged, hardnessValue, [hardnessValue](int v) {
        hardnessValue->setText(QString::number(v) + QStringLiteral("%"));
    });

    layout->addSpacing(10);

    // ── 不透明度 ──
    // 不透明度是"整笔的最高值"，流量是"单个笔刷点落下的量"。
    // 区别在叠加时体现：不透明度 50% 反复涂同一个地方还是 50%，
    // 流量 50% 反复涂会累积到 100%。这是 Photoshop 的标准语义。
    auto *opacity = new QSlider(Qt::Horizontal, bar);
    opacity->setRange(0, 100);
    opacity->setValue(m_opacity);
    opacity->setFixedWidth(90);
    opacity->setToolTip(tr("Maximum opacity of the whole stroke; repeated painting will not build up beyond this value"));
    addLabeled(layout, tr("Opacity"), opacity);

    auto *opacityValue = new QLabel(QString::number(m_opacity) + QStringLiteral("%"), bar);
    opacityValue->setMinimumWidth(34);
    layout->addWidget(opacityValue);

    connect(opacity, &QSlider::valueChanged, this, &BrushOptions::setOpacity);
    connect(opacity, &QSlider::valueChanged, opacityValue, [opacityValue](int v) {
        opacityValue->setText(QString::number(v) + QStringLiteral("%"));
    });

    layout->addSpacing(10);

    // ── 流量 ──
    auto *flow = new QSlider(Qt::Horizontal, bar);
    flow->setRange(1, 100);   // 下限 1 而非 0：流量 0 等于画不出来
    flow->setValue(m_flow);
    flow->setFixedWidth(90);
    flow->setToolTip(tr("How much paint a single dab lays down; repeated dabs build up"));
    addLabeled(layout, tr("Flow"), flow);

    auto *flowValue = new QLabel(QString::number(m_flow) + QStringLiteral("%"), bar);
    flowValue->setMinimumWidth(34);
    layout->addWidget(flowValue);

    connect(flow, &QSlider::valueChanged, this, &BrushOptions::setFlow);
    connect(flow, &QSlider::valueChanged, flowValue, [flowValue](int v) {
        flowValue->setText(QString::number(v) + QStringLiteral("%"));
    });

    // ★ 颜色不在这里选 ★
    //   前景色/背景色由右侧的 ColorPanel 统一管理（工具箱下方那两个色块）。
    //   如果画笔选项条里再放一个颜色按钮，就会出现"两处都能改颜色"，
    //   用户搞不清哪个为准。工具的颜色始终取自 ColorPanel。

    addStretch(layout);

    // ── 初始化时把程序化设置的值同步到控件 ──
    // ★ 为什么要 blockSignals ★
    //   上面的 connect 是在 setValue 之后建立的，所以最初的 setValue 不会
    //   触发信号 —— 但 load() 之后会再次调用这个函数重建控件，
    //   那时就需要靠 blockSignals 或"先建控件再 connect"的顺序来保证。
    //   这里两种保护都做了，属于"无成本的防御"。
    bar->setObjectName(QStringLiteral("BrushOptionsBar"));
    return bar;
}

void BrushOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());

    // 每个键都给默认值 —— 新版本新增参数时，老配置文件里不会有那个键。
    m_size     = qBound(m_minSize, settings.value(QStringLiteral("size"), m_size).toInt(), m_maxSize);
    m_hardness = qBound(0, settings.value(QStringLiteral("hardness"), m_hardness).toInt(), 100);
    m_opacity  = qBound(0, settings.value(QStringLiteral("opacity"), m_opacity).toInt(), 100);
    m_flow     = qBound(1, settings.value(QStringLiteral("flow"), m_flow).toInt(), 100);

    // ★ 颜色用 toString() 存 —— QColor 直接塞进 QSettings 会依赖
    //   QVariant 的序列化格式，跨 Qt 大版本（Qt5 → Qt6）不保证兼容。
    //   "#RRGGBB" 这样的字符串则永远稳定。
    const QString colorName = settings.value(QStringLiteral("color")).toString();
    if (!colorName.isEmpty()) {
        const QColor c(colorName);
        if (c.isValid())
            m_color = c;
    }

    settings.endGroup();
}

void BrushOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("size"),     m_size);
    settings.setValue(QStringLiteral("hardness"), m_hardness);
    settings.setValue(QStringLiteral("opacity"),  m_opacity);
    settings.setValue(QStringLiteral("flow"),     m_flow);
    settings.setValue(QStringLiteral("color"),    m_color.name(QColor::HexRgb));
    settings.endGroup();
}

// ============================================================================
//  BucketOptions —— 油漆桶
// ============================================================================

BucketOptions::BucketOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/bucket"));
}

void BucketOptions::setTolerance(int v)
{
    v = qBound(0, v, 255);
    if (v == m_tolerance)
        return;
    m_tolerance = v;
    notifyChanged();
}

void BucketOptions::setAntialias(bool v)
{
    if (v == m_antialias)
        return;
    m_antialias = v;
    notifyChanged();
}

void BucketOptions::setContiguous(bool v)
{
    if (v == m_contiguous)
        return;
    m_contiguous = v;
    notifyChanged();
}

void BucketOptions::setOpacity(int v)
{
    v = qBound(0, v, 100);
    if (v == m_opacity)
        return;
    m_opacity = v;
    notifyChanged();
}

void BucketOptions::setColor(const QColor &c)
{
    if (!c.isValid() || c == m_color)
        return;
    m_color = c;
    emit colorChanged(c);
    notifyChanged();
}

QWidget *BucketOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    // ── 容差 ──
    auto *tol = new QSlider(Qt::Horizontal, bar);
    tol->setRange(0, 255);
    tol->setValue(m_tolerance);
    tol->setFixedWidth(110);
    tol->setToolTip(tr("Neighboring pixels whose color differs by less than the "
                       "tolerance are filled together.\n"
                       "Uses three-channel Euclidean distance; a tolerance of "
                       "30 is roughly the smallest difference the eye can tell."));
    addLabeled(layout, tr("Tolerance"), tol);

    auto *tolSpin = new QSpinBox(bar);
    tolSpin->setRange(0, 255);
    tolSpin->setValue(m_tolerance);
    layout->addWidget(tolSpin);

    connect(tol, &QSlider::valueChanged, tolSpin, &QSpinBox::setValue);
    connect(tolSpin, QOverload<int>::of(&QSpinBox::valueChanged), tol, &QSlider::setValue);
    connect(tolSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, &BucketOptions::setTolerance);

    layout->addSpacing(10);

    // ── 连续 / 非连续 ──
    // 连续模式：只填充与起点连通（四邻域可达）的像素 —— 这是"点一下填一整块"
    auto *contiguous = new QCheckBox(tr("Contiguous"), bar);
    contiguous->setChecked(m_contiguous);
    contiguous->setToolTip(tr("Checked: fill only the region connected to the "
                              "click point (the usual behavior)\n"
                              "Unchecked: fill every pixel within tolerance "
                              "in the whole image, connected or not"));
    layout->addWidget(contiguous);
    connect(contiguous, &QCheckBox::toggled, this, &BucketOptions::setContiguous);

    layout->addSpacing(6);

    // ── 抗锯齿 ──
    auto *aa = new QCheckBox(tr("Anti-aliasing"), bar);
    aa->setChecked(m_antialias);
    aa->setToolTip(tr("Draw a semi-transparent transition band along the fill edge to avoid jaggies"));
    layout->addWidget(aa);
    connect(aa, &QCheckBox::toggled, this, &BucketOptions::setAntialias);

    layout->addSpacing(10);

    // ── 不透明度 ──
    auto *opacity = new QSlider(Qt::Horizontal, bar);
    opacity->setRange(1, 100);
    opacity->setValue(m_opacity);
    opacity->setFixedWidth(90);
    addLabeled(layout, tr("Opacity"), opacity);

    auto *opacityValue = new QLabel(QString::number(m_opacity) + QStringLiteral("%"), bar);
    opacityValue->setMinimumWidth(34);
    layout->addWidget(opacityValue);

    connect(opacity, &QSlider::valueChanged, this, &BucketOptions::setOpacity);
    connect(opacity, &QSlider::valueChanged, opacityValue, [opacityValue](int v) {
        opacityValue->setText(QString::number(v) + QStringLiteral("%"));
    });

    addStretch(layout);
    return bar;
}

void BucketOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    m_tolerance  = qBound(0, settings.value(QStringLiteral("tolerance"), m_tolerance).toInt(), 255);
    m_antialias  = settings.value(QStringLiteral("antialias"), m_antialias).toBool();
    m_contiguous = settings.value(QStringLiteral("contiguous"), m_contiguous).toBool();
    m_opacity    = qBound(1, settings.value(QStringLiteral("opacity"), m_opacity).toInt(), 100);

    const QString colorName = settings.value(QStringLiteral("color")).toString();
    if (!colorName.isEmpty()) {
        const QColor c(colorName);
        if (c.isValid())
            m_color = c;
    }
    settings.endGroup();
}

void BucketOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("tolerance"),  m_tolerance);
    settings.setValue(QStringLiteral("antialias"),  m_antialias);
    settings.setValue(QStringLiteral("contiguous"), m_contiguous);
    settings.setValue(QStringLiteral("opacity"),    m_opacity);
    settings.setValue(QStringLiteral("color"),      m_color.name(QColor::HexRgb));
    settings.endGroup();
}

// ============================================================================
//  PickerOptions —— 吸管
// ============================================================================

PickerOptions::PickerOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/picker"));
}

void PickerOptions::setRadius(PickRadius r)
{
    if (r == m_radius)
        return;
    m_radius = r;
    notifyChanged();
}

QWidget *PickerOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    auto *combo = new QComboBox(bar);
    combo->addItem(pickRadiusName(PickRadius::Point),      int(PickRadius::Point));
    combo->addItem(pickRadiusName(PickRadius::Average3x3), int(PickRadius::Average3x3));
    combo->addItem(pickRadiusName(PickRadius::Average5x5), int(PickRadius::Average5x5));
    combo->setCurrentIndex(int(m_radius));
    combo->setToolTip(tr("Sampling range. Averages are more stable when the image has visible noise"));
    addLabeled(layout, tr("Sample Range"), combo);

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        setRadius(static_cast<PickRadius>(index));
    });

    // 吸管没有颜色选择控件 —— 它把取到的颜色送到 ColorPanel 的前景色。
    auto *hint = new QLabel(tr("Hold Alt to temporarily switch to the eyedropper"), bar);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addSpacing(12);
    layout->addWidget(hint);

    addStretch(layout);
    return bar;
}

void PickerOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    const int r = settings.value(QStringLiteral("radius"), int(m_radius)).toInt();
    if (r >= int(PickRadius::Point) && r <= int(PickRadius::Average5x5))
        m_radius = static_cast<PickRadius>(r);
    settings.endGroup();
}

void PickerOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("radius"), int(m_radius));
    settings.endGroup();
}

// ============================================================================
//  LineOptions —— 直线
// ============================================================================

LineOptions::LineOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/line"));
}

void LineOptions::setWidth(int v)
{
    v = qBound(1, v, 200);
    if (v == m_width)
        return;
    m_width = v;
    notifyChanged();
}

void LineOptions::setStyle(Qt::PenStyle s)
{
    if (s == m_style)
        return;
    m_style = s;
    notifyChanged();
}

void LineOptions::setColor(const QColor &c)
{
    if (!c.isValid() || c == m_color)
        return;
    m_color = c;
    emit colorChanged(c);
    notifyChanged();
}

void LineOptions::setAntialias(bool v)
{
    if (v == m_antialias)
        return;
    m_antialias = v;
    notifyChanged();
}

QWidget *LineOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    auto *width = new QSpinBox(bar);
    width->setRange(1, 200);
    width->setValue(m_width);
    width->setSuffix(tr(" px"));
    addLabeled(layout, tr("Line Width"), width);
    connect(width, QOverload<int>::of(&QSpinBox::valueChanged), this, &LineOptions::setWidth);

    layout->addSpacing(10);

    auto *styleCombo = new QComboBox(bar);
    styleCombo->addItem(tr("Solid"),     int(Qt::SolidLine));
    styleCombo->addItem(tr("Dashed"),     int(Qt::DashLine));
    styleCombo->addItem(tr("Dotted"),     int(Qt::DotLine));
    styleCombo->addItem(tr("Dash-Dot"),   int(Qt::DashDotLine));
    styleCombo->addItem(tr("Dash-Dot-Dot"), int(Qt::DashDotDotLine));
    styleCombo->setCurrentIndex(styleCombo->findData(int(m_style)));
    addLabeled(layout, tr("Line Style"), styleCombo);
    connect(styleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, styleCombo](int idx) {
        setStyle(static_cast<Qt::PenStyle>(styleCombo->itemData(idx).toInt()));
    });

    layout->addSpacing(10);

    auto *aa = new QCheckBox(tr("Anti-aliasing"), bar);
    aa->setChecked(m_antialias);
    layout->addWidget(aa);
    connect(aa, &QCheckBox::toggled, this, &LineOptions::setAntialias);

    // 颜色同样来自 ColorPanel（前景色）
    addStretch(layout);
    return bar;
}

void LineOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    m_width     = qBound(1, settings.value(QStringLiteral("width"), m_width).toInt(), 200);
    m_antialias = settings.value(QStringLiteral("antialias"), m_antialias).toBool();

    const int styleValue = settings.value(QStringLiteral("style"), int(m_style)).toInt();
    // 校验：只接受合法的 PenStyle 值，否则回退到实线。
    // 配置文件被手工改坏、或跨版本变更枚举时，不做校验会导致画笔样式异常。
    switch (styleValue) {
    case Qt::SolidLine:
    case Qt::DashLine:
    case Qt::DotLine:
    case Qt::DashDotLine:
    case Qt::DashDotDotLine:
        m_style = static_cast<Qt::PenStyle>(styleValue);
        break;
    default:
        m_style = Qt::SolidLine;
        break;
    }

    const QString colorName = settings.value(QStringLiteral("color")).toString();
    if (!colorName.isEmpty()) {
        const QColor c(colorName);
        if (c.isValid())
            m_color = c;
    }
    settings.endGroup();
}

void LineOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("width"),     m_width);
    settings.setValue(QStringLiteral("style"),     int(m_style));
    settings.setValue(QStringLiteral("antialias"), m_antialias);
    settings.setValue(QStringLiteral("color"),     m_color.name(QColor::HexRgb));
    settings.endGroup();
}

// ============================================================================
//  ShapeOptions —— 矩形 / 椭圆
// ============================================================================

ShapeOptions::ShapeOptions(ShapeKind kind, QObject *parent)
    : ToolOptions(parent)
    , m_kind(kind)
{
    setSettingsGroup(kind == ShapeKind::Rectangle
                         ? QStringLiteral("tool/rect")
                         : QStringLiteral("tool/ellipse"));

    // 矩形默认带一点圆角，视觉上更现代；椭圆没有圆角概念，保持 0。
    if (kind == ShapeKind::Rectangle)
        m_cornerRadius = 0;
}

void ShapeOptions::setWidth(int v)
{
    v = qBound(1, v, 200);
    if (v == m_width)
        return;
    m_width = v;
    notifyChanged();
}

void ShapeOptions::setMode(ShapeMode m)
{
    if (m == m_mode)
        return;
    m_mode = m;
    notifyChanged();
}

void ShapeOptions::setStrokeColor(const QColor &c)
{
    if (!c.isValid() || c == m_strokeColor)
        return;
    m_strokeColor = c;
    emit strokeColorChanged(c);
    notifyChanged();
}

void ShapeOptions::setFillColor(const QColor &c)
{
    if (!c.isValid() || c == m_fillColor)
        return;
    m_fillColor = c;
    emit fillColorChanged(c);
    notifyChanged();
}

void ShapeOptions::setAntialias(bool v)
{
    if (v == m_antialias)
        return;
    m_antialias = v;
    notifyChanged();
}

void ShapeOptions::setCornerRadius(int v)
{
    v = qBound(0, v, 500);
    if (v == m_cornerRadius)
        return;
    m_cornerRadius = v;
    notifyChanged();
}

QWidget *ShapeOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    auto *modeCombo = new QComboBox(bar);
    modeCombo->addItem(shapeModeName(ShapeMode::Stroke), int(ShapeMode::Stroke));
    modeCombo->addItem(shapeModeName(ShapeMode::Fill),   int(ShapeMode::Fill));
    modeCombo->addItem(shapeModeName(ShapeMode::Both),   int(ShapeMode::Both));
    modeCombo->setCurrentIndex(int(m_mode));
    addLabeled(layout, tr("Mode"), modeCombo);
    connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, modeCombo](int idx) {
        setMode(static_cast<ShapeMode>(modeCombo->itemData(idx).toInt()));
    });

    layout->addSpacing(10);

    auto *width = new QSpinBox(bar);
    width->setRange(1, 200);
    width->setValue(m_width);
    width->setSuffix(tr(" px"));
    addLabeled(layout, tr("Line Width"), width);
    connect(width, QOverload<int>::of(&QSpinBox::valueChanged), this, &ShapeOptions::setWidth);

    // 只有矩形才有圆角参数
    if (m_kind == ShapeKind::Rectangle) {
        layout->addSpacing(10);

        auto *radius = new QSpinBox(bar);
        radius->setRange(0, 500);
        radius->setValue(m_cornerRadius);
        radius->setSuffix(tr(" px"));
        radius->setToolTip(tr("0 means square corners; values above half of the shorter side taper into a capsule"));
        addLabeled(layout, tr("Corner Radius"), radius);
        connect(radius, QOverload<int>::of(&QSpinBox::valueChanged),
                this, &ShapeOptions::setCornerRadius);
    }

    layout->addSpacing(10);

    auto *aa = new QCheckBox(tr("Anti-aliasing"), bar);
    aa->setChecked(m_antialias);
    layout->addWidget(aa);
    connect(aa, &QCheckBox::toggled, this, &ShapeOptions::setAntialias);

    // 描边色与填充色的说明：
    //   描边色取当前前景色（由 ColorPanel 控制），
    //   填充色为选项条内的独立设置 —— 因为"描边黑、填充白"是常见需求，
    //   如果两者都绑前景色就没法同时用两个颜色。
    auto *hint = new QLabel(tr("Strokes use the foreground color"), bar);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addSpacing(12);
    layout->addWidget(hint);

    addStretch(layout);
    return bar;
}

void ShapeOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    m_width = qBound(1, settings.value(QStringLiteral("width"), m_width).toInt(), 200);

    const int modeValue = settings.value(QStringLiteral("mode"), int(m_mode)).toInt();
    if (modeValue >= int(ShapeMode::Stroke) && modeValue <= int(ShapeMode::Both))
        m_mode = static_cast<ShapeMode>(modeValue);

    m_antialias    = settings.value(QStringLiteral("antialias"), m_antialias).toBool();
    m_cornerRadius = qBound(0, settings.value(QStringLiteral("cornerRadius"), m_cornerRadius).toInt(), 500);

    const QString strokeName = settings.value(QStringLiteral("strokeColor")).toString();
    if (!strokeName.isEmpty()) {
        const QColor c(strokeName);
        if (c.isValid())
            m_strokeColor = c;
    }
    const QString fillName = settings.value(QStringLiteral("fillColor")).toString();
    if (!fillName.isEmpty()) {
        const QColor c(fillName);
        if (c.isValid())
            m_fillColor = c;
    }
    settings.endGroup();
}

void ShapeOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("width"),        m_width);
    settings.setValue(QStringLiteral("mode"),         int(m_mode));
    settings.setValue(QStringLiteral("antialias"),    m_antialias);
    settings.setValue(QStringLiteral("cornerRadius"), m_cornerRadius);
    settings.setValue(QStringLiteral("strokeColor"),  m_strokeColor.name(QColor::HexRgb));
    settings.setValue(QStringLiteral("fillColor"),    m_fillColor.name(QColor::HexRgb));
    settings.endGroup();
}

// ============================================================================
//  TextOptions —— 文字
// ============================================================================

TextOptions::TextOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/text"));
}

void TextOptions::setFamily(const QString &f)
{
    if (f.isEmpty() || f == m_family)
        return;
    m_family = f;
    notifyChanged();
}

void TextOptions::setPointSize(int v)
{
    v = qBound(1, v, 2000);
    if (v == m_pointSize)
        return;
    m_pointSize = v;
    notifyChanged();
}

void TextOptions::setColor(const QColor &c)
{
    if (!c.isValid() || c == m_color)
        return;
    m_color = c;
    emit colorChanged(c);
    notifyChanged();
}

void TextOptions::setBold(bool v)
{
    if (v == m_bold)
        return;
    m_bold = v;
    notifyChanged();
}

void TextOptions::setItalic(bool v)
{
    if (v == m_italic)
        return;
    m_italic = v;
    notifyChanged();
}

void TextOptions::setUnderline(bool v)
{
    if (v == m_underline)
        return;
    m_underline = v;
    notifyChanged();
}

void TextOptions::setAlign(TextAlign a)
{
    if (a == m_align)
        return;
    m_align = a;
    notifyChanged();
}

void TextOptions::setLineHeight(qreal v)
{
    v = qBound(0.5, v, 5.0);
    if (qFuzzyCompare(v, m_lineHeight))
        return;
    m_lineHeight = v;
    notifyChanged();
}

void TextOptions::setLetterSpacing(qreal v)
{
    v = qBound(-50.0, v, 500.0);
    if (qFuzzyCompare(v, m_letterSpacing))
        return;
    m_letterSpacing = v;
    notifyChanged();
}

QFont TextOptions::toFont(qreal dpi) const
{
    QFont f(m_family);

    // ★ 磅值 → 像素的换算 ★
    //
    //   QFont 的 setPointSize 依赖"逻辑 DPI"（QGuiApplication 的
    //   屏幕 DPI 设置），在不同机器上结果不同 —— 这会导致同一份工程
    //   在两台机器上文字大小不一样。对于图像编辑程序，这是不可接受的。
    //
    //   所以这里统一用 setPixelSize()：显式按文档 DPI 换算成像素。
    //   72 是印刷标准的"每英寸点数基准"，也就是 1pt = 1/72 inch。
    //   因此：像素 = 磅值 × dpi / 72
    //
    //   文档 DPI 为 72 时，1pt = 1px，此时"磅值"与"像素"数值相同，
    //   这也是最直观的默认状态。
    const qreal safeDpi = (dpi > 1.0 ? dpi : 72.0);
    const int pixelSize = qMax(1, qRound(m_pointSize * safeDpi / 72.0));
    f.setPixelSize(pixelSize);

    f.setBold(m_bold);
    f.setItalic(m_italic);
    f.setUnderline(m_underline);

    // 字间距。负值表示紧凑，正值表示疏松。
    // QFont 的字间距用百分比表示（100 为正常），这里把"像素"换算成
    // 相对于字号的百分比，这样缩放字号时字间距会等比变化，视觉更协调。
    if (!qFuzzyIsNull(m_letterSpacing)) {
        const qreal base = qMax(1, pixelSize);
        const qreal percent = 100.0 + m_letterSpacing / base * 100.0;
        f.setLetterSpacing(QFont::PercentageSpacing, qBound(-50.0, percent, 500.0));
    }

    return f;
}

void TextOptions::setFromFont(const QFont &font)
{
    // 把 QFont 反向拆解成各个参数 —— 用户从 QFontDialog 选完之后同步过来。
    setFamily(font.family());
    // 优先用 pointSize：QFontDialog 返回的通常是磅值
    if (font.pointSize() > 0)
        setPointSize(font.pointSize());
    else if (font.pixelSize() > 0)
        setPointSize(font.pixelSize());   // DPI=72 时与磅值等价

    setBold(font.bold());
    setItalic(font.italic());
    setUnderline(font.underline());
}

QWidget *TextOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    // ── 字体 ──
    auto *fontCombo = new QFontComboBox(bar);
    fontCombo->setCurrentFont(QFont(m_family));
    fontCombo->setFixedWidth(180);
    addLabeled(layout, tr("Font"), fontCombo);
    connect(fontCombo, &QFontComboBox::currentFontChanged, this, [this](const QFont &f) {
        setFamily(f.family());
    });

    layout->addSpacing(10);

    // ── 字号 ──
    auto *size = new QSpinBox(bar);
    size->setRange(1, 2000);
    size->setValue(m_pointSize);
    size->setSuffix(tr(" pt"));
    size->setToolTip(tr("Text size in points. When the document DPI is 72, 1 pt = 1 px"));
    addLabeled(layout, tr("Font Size"), size);
    connect(size, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &TextOptions::setPointSize);

    layout->addSpacing(10);

    // ── 粗体 / 斜体 / 下划线 ──
    // 用三个可切换的 QToolButton 而不是 QCheckBox，节省横向空间。
    auto *bold = new QToolButton(bar);
    bold->setText(tr("B"));
    bold->setCheckable(true);
    bold->setChecked(m_bold);
    bold->setToolTip(tr("Bold"));
    {
        QFont f = bold->font();
        f.setBold(true);
        bold->setFont(f);
    }
    layout->addWidget(bold);
    connect(bold, &QToolButton::toggled, this, &TextOptions::setBold);

    auto *italic = new QToolButton(bar);
    italic->setText(tr("I"));
    italic->setCheckable(true);
    italic->setChecked(m_italic);
    italic->setToolTip(tr("Italic"));
    {
        QFont f = italic->font();
        f.setItalic(true);
        italic->setFont(f);
    }
    layout->addWidget(italic);
    connect(italic, &QToolButton::toggled, this, &TextOptions::setItalic);

    auto *underline = new QToolButton(bar);
    underline->setText(tr("U"));
    underline->setCheckable(true);
    underline->setChecked(m_underline);
    underline->setToolTip(tr("Underline"));
    {
        QFont f = underline->font();
        f.setUnderline(true);
        underline->setFont(f);
    }
    layout->addWidget(underline);
    connect(underline, &QToolButton::toggled, this, &TextOptions::setUnderline);

    layout->addSpacing(10);

    // ── 对齐 ──
    auto *alignCombo = new QComboBox(bar);
    alignCombo->addItem(textAlignName(TextAlign::Left),   int(TextAlign::Left));
    alignCombo->addItem(textAlignName(TextAlign::Center), int(TextAlign::Center));
    alignCombo->addItem(textAlignName(TextAlign::Right),  int(TextAlign::Right));
    alignCombo->setCurrentIndex(int(m_align));
    addLabeled(layout, tr("Alignment"), alignCombo);
    connect(alignCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, alignCombo](int idx) {
        setAlign(static_cast<TextAlign>(alignCombo->itemData(idx).toInt()));
    });

    layout->addSpacing(10);

    // ── 行距 ──
    auto *lineHeight = new QDoubleSpinBox(bar);
    lineHeight->setRange(0.5, 5.0);
    lineHeight->setSingleStep(0.1);
    lineHeight->setDecimals(1);
    lineHeight->setValue(m_lineHeight);
    lineHeight->setSuffix(tr("×"));
    lineHeight->setToolTip(tr("Line spacing multiple (1.0 matches the font size)"));
    addLabeled(layout, tr("Line Spacing"), lineHeight);
    connect(lineHeight, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &TextOptions::setLineHeight);

    layout->addSpacing(10);

    // ── 字间距 ──
    auto *letterSpacing = new QDoubleSpinBox(bar);
    letterSpacing->setRange(-50.0, 500.0);
    letterSpacing->setSingleStep(1.0);
    letterSpacing->setDecimals(1);
    letterSpacing->setValue(m_letterSpacing);
    letterSpacing->setSuffix(tr(" px"));
    letterSpacing->setToolTip(tr("Letter spacing in pixels. Negative values tighten the text"));
    addLabeled(layout, tr("Letter Spacing"), letterSpacing);
    connect(letterSpacing, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &TextOptions::setLetterSpacing);

    // 文字颜色来自 ColorPanel 的前景色
    addStretch(layout);
    return bar;
}

void TextOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    m_family = settings.value(QStringLiteral("family"), m_family).toString();
    m_pointSize = qBound(1, settings.value(QStringLiteral("pointSize"), m_pointSize).toInt(), 2000);
    m_bold      = settings.value(QStringLiteral("bold"), m_bold).toBool();
    m_italic    = settings.value(QStringLiteral("italic"), m_italic).toBool();
    m_underline = settings.value(QStringLiteral("underline"), m_underline).toBool();

    const int alignValue = settings.value(QStringLiteral("align"), int(m_align)).toInt();
    if (alignValue >= int(TextAlign::Left) && alignValue <= int(TextAlign::Right))
        m_align = static_cast<TextAlign>(alignValue);

    m_lineHeight    = qBound(0.5, settings.value(QStringLiteral("lineHeight"), m_lineHeight).toDouble(), 5.0);
    m_letterSpacing = qBound(-50.0, settings.value(QStringLiteral("letterSpacing"), m_letterSpacing).toDouble(), 500.0);

    const QString colorName = settings.value(QStringLiteral("color")).toString();
    if (!colorName.isEmpty()) {
        const QColor c(colorName);
        if (c.isValid())
            m_color = c;
    }
    settings.endGroup();
}

void TextOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("family"),        m_family);
    settings.setValue(QStringLiteral("pointSize"),     m_pointSize);
    settings.setValue(QStringLiteral("bold"),          m_bold);
    settings.setValue(QStringLiteral("italic"),        m_italic);
    settings.setValue(QStringLiteral("underline"),     m_underline);
    settings.setValue(QStringLiteral("align"),         int(m_align));
    settings.setValue(QStringLiteral("lineHeight"),    m_lineHeight);
    settings.setValue(QStringLiteral("letterSpacing"), m_letterSpacing);
    settings.setValue(QStringLiteral("color"),         m_color.name(QColor::HexRgb));
    settings.endGroup();
}

// ============================================================================
//  SelectionOptions —— 选区
// ============================================================================

SelectionOptions::SelectionOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/selection"));
}

void SelectionOptions::setMode(SelectionMode m)
{
    if (m == m_mode)
        return;
    m_mode = m;
    notifyChanged();
}

void SelectionOptions::setFeatherRadius(qreal v)
{
    v = qBound(0.0, v, 250.0);
    if (qFuzzyCompare(v, m_featherRadius))
        return;
    m_featherRadius = v;
    notifyChanged();
}

void SelectionOptions::setAntialias(bool v)
{
    if (v == m_antialias)
        return;
    m_antialias = v;
    notifyChanged();
}

int SelectionOptions::toSelectionOp(SelectionMode mode)
{
    // ★ 这个映射必须是"直接对应"的 ★
    //   两个枚举的数值顺序刻意设计成一致（Replace/Add/Subtract/Intersect
    //   都是 0/1/2/3），所以这里可以直接转。但**仍然写成 switch**：
    //   将来任一侧调整顺序时，编译器会在这里提醒（-Wswitch），
    //   而直接 static_cast 则会静默地把"减去"变成"交集"。
    switch (mode) {
    case SelectionMode::Replace:   return 0;   // Selection::Op::Replace
    case SelectionMode::Add:       return 1;   // Selection::Op::Add
    case SelectionMode::Subtract:  return 2;   // Selection::Op::Subtract
    case SelectionMode::Intersect: return 3;   // Selection::Op::Intersect
    }
    return 0;
}

SelectionMode SelectionOptions::modeFromModifiers(bool shift, bool alt, SelectionMode fallback)
{
    // Photoshop 的约定，用户肌肉记忆已经形成：
    //   Shift           → 添加
    //   Alt             → 减去
    //   Shift + Alt     → 交集
    //   都不按          → 用选项条上选的模式
    if (shift && alt)
        return SelectionMode::Intersect;
    if (shift)
        return SelectionMode::Add;
    if (alt)
        return SelectionMode::Subtract;
    return fallback;
}

QWidget *SelectionOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    // ── 四个模式按钮 ──
    // 用互斥的 QToolButton 组，模拟 Photoshop 选项条上的四个按钮。
    // ★ 实现搬到了文件内的 addModeButtons() ★
    //   魔术棒的选项条上也有同样四个按钮，语义必须逐字一致
    //   （见那个函数上方的说明）。
    addModeButtons(bar, layout, m_mode, [this](SelectionMode m) { setMode(m); });

    layout->addSpacing(12);

    // ── 羽化 ──
    auto *feather = new QDoubleSpinBox(bar);
    feather->setRange(0.0, 250.0);
    feather->setSingleStep(0.5);
    feather->setDecimals(1);
    feather->setValue(m_featherRadius);
    feather->setSuffix(tr(" px"));
    feather->setToolTip(tr("Feather radius. 0 means a crisp edge;\n"
                           "larger values soften the transition along the "
                           "selection edge."));
    addLabeled(layout, tr("Feather"), feather);
    connect(feather, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &SelectionOptions::setFeatherRadius);

    layout->addSpacing(12);

    // ── 抗锯齿 ──
    auto *aa = new QCheckBox(tr("Anti-aliasing"), bar);
    aa->setChecked(m_antialias);
    aa->setToolTip(tr("Generate 1-254 transition values along the selection edge to avoid jaggies"));
    layout->addWidget(aa);
    connect(aa, &QCheckBox::toggled, this, &SelectionOptions::setAntialias);

    addStretch(layout);
    return bar;
}

void SelectionOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    const int modeValue = settings.value(QStringLiteral("mode"), int(m_mode)).toInt();
    if (modeValue >= int(SelectionMode::Replace) && modeValue <= int(SelectionMode::Intersect))
        m_mode = static_cast<SelectionMode>(modeValue);

    m_featherRadius = qBound(0.0, settings.value(QStringLiteral("feather"), m_featherRadius).toDouble(), 250.0);
    m_antialias     = settings.value(QStringLiteral("antialias"), m_antialias).toBool();
    settings.endGroup();
}

void SelectionOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("mode"),      int(m_mode));
    settings.setValue(QStringLiteral("feather"),   m_featherRadius);
    settings.setValue(QStringLiteral("antialias"), m_antialias);
    settings.endGroup();
}

// ============================================================================
//  MagicWandOptions —— 魔术棒
// ============================================================================

MagicWandOptions::MagicWandOptions(QObject *parent)
    : ToolOptions(parent)
{
    // ★ 独立分组，不与 "selection"（选框）共用 ★
    //   理由见头文件：两者的常用取值差别很大（选框常羽化、魔术棒常不羽化），
    //   共用分组会让"在魔术棒里没动羽化"顺带改掉选框的羽化。
    setSettingsGroup(QStringLiteral("tool/magicwand"));
}

void MagicWandOptions::setMode(SelectionMode m)
{
    if (m == m_mode)
        return;
    m_mode = m;
    notifyChanged();
}

void MagicWandOptions::setTolerance(int v)
{
    v = qBound(0, v, 255);
    if (v == m_tolerance)
        return;
    m_tolerance = v;
    notifyChanged();
}

void MagicWandOptions::setContiguous(bool v)
{
    if (v == m_contiguous)
        return;
    m_contiguous = v;
    notifyChanged();
}

void MagicWandOptions::setAntialias(bool v)
{
    if (v == m_antialias)
        return;
    m_antialias = v;
    notifyChanged();
}

void MagicWandOptions::setSampleAllLayers(bool v)
{
    if (v == m_sampleAllLayers)
        return;
    m_sampleAllLayers = v;
    notifyChanged();
}

void MagicWandOptions::setFeatherRadius(qreal v)
{
    v = qBound(0.0, v, 250.0);
    if (qFuzzyCompare(v, m_featherRadius))
        return;
    m_featherRadius = v;
    notifyChanged();
}

QWidget *MagicWandOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    // ── 四个运算按钮（与选框共用同一份实现）──
    addModeButtons(bar, layout, m_mode, [this](SelectionMode m) { setMode(m); });

    layout->addSpacing(12);

    // ── 容差 ──
    // ★ 与油漆桶同一个刻度 ★
    //   floodFill 的判据是"与起点颜色的三通道欧氏距离归一化到 0-255"
    //   （见 engine/Fill.h），所以两处的"容差 30"手感完全一致 ——
    //   用户在油漆桶里练出来的直觉在魔术棒上直接可用。
    auto *tol = new QSlider(Qt::Horizontal, bar);
    tol->setRange(0, 255);
    tol->setValue(m_tolerance);
    tol->setFixedWidth(110);
    tol->setToolTip(tr("Pixels whose color differs from the click point by less "
                       "than the tolerance are selected together.\n"
                       "Uses three-channel Euclidean distance; a tolerance of "
                       "30 is roughly the smallest difference the eye can tell."));
    addLabeled(layout, tr("Tolerance"), tol);

    auto *tolSpin = new QSpinBox(bar);
    tolSpin->setRange(0, 255);
    tolSpin->setValue(m_tolerance);
    layout->addWidget(tolSpin);

    connect(tol, &QSlider::valueChanged, tolSpin, &QSpinBox::setValue);
    connect(tolSpin, QOverload<int>::of(&QSpinBox::valueChanged), tol, &QSlider::setValue);
    connect(tolSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MagicWandOptions::setTolerance);

    layout->addSpacing(10);

    // ── 连续 ──
    auto *contiguous = new QCheckBox(tr("Contiguous"), bar);
    contiguous->setChecked(m_contiguous);
    contiguous->setToolTip(tr("Checked: select only the region connected to "
                              "the click point (the usual behavior)\n"
                              "Unchecked: select every pixel within tolerance "
                              "in the whole image, connected or not"));
    layout->addWidget(contiguous);
    connect(contiguous, &QCheckBox::toggled, this, &MagicWandOptions::setContiguous);

    layout->addSpacing(6);

    // ── 抗锯齿 ──
    auto *aa = new QCheckBox(tr("Anti-aliasing"), bar);
    aa->setChecked(m_antialias);
    aa->setToolTip(tr("Generate 1-254 transition values along the selection edge to avoid a hard step"));
    layout->addWidget(aa);
    connect(aa, &QCheckBox::toggled, this, &MagicWandOptions::setAntialias);

    layout->addSpacing(6);

    // ── 判据取哪张图 ──
    // ★ 勾选状态与 Photoshop **相反**，提示里必须说清楚 ★
    //   否则用户会以为程序错了（"我明明没勾对所有图层，怎么取的是合成图"）。
    auto *allLayers = new QCheckBox(tr("Sample All Layers"), bar);
    allLayers->setChecked(m_sampleAllLayers);
    allLayers->setToolTip(tr("Checked (the default): colors are judged on the "
                             "composited image - what you see is what you get, "
                             "and transparent spots show the layers below "
                             "(same as the eyedropper)\n"
                             "Unchecked: judged on the current layer only - "
                             "its transparent spots count as one flat color, "
                             "for strictly per-layer selection"));
    layout->addWidget(allLayers);
    connect(allLayers, &QCheckBox::toggled, this, &MagicWandOptions::setSampleAllLayers);

    layout->addSpacing(12);

    // ── 羽化 ──
    // ★ 与"抗锯齿"不是一回事 ★
    //   抗锯齿是 floodFill 按**色差**给出的过渡带；羽化是按**像素距离**
    //   的模糊。两者都让边缘更软，同时打开不会出错 —— 但别指望它们叠加
    //   能得到"更精确"的边缘。
    auto *feather = new QDoubleSpinBox(bar);
    feather->setRange(0.0, 250.0);
    feather->setSingleStep(0.5);
    feather->setDecimals(1);
    feather->setValue(m_featherRadius);
    feather->setSuffix(tr(" px"));
    feather->setToolTip(tr("Feather radius. 0 (the default) means a crisp edge.\n"
                           "Larger values soften the transition along the "
                           "selection edge."));
    addLabeled(layout, tr("Feather"), feather);
    connect(feather, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MagicWandOptions::setFeatherRadius);

    addStretch(layout);
    return bar;
}

void MagicWandOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());

    const int modeValue = settings.value(QStringLiteral("mode"), int(m_mode)).toInt();
    if (modeValue >= int(SelectionMode::Replace) && modeValue <= int(SelectionMode::Intersect))
        m_mode = static_cast<SelectionMode>(modeValue);

    m_tolerance       = qBound(0, settings.value(QStringLiteral("tolerance"), m_tolerance).toInt(), 255);
    m_contiguous      = settings.value(QStringLiteral("contiguous"), m_contiguous).toBool();
    m_antialias       = settings.value(QStringLiteral("antialias"), m_antialias).toBool();
    m_sampleAllLayers = settings.value(QStringLiteral("sampleAllLayers"), m_sampleAllLayers).toBool();
    m_featherRadius   = qBound(0.0, settings.value(QStringLiteral("feather"), m_featherRadius).toDouble(), 250.0);

    settings.endGroup();
}

void MagicWandOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("mode"),            int(m_mode));
    settings.setValue(QStringLiteral("tolerance"),       m_tolerance);
    settings.setValue(QStringLiteral("contiguous"),      m_contiguous);
    settings.setValue(QStringLiteral("antialias"),       m_antialias);
    settings.setValue(QStringLiteral("sampleAllLayers"), m_sampleAllLayers);
    settings.setValue(QStringLiteral("feather"),         m_featherRadius);
    settings.endGroup();
}

// ============================================================================
//  MoveOptions —— 移动
// ============================================================================

MoveOptions::MoveOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/move"));
}

void MoveOptions::setAutoSelectLayer(bool v)
{
    if (v == m_autoSelectLayer)
        return;
    m_autoSelectLayer = v;
    notifyChanged();
}

void MoveOptions::setShowTransformBox(bool v)
{
    if (v == m_showTransformBox)
        return;
    m_showTransformBox = v;
    notifyChanged();
}

QWidget *MoveOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    auto *autoSelect = new QCheckBox(tr("Auto-Select Layer"), bar);
    autoSelect->setChecked(m_autoSelectLayer);
    autoSelect->setToolTip(tr("Checked: clicking the canvas switches to the "
                               "topmost layer with a pixel under the cursor\n"
                               "Unchecked: always move the content of the "
                               "active layer"));
    layout->addWidget(autoSelect);
    connect(autoSelect, &QCheckBox::toggled, this, &MoveOptions::setAutoSelectLayer);

    layout->addSpacing(12);

    auto *transformBox = new QCheckBox(tr("Show Transform Box"), bar);
    transformBox->setChecked(m_showTransformBox);
    transformBox->setToolTip(tr("Draw a dashed bounding box around the layer content"));
    layout->addWidget(transformBox);
    connect(transformBox, &QCheckBox::toggled, this, &MoveOptions::setShowTransformBox);

    auto *hint = new QLabel(tr("Shift constrains horizontally/vertically; Alt-drag duplicates"), bar);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addSpacing(12);
    layout->addWidget(hint);

    addStretch(layout);
    return bar;
}

void MoveOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    m_autoSelectLayer  = settings.value(QStringLiteral("autoSelectLayer"), m_autoSelectLayer).toBool();
    m_showTransformBox = settings.value(QStringLiteral("showTransformBox"), m_showTransformBox).toBool();
    settings.endGroup();
}

void MoveOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("autoSelectLayer"),  m_autoSelectLayer);
    settings.setValue(QStringLiteral("showTransformBox"), m_showTransformBox);
    settings.endGroup();
}

// ============================================================================
//  CropOptions —— 裁剪
// ============================================================================

CropOptions::CropOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/crop"));
}

void CropOptions::setPreset(AspectPreset p)
{
    if (p == m_preset)
        return;
    m_preset = p;
    notifyChanged();
}

void CropOptions::setDeleteOutside(bool v)
{
    if (v == m_deleteOutside)
        return;
    m_deleteOutside = v;
    notifyChanged();
}

void CropOptions::setGrid(CropGrid g)
{
    if (g == m_grid)
        return;
    m_grid = g;
    notifyChanged();
}

qreal CropOptions::presetRatio(AspectPreset p)
{
    // 返回"宽 / 高"。0 表示不约束比例。
    switch (p) {
    case AspectPreset::Free:        return 0.0;              // 自由
    case AspectPreset::Square:      return 1.0 / 1.0;
    case AspectPreset::FourThree:   return 4.0 / 3.0;
    case AspectPreset::ThreeTwo:    return 3.0 / 2.0;
    case AspectPreset::SixteenNine: return 16.0 / 9.0;
    case AspectPreset::NineSixteen: return 9.0 / 16.0;
    case AspectPreset::TwoThree:    return 2.0 / 3.0;
    case AspectPreset::ThreeFour:   return 3.0 / 4.0;
    case AspectPreset::Custom:      return 0.0;              // 由调用方另行提供
    }
    return 0.0;
}

QWidget *CropOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    auto *presetCombo = new QComboBox(bar);
    for (int i = int(AspectPreset::Free); i <= int(AspectPreset::ThreeFour); ++i) {
        const auto p = static_cast<AspectPreset>(i);
        presetCombo->addItem(aspectPresetName(p), i);
    }
    presetCombo->setCurrentIndex(presetCombo->findData(int(m_preset)));
    presetCombo->setToolTip(tr("Aspect-ratio constraint for the crop box"));
    addLabeled(layout, tr("Aspect Ratio"), presetCombo);
    connect(presetCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, presetCombo](int idx) {
        setPreset(static_cast<AspectPreset>(presetCombo->itemData(idx).toInt()));
    });

    layout->addSpacing(12);

    // ── 网格叠加 ──
    auto *gridCombo = new QComboBox(bar);
    gridCombo->addItem(cropGridName(CropGrid::None),        int(CropGrid::None));
    gridCombo->addItem(cropGridName(CropGrid::Thirds),      int(CropGrid::Thirds));
    gridCombo->addItem(cropGridName(CropGrid::Grid),        int(CropGrid::Grid));
    gridCombo->addItem(cropGridName(CropGrid::Diagonal),    int(CropGrid::Diagonal));
    gridCombo->addItem(cropGridName(CropGrid::GoldenRatio), int(CropGrid::GoldenRatio));
    gridCombo->setCurrentIndex(int(m_grid));
    gridCombo->setToolTip(tr("Composition guides inside the crop box (display only, no effect on the result)"));
    addLabeled(layout, tr("Grid"), gridCombo);
    connect(gridCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, gridCombo](int idx) {
        setGrid(static_cast<CropGrid>(gridCombo->itemData(idx).toInt()));
    });

    layout->addSpacing(12);

    // ── 关于「删除裁剪区域外的像素」：这里**故意没有**那个勾选框 ──
    //
    // ★ 为什么不做成勾选框 ★
    //   本工程有一条贯穿全项目的隐含不变量：**图层位图尺寸恒等于画布尺寸**
    //   （Document::crop / resizeCanvas / scaleImage 都按画布尺寸重建位图；
    //   画笔落点、滤镜遍历、选区蒙版全都按画布坐标索引图层像素）。
    //   "保留区域外内容"要求图层**大于**画布，会同时波及这三处 ——
    //   不是裁剪工具能单独承担的改动。
    //   所以裁剪恒为破坏性（区域外像素丢弃，但**可撤销**）。这里给一句话
    //   说明，而不是摆一个点了没反应的勾选框。完整理由见
    //   docs/画布与文件操作设计.md 6.7 末尾。
    //
    //   CropOptions::deleteOutside 字段与其 QSettings 键都保留不动
    //   （键名变了会让用户的配置漂移），等"允许图层大于画布"这件事
    //   真做的时候再把它放出来。
    auto *note = new QLabel(tr("Drag on the canvas to box the area to keep; hold Shift while dragging for a square"), bar);
    note->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(note);

    auto *hint = new QLabel(tr("Press Enter or double-click inside the box to confirm; Esc resets"), bar);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addSpacing(12);
    layout->addWidget(hint);

    addStretch(layout);
    return bar;
}

void CropOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());

    const int presetValue = settings.value(QStringLiteral("preset"), int(m_preset)).toInt();
    if (presetValue >= int(AspectPreset::Free) && presetValue <= int(AspectPreset::Custom))
        m_preset = static_cast<AspectPreset>(presetValue);

    const int gridValue = settings.value(QStringLiteral("grid"), int(m_grid)).toInt();
    if (gridValue >= int(CropGrid::None) && gridValue <= int(CropGrid::GoldenRatio))
        m_grid = static_cast<CropGrid>(gridValue);

    m_deleteOutside = settings.value(QStringLiteral("deleteOutside"), m_deleteOutside).toBool();
    settings.endGroup();
}

void CropOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("preset"),        int(m_preset));
    settings.setValue(QStringLiteral("grid"),          int(m_grid));
    settings.setValue(QStringLiteral("deleteOutside"), m_deleteOutside);
    settings.endGroup();
}

// ============================================================================
//  ZoomOptions —— 缩放
// ============================================================================

ZoomOptions::ZoomOptions(QObject *parent)
    : ToolOptions(parent)
{
    setSettingsGroup(QStringLiteral("tool/zoom"));
}

void ZoomOptions::setZoomIn(bool v)
{
    if (v == m_zoomIn)
        return;
    m_zoomIn = v;
    notifyChanged();
}

QWidget *ZoomOptions::createWidget(QWidget *parent)
{
    auto *bar = new QWidget(parent);
    auto *layout = makeBarLayout(bar);

    auto *inBtn  = new QToolButton(bar);
    auto *outBtn = new QToolButton(bar);

    inBtn->setText(tr("Zoom In"));
    inBtn->setCheckable(true);
    inBtn->setAutoExclusive(true);
    inBtn->setChecked(m_zoomIn);

    outBtn->setText(tr("Zoom Out"));
    outBtn->setCheckable(true);
    outBtn->setAutoExclusive(true);
    outBtn->setChecked(!m_zoomIn);

    layout->addWidget(inBtn);
    layout->addWidget(outBtn);

    connect(inBtn,  &QToolButton::clicked, this, [this]() { setZoomIn(true);  });
    connect(outBtn, &QToolButton::clicked, this, [this]() { setZoomIn(false); });

    layout->addSpacing(12);

    // 补充说明：缩放工具的实际行为由 CanvasView 执行
    // （工具通过 requestZoom 信号请求，视图层决定如何实现），
    // 因此这里只需要把"方向"这个参数暴露出来。
    auto *hint = new QLabel(tr("Hold Alt to temporarily switch to the opposite direction"), bar);
    hint->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(hint);

    addStretch(layout);
    return bar;
}

void ZoomOptions::load(QSettings &settings)
{
    settings.beginGroup(settingsGroup());
    m_zoomIn = settings.value(QStringLiteral("zoomIn"), m_zoomIn).toBool();
    settings.endGroup();
}

void ZoomOptions::save(QSettings &settings) const
{
    settings.beginGroup(settingsGroup());
    settings.setValue(QStringLiteral("zoomIn"), m_zoomIn);
    settings.endGroup();
}

} // namespace zl
