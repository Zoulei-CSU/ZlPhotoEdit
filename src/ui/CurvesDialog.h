#pragma once

// ============================================================================
//  CurvesDialog.h —— 曲线调整对话框
// ----------------------------------------------------------------------------
//  组装三块东西：
//    · CurveEditor —— 控制点编辑与曲线绘制
//    · 通道选择   —— RGB 主通道 / 红 / 绿 / 蓝各一条独立曲线
//    · 背景直方图 —— 当前通道的分布，让用户"有依据地"调曲线而不是盲调
//
//  ★ 四条曲线是"叠乘"关系，不是四条各自独立作用 ★
//
//      最终映射 = curveBlue(curveGreen(curveRed(curveRgb(x))))
//
//    合成由 engine 的 CurvesSettings::bakeLuts() 完成（会先把四条曲线
//    压成三张表，逐像素只查一次）。本对话框只负责维护四条曲线的控制点，
//    不做任何插值或合成 —— 一旦在 UI 里自己算一遍，就会出现
//    "界面上看着是 A、实际结果是 B"这类最难查的问题。
//
//  ★ 直方图只在切换通道时算 ★
//
//    对整层遍历一遍约 30ms（1200 万像素）。放在滑块拖动路径上会直接
//    毁掉交互手感，所以按通道缓存，算过就不再算。
// ============================================================================

#include "ui/AdjustmentDialog.h"
#include "engine/Curves.h"

#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;

namespace zl {

class CurveEditor;

class CurvesDialog : public AdjustmentDialog
{
    Q_OBJECT

public:
    explicit CurvesDialog(const QImage &source, QWidget *parent = nullptr);

    QImage apply(const QImage &src) const override;
    bool   isChanged() const override;
    void   resetParams() override;

private slots:
    void onChannelChanged(int index);
    void onPresetActivated(int index);
    void onSmoothToggled(bool on);

    // 编辑器的控制点变化 → 立刻写回 m_settings
    // ★ 必须"实时回写"而不是"确定时再读" ★
    //   apply() 可能被主窗口在任意时刻调用（拖动中每帧一次），
    //   如果那时 m_settings 还是旧值，预览就会慢一拍。
    void onEditorChanged();

private:
    void pushCurveToEditor();                  // m_settings → 编辑器
    QColor channelColor(CurveChannel ch) const;
    const QVector<int> &histogramFor(CurveChannel ch);

    CurveEditor *m_editor       = nullptr;
    QComboBox   *m_channelBox   = nullptr;
    QComboBox   *m_presetBox    = nullptr;
    QCheckBox   *m_smoothCheck  = nullptr;
    QLabel      *m_hintLabel    = nullptr;

    CurvesSettings m_settings;
    CurveChannel   m_channel = CurveChannel::Rgb;

    // 按通道缓存的直方图。索引直接对应 CurveChannel 的取值（0..3）。
    QVector<int> m_histCache[int(CurveChannel::Count)];
    bool         m_histValid[int(CurveChannel::Count)] = { false, false, false, false };
};

} // namespace zl
