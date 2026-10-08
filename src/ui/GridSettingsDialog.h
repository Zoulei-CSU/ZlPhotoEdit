#pragma once

// ============================================================================
//  GridSettingsDialog.h —— "网格设置"对话框（间距 + 线色）
// ----------------------------------------------------------------------------
//  ★ 为什么不只做预设、非要一个对话框 ★
//
//    预设（8/16/32/64/128/256）覆盖了九成场景，但排版里"每 100 像素一条线"
//    这种需求很常见，而 100 不在预设里。只给预设等于把这类用户挡在门外。
//    对话框还顺带给了"线色"——网格线压在照片上时，灰线可能看不清，
//    换个深色/浅色是刚需。
//
//  ★ 为什么对话框自己**不**改文档/画布 ★
//
//    它只负责"收集用户意愿"，落在哪个画布上由 MainWindow 决定。
//    理由与其它设置类对话框一致：多标签下"当前是哪个画布"只有 MainWindow
//    知道，对话框不该去猜（猜错就改到别的页上了）。
//    调用方读 spacing() / color() 之后再推给画布。
// ============================================================================

#include <QColor>
#include <QDialog>

class QSpinBox;
class QToolButton;

namespace zl {

class GridSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    // spacing   —— 当前网格间距（图像像素）
    // color     —— 当前网格线颜色
    explicit GridSettingsDialog(int spacing, const QColor &color,
                                QWidget *parent = nullptr);

    // 用户确认之后的取值。
    // ★ 只在 exec() 返回 Accepted 时读 ★ —— 取消时这些值没有意义。
    int    spacing() const;
    QColor color() const;

private slots:
    void onPickColor();

private:
    void updateColorButton();

    QSpinBox    *m_spin   = nullptr;
    QToolButton *m_colorButton = nullptr;

    QColor m_color;
};

} // namespace zl