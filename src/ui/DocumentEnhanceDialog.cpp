// ============================================================================
//  DocumentEnhanceDialog.cpp
// ============================================================================

#include "ui/DocumentEnhanceDialog.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace zl {

DocumentEnhanceDialog::DocumentEnhanceDialog(const QImage &source, QWidget *parent)
    : AdjustmentDialog(source, tr("Document Enhancement"), parent)
{
    auto *params = new QWidget(this);
    auto *grid = new QGridLayout(params);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(8);

    // 三个参数都取 [0,100]，默认 50。
    //
    // ★ 为什么默认值全是 50 而不是 0 ★
    //   0 在这里不是"不变"而是"关掉这个功能"：阴影 0 等于不平，
    //   文字 0 等于不提黑，提白 0 等于不去白。用户打开这个对话框，
    //   期望的是"立刻看到一张更干净的纸"，而不是"什么都没发生"。
    //   50 是三个维度都做到"明显但不过分"的折中值。
    m_shadow   = addSliderRow(grid, 0, tr("Remove Shadow"), 0, 100, 50);
    m_contrast = addSliderRow(grid, 1, tr("Darken Text"),   0, 100, 50);
    m_whiten   = addSliderRow(grid, 2, tr("Whiten Paper"),  0, 100, 50);

    m_gray = new QCheckBox(tr("Convert to black && white"), this);
    m_gray->setChecked(false);
    grid->addWidget(m_gray, 3, 0, 1, 3);

    // 勾选状态变化同样要触发预览。addSliderRow 只连了滑块，
    // 复选框得自己接 —— 漏了它的症状是"勾了黑白，预览纹丝不动"。
    connect(m_gray, &QCheckBox::toggled, this, [this](bool) { notifyChanged(); });

    grid->setColumnStretch(1, 1);

    buildLayout(params);
    resize(460, sizeHint().height());
}

scan::EnhanceParams DocumentEnhanceDialog::params() const
{
    scan::EnhanceParams p;
    p.shadow    = m_shadow->value();
    p.contrast  = m_contrast->value();
    p.whiten    = m_whiten->value();
    p.grayscale = m_gray->isChecked();
    return p;
}

QImage DocumentEnhanceDialog::apply(const QImage &src) const
{
    if (src.isNull())
        return src;

    // 引擎内部自己处理"预乘 ↔ 非预乘"的往返（见 DocumentScan.cpp 的说明），
    // 这里不需要也不应该再关心格式。
    return scan::enhance(src, params());
}

bool DocumentEnhanceDialog::isChanged() const
{
    // ★ 这里与别的调整对话框**不一样**：永远返回 true ★
    //
    //   别的对话框默认参数 = 恒等变换（亮度 0、对比度 0、色阶 0~255），
    //   "什么都没动就点确定"确实等于什么都没做，所以它们返回 false 来
    //   避免在历史面板里留空记录。
    //
    //   本对话框的默认参数（50/50/50）是**一次真实的增强**：它的作用就是
    //   "把这张纸变白、字变黑"。用户打开它、看到预览变干净了、直接点确定，
    //   期望的是"这个效果被应用"，而不是"什么都没发生"。
    //
    //   若照抄别处的写法，症状会非常费解：预览里纸是白的，一点确定
    //   又变回灰的 —— 而且没有任何提示。
    return true;
}

void DocumentEnhanceDialog::resetParams()
{
    const scan::EnhanceParams def;
    m_shadow->setValue(def.shadow);
    m_contrast->setValue(def.contrast);
    m_whiten->setValue(def.whiten);
    m_gray->setChecked(def.grayscale);
}

} // namespace zl
