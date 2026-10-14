// ============================================================================
//  LevelsDialog.cpp
// ============================================================================

#include "ui/LevelsDialog.h"
#include "ui/LevelsWidget.h"

#include "engine/Adjust.h"
#include "engine/Histogram.h"
#include "engine/Lut.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace zl {

namespace {

// 与 engine 的 gamma 钳制范围一致（adjust::levelsLut）。
// 若这里放宽，手柄能拖到引擎不响应的地方 —— 表现为"画面上不动了"。
constexpr double kMinGamma = 0.1;
constexpr double kMaxGamma = 10.0;

// 数值框的统一宽度：五个框必须一样宽，否则用户在两行之间对比数值时
// 会不自觉地被错位的数字干扰。
constexpr int kBoxW = 64;

} // namespace

LevelsDialog::LevelsDialog(const QImage &source, QWidget *parent)
    : AdjustmentDialog(source, tr("Levels"), parent)
{
    auto *params = new QWidget(this);
    auto *outer = new QVBoxLayout(params);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(10);

    // ── 直方图与手柄 ──

    m_levels = new LevelsWidget(params);
    m_levels->setObjectName(QStringLiteral("levelsWidget"));

    // ★ 直方图只在这里算一次 ★
    //   对整层遍历一遍约 30ms（1200 万像素）。放在拖动路径上会直接
    //   毁掉交互手感；而它在这个对话框的整个生命周期内是常量。
    //   用亮度直方图而不是逐通道：色阶在这里是"亮度语义"的操作
    //   （RGB 复合通道），与 computeAutoLevels() 的统计口径一致。
    m_levels->setHistogram(histogram::luminance(source));

    connect(m_levels, &LevelsWidget::levelsChanged,
            this, &LevelsDialog::onLevelsChanged);
    connect(m_levels, &LevelsWidget::editingFinished,
            this, &LevelsDialog::onLevelsChanged);

    outer->addWidget(m_levels);

    // ── 数值区 ──
    //
    //   列布局（两行共用，保证上下两组数字严格对齐）：
    //         [       ]  [黑场]  [灰度系数]  [白场]
    //         [输入色阶]  [  0 ]  [ 1.00  ]  [255 ]
    //         [输出色阶]  [  0 ]  [       ]  [255 ]
    //                                        [自动]
    auto *grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);

    auto makeCaption = [this](const QString &text) {
        auto *l = new QLabel(text, this);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QStringLiteral("color: #666666;"));
        return l;
    };

    auto makeSpin = [this](const QString &name, int lo, int hi, int value,
                           const QString &tip) {
        auto *s = new QSpinBox(this);
        // ★ 给控件起名 ★
        //   自动化测试靠它定位控件。用 findChildren 的顺序定位看起来更省事，
        //   但一旦有人调整创建顺序（把"白场"挪到"黑场"前面），测试会静默
        //   张冠李戴地断言错对象 —— 那比没有测试更糟。
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setValue(value);
        s->setFixedWidth(kBoxW);
        s->setAlignment(Qt::AlignCenter);
        s->setToolTip(tip);
        // ★ 关掉键盘跟踪 ★
        //   默认开着时，用户输入"100"会依次触发 1、10、100 三次
        //   参数变化，也就重算三次预览。大图上能明显感到输入框发涩。
        s->setKeyboardTracking(false);
        return s;
    };

    // 黑场上限 254、白场下限 1：这样"黑场顶到白场 +1"永远有解，
    // 不需要再反向处理"白场已到 255 而黑场还要往右"的情形。
    m_inBlack = makeSpin(QStringLiteral("spinInBlack"), 0, 254, 0,
        tr("Input black point: input pixels below this value are crushed to pure black"));
    m_inWhite = makeSpin(QStringLiteral("spinInWhite"), 1, 255, 255,
        tr("Input white point: input pixels above this value are pulled to pure white"));

    m_gamma = new QDoubleSpinBox(this);
    m_gamma->setObjectName(QStringLiteral("spinGamma"));
    m_gamma->setRange(kMinGamma, kMaxGamma);
    m_gamma->setDecimals(2);
    m_gamma->setSingleStep(0.01);
    m_gamma->setValue(1.0);
    m_gamma->setFixedWidth(kBoxW);
    m_gamma->setAlignment(Qt::AlignCenter);
    m_gamma->setKeyboardTracking(false);
    m_gamma->setToolTip(tr("Gamma (midtones): above 1 brightens, below 1 "
                           "darkens.\n"
                           "You can also drag the mid-gray handle under the "
                           "histogram."));

    m_outBlack = makeSpin(QStringLiteral("spinOutBlack"), 0, 254, 0,
        tr("Output black point: the gray level the darkest result maps to"));
    m_outWhite = makeSpin(QStringLiteral("spinOutWhite"), 1, 255, 255,
        tr("Output white point: the gray level the brightest result maps to"));

    grid->addWidget(makeCaption(tr("Black")),     0, 1);
    grid->addWidget(makeCaption(tr("Gamma")), 0, 2);
    grid->addWidget(makeCaption(tr("White")),     0, 3);

    auto *inLabel  = new QLabel(tr("Input Levels"), this);
    auto *outLabel = new QLabel(tr("Output Levels"), this);

    grid->addWidget(inLabel,   1, 0);
    grid->addWidget(m_inBlack, 1, 1);
    grid->addWidget(m_gamma,   1, 2);
    grid->addWidget(m_inWhite, 1, 3);

    grid->addWidget(outLabel,    2, 0);
    grid->addWidget(m_outBlack,  2, 1);
    grid->addWidget(m_outWhite,  2, 3);

    grid->setColumnStretch(4, 1);

    outer->addLayout(grid);

    // ── 自动按钮 ──
    auto *autoRow = new QHBoxLayout;
    autoRow->addStretch(1);

    m_autoButton = new QPushButton(tr("Auto(&A)"), this);
    m_autoButton->setObjectName(QStringLiteral("buttonAuto"));
    m_autoButton->setToolTip(tr("Clips 0.1%% off each end of the histogram to "
                                "set the input black/white points "
                                "automatically.\n"
                                "Gamma and the output range are left "
                                "untouched."));
    connect(m_autoButton, &QPushButton::clicked, this, &LevelsDialog::onAutoClicked);
    autoRow->addWidget(m_autoButton);

    outer->addLayout(autoRow);

    // ── 挂到基类的整体布局上（补"预览 + 确定/取消/复位"）──
    buildLayout(params);

    // 初始参数（恒等）与数值框对齐。放在 buildLayout() 之后，
    // 那时基类的参数初始化标志已清空，行为才是"最终一致"的。
    pushParamsToSpins();

    resize(460, sizeHint().height());

    connect(m_inBlack,  QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LevelsDialog::onSpinChanged);
    connect(m_inWhite,  QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LevelsDialog::onSpinChanged);
    connect(m_outBlack, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LevelsDialog::onSpinChanged);
    connect(m_outWhite, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LevelsDialog::onSpinChanged);
    connect(m_gamma, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &LevelsDialog::onSpinChanged);
}

// ---------------------------------------------------------------------------

QImage LevelsDialog::apply(const QImage &src) const
{
    if (src.isNull())
        return src;

    // 参数等于"不变"时直接返回原图。
    // 这不是纯粹的性能优化：主窗口拖动时每帧都会调到这里，
    // 若初值状态下也要走一遍整层查表，"刚打开对话框"就会卡一下。
    //
    // ★ 返回 src 而不是 src.copy() ★
    //   返回值本身就是值语义，QImage 的隐式共享保证调用方一旦写入就会
    //   自动 detach —— 不会污染这里的原图。而 copy() 是每帧一次深拷贝
    //   （预览基准图约 10MB），白白拖慢交互。
    if (m_levels->isIdentity())
        return src;

    // "预乘 → 非预乘 → 查表 → 预乘"的往返由 applyLutRgb 内部处理
    // （见 Lut.h 的预乘陷阱说明），这里不需要也不应该再关心格式。
    return applyLutRgb(src, adjust::levelsLut(m_levels->params()));
}

bool LevelsDialog::isChanged() const
{
    return !m_levels->isIdentity();
}

void LevelsDialog::resetParams()
{
    // 复位期间基类会屏蔽 notifyChanged（见 AdjustmentDialog::onResetClicked），
    // 复位完成后统一发一次预览请求 —— 不会闪很多次。
    m_levels->setParams(adjust::LevelsParams());
    pushParamsToSpins();
}

// ---------------------------------------------------------------------------

void LevelsDialog::onLevelsChanged()
{
    pushParamsToSpins();
    notifyChanged();
}

void LevelsDialog::onSpinChanged()
{
    // 程序设置数值框时不回头改参数（见头文件的防重入说明）
    if (m_syncing)
        return;

    adjust::LevelsParams p = m_levels->params();
    p.inBlack  = m_inBlack->value();
    p.inWhite  = m_inWhite->value();
    p.gamma    = m_gamma->value();
    p.outBlack = m_outBlack->value();
    p.outWhite = m_outWhite->value();

    // ★ 黑白场必须保持严格的大小关系 ★
    //   引擎对交叉参数会做交换兜底（见 levelsLut 的注释），但那是最后一道
    //   防线：让用户看着画面整体反转发懵，不如在界面上就纠正。
    //   数值框的 range 已经把黑场上限设为 254、白场下限设为 1，
    //   所以"白场顶到黑场 + 1"永远落在合法区间内。
    if (p.inWhite <= p.inBlack)
        p.inWhite = qMin(255, p.inBlack + 1);
    if (p.outWhite <= p.outBlack)
        p.outWhite = qMin(255, p.outBlack + 1);

    m_levels->setParams(p);

    // 把（可能已被纠正的）值写回数值框：用户敲"输入白场 = 0"而黑场是 10 时，
    // 框里必须立刻显示 11 —— 否则界面在陈述一件与实际不符的事。
    pushParamsToSpins();

    notifyChanged();
}

void LevelsDialog::onAutoClicked()
{
    // ★ 只接管输入黑白场 ★
    //   见头文件说明：整份覆盖会把用户手工调好的 gamma 静默重置，
    //   表现成"按了自动按钮，画面反而变了样"。
    const adjust::LevelsParams autoP = adjust::computeAutoLevels(source());

    adjust::LevelsParams p = m_levels->params();
    p.inBlack = autoP.inBlack;
    p.inWhite = autoP.inWhite;

    // ★ 防御性兜底：自动结果也必须满足"黑场 < 白场" ★
    //
    //   引擎的 computeAutoLevels() 其实已经处理了黑白场重合的情形：
    //   整幅只有一个亮度值的图（纯色块、空白画布）会让两端分位数落在
    //   同一个桶里，此时它刻意返回"不变"而不是一对交叉参数
    //   （见 Adjust.cpp 的 `white <= black + 1` 兜底分支）。
    //
    //   这里再挡一道，因为交叉参数一旦进入界面状态，代价落在 UI 这边：
    //   灰点反查会退化成贴着白场、两枚手柄完全重叠而分不出谁是谁。
    //   UI 自己保证状态合法，比指望上游永不出错更可靠。
    if (p.inWhite <= p.inBlack)
        p.inWhite = qMin(255, p.inBlack + 1);

    m_levels->setParams(p);
    pushParamsToSpins();

    notifyChanged();
}

void LevelsDialog::pushParamsToSpins()
{
    const adjust::LevelsParams p = m_levels->params();

    m_syncing = true;
    m_inBlack->setValue(p.inBlack);
    m_inWhite->setValue(p.inWhite);
    m_gamma->setValue(p.gamma);
    m_outBlack->setValue(p.outBlack);
    m_outWhite->setValue(p.outWhite);
    m_syncing = false;
}

} // namespace zl
