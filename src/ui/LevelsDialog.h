#pragma once

// ============================================================================
//  LevelsDialog.h —— 色阶对话框
// ----------------------------------------------------------------------------
//  组装三块东西：
//    · LevelsWidget   —— 直方图 + 五枚可拖动手柄
//    · 五个数值控件   —— 与手柄双向同步，便于精确输入
//    · 「自动」按钮   —— 调用 engine 的 computeAutoLevels()
//
//  ★ 自动色阶只接管输入黑白场，绝不动 gamma 与输出范围 ★
//
//    computeAutoLevels() 返回的是一份完整的 LevelsParams，其中 gamma = 1、
//    输出为 0..255。如果整份覆盖当前参数，用户手工调好的 gamma 会被静默
//    重置 —— 那就是"按了自动按钮，画面反而变了样"的经典来源。
//    这里只取它的 inBlack / inWhite 两项，其余保留用户当前设置。
//
//  ★ 数值控件与手柄的同步必须防重入 ★
//
//    给 QSpinBox setValue() 会触发 valueChanged → 回写参数 → 再去刷新
//    手柄与其它控件。虽然最终值相同（幂等），但会多绕几圈，而且容易在
//    "黑白场交叉"这类边界上绕出不一致。用 m_syncing 把整段同步包起来。
//    这与基类 addSliderRow() 里用 QSignalBlocker 挡反向信号是同一个思路。
//
//  ★ 交叉约束在本类里做，且只在数值框这一侧做 ★
//
//    拖动路径上的约束由 LevelsWidget 负责（它知道鼠标在哪）。
//    用户直接在数值框里敲"白场 = 10"而黑场还是 0 时，这里负责把白场顶到
//    黑场 +1。两处都做，但各管一条路径，不重叠、不冲突。
// ============================================================================

#include "ui/AdjustmentDialog.h"

class QDoubleSpinBox;
class QPushButton;
class QSpinBox;

namespace zl {

class LevelsWidget;

class LevelsDialog : public AdjustmentDialog
{
    Q_OBJECT

public:
    explicit LevelsDialog(const QImage &source, QWidget *parent = nullptr);

    QImage apply(const QImage &src) const override;
    bool   isChanged() const override;
    void   resetParams() override;

private slots:
    // 手柄拖动 / 键盘微调 → 刷数值框并请求预览
    void onLevelsChanged();

    // 任一数值框改动 → 回写控件
    void onSpinChanged();

    void onAutoClicked();

private:
    // 参数 → 数值框（单向，不回头触发 onSpinChanged）
    void pushParamsToSpins();

    LevelsWidget  *m_levels    = nullptr;
    QSpinBox      *m_inBlack   = nullptr;
    QDoubleSpinBox *m_gamma    = nullptr;
    QSpinBox      *m_inWhite   = nullptr;
    QSpinBox      *m_outBlack  = nullptr;
    QSpinBox      *m_outWhite  = nullptr;
    QPushButton   *m_autoButton = nullptr;

    // 同步期间为 true：onSpinChanged 直接返回，
    // 避免"程序设置数值框"被误当成"用户改了参数"。
    bool m_syncing = false;
};

} // namespace zl
