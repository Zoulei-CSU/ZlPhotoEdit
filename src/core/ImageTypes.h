#pragma once

// ============================================================================
//  ImageTypes.h —— 跨层共享的基础类型定义
// ----------------------------------------------------------------------------
//  这些枚举被 core / engine / commands / tools / ui 各层共用，因此放在一个
//  独立的、只依赖 QtGui 的头文件里，而不是塞进某个具体类。
//
//  注意：本文件属于 core 层，严禁 include 任何 QtWidgets 头文件。
// ============================================================================

#include <QImage>
#include <QString>
#include <QMetaType>

namespace zl {

// ---------------------------------------------------------------------------
//  图层混合模式
// ---------------------------------------------------------------------------
//  与 Photoshop 的 16 种常用模式对齐。数值不写死为 0,1,2...，
//  而是留出间隔，方便后续插入新模式而不破坏已序列化的数据。
// ---------------------------------------------------------------------------
enum class BlendMode : int {
    Normal      = 0,    // 正常
    Dissolve    = 1,    // 溶解（需要随机数，暂不实现，保留位置）
    Darken      = 10,   // 变暗
    Multiply    = 11,   // 正片叠底
    ColorBurn   = 12,   // 颜色加深
    LinearBurn  = 13,   // 线性加深
    Lighten     = 20,   // 变亮
    Screen      = 21,   // 滤色
    ColorDodge  = 22,   // 颜色减淡
    LinearDodge = 23,   // 线性减淡（相加）
    Overlay     = 30,   // 叠加
    SoftLight   = 31,   // 柔光
    HardLight   = 32,   // 强光
    VividLight  = 33,   // 亮光
    LinearLight = 34,   // 线性光
    PinLight    = 35,   // 点光
    Difference  = 40,   // 差值
    Exclusion   = 41,   // 排除
    Hue         = 50,   // 色相
    Saturation  = 51,   // 饱和度
    Color       = 52,   // 颜色
    Luminosity  = 53    // 明度
};

// 混合模式的中文显示名。用于图层面板的下拉框。
QString blendModeName(BlendMode mode);

// 全部可选混合模式（按图层面板下拉框的展示顺序）。
// 返回 QList 而非数组，避免使用方需要知道元素个数。
QList<BlendMode> allBlendModes();

// ---------------------------------------------------------------------------
//  重采样算法
// ---------------------------------------------------------------------------
//  缩放图像时的插值方式。Qt 只自带 Nearest 与 Bilinear，Bicubic 与 Lanczos
//  由本工程在 engine/Resample.cpp 中自行实现。
// ---------------------------------------------------------------------------
enum class ResampleMethod : int {
    Nearest  = 0,   // 邻近取样：最快，马赛克感明显，适合像素画放大
    Bilinear = 1,   // 双线性：快，但缩小会有锯齿，适合作为拖动时的预览算法
    Bicubic  = 2,   // 双三次（Keys 三次卷积，a = -0.5）：质量与速度的平衡点，**默认**
    Lanczos  = 3    // Lanczos（a = 3）：最锐利，但可能产生轻微振铃
};

QString resampleMethodName(ResampleMethod method);

// ---------------------------------------------------------------------------
//  图像格式信息
// ---------------------------------------------------------------------------
//  打开 / 导出时用到的一组元数据。集中放这里，避免 Document 头文件被拖长。
// ---------------------------------------------------------------------------
struct ImageFormatInfo {
    int     width      = 0;
    int     height     = 0;
    int     dpiX       = 72;    // 水平物理分辨率
    int     dpiY       = 72;
    QString formatName;         // 如 "PNG"、"JPEG"
};

} // namespace zl

// 让这些枚举能塞进 QVariant（撤销命令的参数传递会用到）
Q_DECLARE_METATYPE(zl::BlendMode)
Q_DECLARE_METATYPE(zl::ResampleMethod)
