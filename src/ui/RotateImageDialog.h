#pragma once

// ============================================================================
//  RotateImageDialog.h —— "任意角度旋转"对话框
// ----------------------------------------------------------------------------
//  与"顺时针 90°"那几个菜单项的差别：
//
//    · 90° 倍数  → 整数坐标重排，**无损**，画布与内容等大
//    · 任意角度  → 必然重采样，画布按包围盒**扩大**，四角空出来需要填充
//
//  正因为"任意角度"是**不可逆**的重采样操作（转 15° 再转 -15° 回不到
//  原图，只会多糊一次），本对话框必须有预览：用户得先看到结果再确认。
//
//  ★ 预览走主画布，而不是在对话框里画一张小图 ★
//
//    调整类对话框那套"降采样 + apply()"的通道正好合适 —— 主窗口把
//    降采样后的合成图交给 apply()，把结果贴回画布呈现。用户看到的是
//    **整幅图**在真实取景框里的样子，比自己画一张缩略图信息量大得多。
//
//    代价是旋转会改变画布尺寸，所以 CanvasView 的预览通道多了
//    "预览自带画布尺寸"这一维（见 CanvasView::setPreviewImage 的重载）。
//    否则用户看到的会是"旧取景框里塞进一个旋转过的图"—— 四角被裁掉，
//    而真实结果里那四角是填充区。
//
//  ★ apply() 只用于预览 ★
//
//    旋转的最终提交对象是**所有图层的位图 + 画布尺寸**，而 apply() 的
//    契约是"单张图 → 单张图"。所以确定时主窗口走的是
//    Document::rotateImageArbitrary()，而不是把 apply() 的结果塞回去。
//    这不影响"预览与结果一致"——两边调的都是 geometry::rotateImage()，
//    同一份实现、同一个角度、同一种填充色。
//
//  ★ source 传的是降采样图，不是全尺寸原图 ★
//
//    本对话框用不到全尺寸像素（apply() 只跑预览），传全尺寸等于白占
//    几十 MB。将来若要做"旋转后立即保存"之类的功能，再改这里。
// ============================================================================

#include "ui/AdjustmentDialog.h"

#include "core/ImageTypes.h"

#include <QColor>
#include <QSize>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSlider;

namespace zl {

class RotateImageDialog : public AdjustmentDialog
{
    Q_OBJECT

public:
    // canvasSize        —— 文档当前画布尺寸。用于算"旋转后画布多大"。
    // previewSource     —— 降采样后的合成图，apply() 的输入（见文件头说明）。
    // method            —— 重采样内核，任意角度时才会用到。
    // backgroundColor   —— 填充色选项里的"背景色"取自它（来自工具栏的底色）。
    RotateImageDialog(const QSize &canvasSize,
                      const QImage &previewSource,
                      ResampleMethod method = ResampleMethod::Bicubic,
                      const QColor &backgroundColor = Qt::white,
                      QWidget *parent = nullptr);
    ~RotateImageDialog() override;

    // ── 参数 ──

    // 顺时针角度，单位度，范围 (-180, 180]。
    qreal angle() const { return m_angle; }

    // 重采样内核。
    //
    // ★ 有意不提供界面选择 ★
    //   纯旋转时 applyGeometry() 里没有前置缩放步骤（sx = sy = 1），
    //   插值由 QPainter 的平滑变换统一负责 —— 也就是说把四项内核
    //   摆在下拉框里，用户无论选哪个，结果都一模一样。
    //   给一个"选了不生效"的控件，比不给更糟。
    //   （将来若做"旋转 + 同时缩放"，这里才需要暴露出来。）
    ResampleMethod method() const { return m_method; }

    // 扩展区域（旋转后四角）的填充色。透明 / 背景色 / 白 / 黑。
    QColor fillColor() const;

    // ★ 旋转后的画布尺寸 ★
    //   与确定时 Document 内部的算法是**同一个函数**（rotatedBoundsSize），
    //   杜绝"对话框说 5320×4120、确定后却是 5321×4120"这类分叉。
    QSize newCanvasSize() const;

    // ── AdjustmentDialog 接口 ──

    QImage apply(const QImage &src) const override;
    bool isChanged() const override;
    void resetParams() override;

    // ── 供测试定位（不参与业务逻辑）──

    QSlider        *angleSlider()  const { return m_angleSlider; }
    QDoubleSpinBox *angleSpinBox() const { return m_angleBox; }
    QComboBox      *fillComboBox() const { return m_fillCombo; }
    QLabel         *infoLabel()    const { return m_infoLabel; }

private slots:
    // 角度变化后统一走这里：刷新信息面板 + 请求预览。
    void onAngleChanged();
    void onFillModeChanged(int index);

private:
    QString infoText() const;

    QSize          m_canvasSize;
    ResampleMethod m_method;
    QColor         m_backgroundColor;
    qreal          m_angle = 0.0;

    QSlider        *m_angleSlider = nullptr;
    QDoubleSpinBox *m_angleBox    = nullptr;
    QLabel         *m_infoLabel   = nullptr;
    QComboBox      *m_fillCombo   = nullptr;
};

} // namespace zl
