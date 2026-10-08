// ============================================================================
//  GridSettingsDialog.cpp —— "网格设置"对话框的实现
// ----------------------------------------------------------------------------
//  一个间距输入 + 一个颜色按钮 + "线宽恒定为 1 屏幕像素"的说明。
//
//  ★ 为什么写"线宽恒定为 1 屏幕像素"这句提示 ★
//    用户可能会奇怪"为什么我放大到 800% 网格线还是 1 像素细"。
//    这是**刻意的**（见 CanvasView::drawGrid 的 cosmetic pen），
//    不写清楚就会被当成 bug 报上来。
// ============================================================================

#include "ui/GridSettingsDialog.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace zl {

GridSettingsDialog::GridSettingsDialog(int spacing, const QColor &color, QWidget *parent)
    : QDialog(parent)
    , m_color(color.isValid() ? color : QColor(120, 120, 120, 110))
{
    setWindowTitle(tr("网格设置"));

    m_spin = new QSpinBox(this);
    // ★ 下界与 CanvasView::setGridSize 的 qBound(4, ...) 必须一致 ★
    //   两处不同的话，用户能在对话框里设一个画布会悄悄夹掉的值 ——
    //   表现是"我设了 2，确定后却变成 4"。所以下界只在这里与那里各写一次，
    //   改一处要记得另一处（已有一条自检钉住"对话框下界 == 画布夹取下界"）。
    m_spin->setRange(4, 2000);
    m_spin->setValue(qBound(4, spacing, 2000));
    m_spin->setSuffix(tr(" 像素"));
    m_spin->setToolTip(tr("网格间距，单位为图像像素。\n"
                          "放大画布时网格线不会变粗 —— 线宽恒定 1 个屏幕像素。"));

    m_colorButton = new QToolButton(this);
    m_colorButton->setFixedSize(48, 24);
    m_colorButton->setToolTip(tr("网格线颜色"));
    connect(m_colorButton, &QToolButton::clicked, this, &GridSettingsDialog::onPickColor);
    updateColorButton();

    auto *form = new QFormLayout;
    form->addRow(tr("间距(&S):"), m_spin);
    form->addRow(tr("颜色(&C):"), m_colorButton);

    auto *hint = new QLabel(
        tr("提示：网格线用于对齐参考，<b>不会</b>出现在导出的图片里。"), this);
    hint->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(hint);
    root->addStretch(1);
    root->addWidget(buttons);
}

int GridSettingsDialog::spacing() const
{
    return m_spin->value();
}

QColor GridSettingsDialog::color() const
{
    return m_color;
}

void GridSettingsDialog::onPickColor()
{
    const QColor picked = QColorDialog::getColor(
        m_color, this, tr("选择网格线颜色"),
        QColorDialog::ShowAlphaChannel);
    if (!picked.isValid())
        return;
    m_color = picked;
    updateColorButton();
}

void GridSettingsDialog::updateColorButton()
{
    // ★ 用 QPixmap 画色块，而不是 setStyleSheet("background:...") ★
    //   样式表写法要拼颜色字符串，alpha 还得自己转 rgba() —— 而且
    //   在某些平台风格下会被 QToolButton 的原生绘制盖掉。
    //   画一张小图当图标是最直白的做法，颜色与 alpha 都如实呈现。
    QPixmap pm(m_colorButton->size());
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    // 先铺一层棋盘，让半透明颜色能看清（与画布上的透明背景同源做法）
    const int cell = 6;
    for (int y = 0; y < pm.height(); y += cell) {
        for (int x = 0; x < pm.width(); x += cell) {
            const bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            p.fillRect(QRect(x, y, cell, cell),
                       dark ? QColor(180, 180, 180) : QColor(230, 230, 230));
        }
    }
    p.fillRect(pm.rect(), m_color);
    p.setPen(QPen(QColor(80, 80, 80), 1));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    p.end();

    m_colorButton->setIcon(QIcon(pm));
    m_colorButton->setIconSize(pm.size());
}

} // namespace zl