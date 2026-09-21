// ============================================================================
//  AdjustmentDialog.cpp —— 调整对话框基类的实现
// ============================================================================

#include "ui/AdjustmentDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>

namespace zl {

AdjustmentDialog::AdjustmentDialog(const QImage &source, const QString &title, QWidget *parent)
    : QDialog(parent)
    , m_source(source)
{
    setWindowTitle(title);

    // ★ 不做原生对话框 ★
    //   Windows 的原生对话框会套用系统主题，与主窗口的中性灰界面
    //   割裂得很明显；更重要的是原生对话框不支持我们对预览行为的
    //   精细控制（例如点关闭按钮时的返回值语义）。
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    // 调整对话框不需要"最大化"，固定为可调整尺寸即可
    setSizeGripEnabled(true);
}

AdjustmentDialog::~AdjustmentDialog() = default;

bool AdjustmentDialog::isPreviewEnabled() const
{
    // 勾选框是在 buildLayout() 里创建的；理论上子类构造完就存在了，
    // 但为了防御"子类忘了调 buildLayout"这种写法，这里兜底返回 true，
    // 让预览仍然工作（而不是静默什么都不发生）。
    return m_previewCheck ? m_previewCheck->isChecked() : true;
}

void AdjustmentDialog::buildLayout(QWidget *paramArea)
{
    auto *main = new QVBoxLayout(this);
    main->addWidget(paramArea, 1);

    m_previewCheck = new QCheckBox(tr("预览(&P)"), this);
    m_previewCheck->setChecked(true);
    m_previewCheck->setToolTip(tr("取消勾选可以临时看到未调整前的画面，"
                                  "便于对比（不会丢失当前参数）"));
    connect(m_previewCheck, &QCheckBox::toggled, this, &AdjustmentDialog::onPreviewToggled);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));

    m_resetButton = buttons->addButton(tr("复位(&R)"), QDialogButtonBox::ResetRole);
    connect(m_resetButton, &QPushButton::clicked, this, &AdjustmentDialog::onResetClicked);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_previewCheck);
    bottom->addStretch(1);
    bottom->addWidget(buttons);
    main->addLayout(bottom);

    // 参数初始化阶段结束。之后任何控件变化都会被当作"用户操作"。
    m_loadingParams = false;
}

QSlider *AdjustmentDialog::addSliderRow(QGridLayout *grid, int row,
                                       const QString &label, int lo, int hi, int value,
                                       const QString &suffix)
{
    auto *name = new QLabel(label, this);

    auto *slider = new QSlider(Qt::Horizontal, this);
    slider->setRange(lo, hi);
    slider->setValue(value);
    // 给一个最小宽度，否则不同对话框的滑块长度会随标签长短变化，
    // 在几个调整对话框之间来回切换时会觉得界面在"抖"。
    slider->setMinimumWidth(220);

    auto *box = new QSpinBox(this);
    box->setRange(lo, hi);
    box->setValue(value);
    if (!suffix.isEmpty())
        box->setSuffix(suffix);

    grid->addWidget(name,   row, 0);
    grid->addWidget(slider, row, 1);
    grid->addWidget(box,    row, 2);

    // ── 滑块 ⇄ 数字框 双向同步 ──
    //
    // ★ 必须用 QSignalBlocker 挡住反向信号 ★
    //   slider→box→slider 会形成回路。虽然 Qt 的 setValue 在值不变时
    //   不再发信号，回路会在第二步自然终止，但 notifyChanged() 会被
    //   调用**两次** —— 每拖动一格就白白多算一遍预览。
    //   挡住被同步的那一侧，让"一次用户操作只产生一次通知"。
    connect(slider, &QSlider::valueChanged, this, [this, box](int v) {
        const QSignalBlocker block(box);
        box->setValue(v);
        notifyChanged();
    });
    connect(box, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, slider](int v) {
        const QSignalBlocker block(slider);
        slider->setValue(v);
        notifyChanged();
    });

    return slider;
}

void AdjustmentDialog::notifyChanged()
{
    if (m_loadingParams)
        return;

    // 预览关闭时也要发信号 —— 主窗口据此把画布切回"未调整"的画面，
    // 否则关掉预览后画面会停在上一次的预览结果上，看起来像没生效。
    emit previewNeeded();
}

void AdjustmentDialog::onResetClicked()
{
    // ★ 复位期间要进入静默状态 ★
    //   resetParams() 会把每个控件逐个设回初值，中途会发很多次
    //   valueChanged。若不加保护，用户在"色相"这一项上会看到预览
    //   闪好几次（每次只复位了一部分参数）。
    m_loadingParams = true;
    resetParams();
    m_loadingParams = false;

    // 复位完成后统一通知一次
    emit previewNeeded();
}

void AdjustmentDialog::onPreviewToggled(bool /*on*/)
{
    // 勾选与取消勾选都只是"要不要显示预览"，
    // 参数本身保持不变 —— 所以统一发一次通知，由主窗口决定画什么。
    emit previewNeeded();
}

} // namespace zl
