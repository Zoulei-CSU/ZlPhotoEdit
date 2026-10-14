#include "core/ImageTypes.h"

#include <QObject>
#include <QList>

namespace zl {

// ---------------------------------------------------------------------------
//  混合模式的中文名
// ---------------------------------------------------------------------------
//  用 QObject::tr() 而不是 QCoreApplication::translate()，是为了让 Qt 的
//  lupdate 工具能扫出这些字符串。即使当前不做多语言，先把 tr() 加上，
//  将来要加语言支持时不用回头翻代码找硬编码字符串。
// ---------------------------------------------------------------------------
QString blendModeName(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Normal:      return QObject::tr("Normal");
    case BlendMode::Dissolve:    return QObject::tr("Dissolve");
    case BlendMode::Darken:      return QObject::tr("Darken");
    case BlendMode::Multiply:    return QObject::tr("Multiply");
    case BlendMode::ColorBurn:   return QObject::tr("Color Burn");
    case BlendMode::LinearBurn:  return QObject::tr("Linear Burn");
    case BlendMode::Lighten:     return QObject::tr("Lighten");
    case BlendMode::Screen:      return QObject::tr("Screen");
    case BlendMode::ColorDodge:  return QObject::tr("Color Dodge");
    case BlendMode::LinearDodge: return QObject::tr("Linear Dodge (Add)");
    case BlendMode::Overlay:     return QObject::tr("Overlay");
    case BlendMode::SoftLight:   return QObject::tr("Soft Light");
    case BlendMode::HardLight:   return QObject::tr("Hard Light");
    case BlendMode::VividLight:  return QObject::tr("Vivid Light");
    case BlendMode::LinearLight: return QObject::tr("Linear Light");
    case BlendMode::PinLight:    return QObject::tr("Pin Light");
    case BlendMode::Difference:  return QObject::tr("Difference");
    case BlendMode::Exclusion:   return QObject::tr("Exclusion");
    case BlendMode::Hue:         return QObject::tr("Hue");
    case BlendMode::Saturation:  return QObject::tr("Saturation");
    case BlendMode::Color:       return QObject::tr("Color");
    case BlendMode::Luminosity:  return QObject::tr("Luminosity");
    }
    return QObject::tr("Normal");
}

QList<BlendMode> allBlendModes()
{
    // 顺序与 Photoshop 图层面板下拉框基本一致：
    // 先是"暗"组，再是"亮"组，然后是"对比"组，最后是"分量"组。
    // 分组展示比按枚举值排更符合用户心智。
    return {
        BlendMode::Normal,
        BlendMode::Dissolve,

        BlendMode::Darken,
        BlendMode::Multiply,
        BlendMode::ColorBurn,
        BlendMode::LinearBurn,

        BlendMode::Lighten,
        BlendMode::Screen,
        BlendMode::ColorDodge,
        BlendMode::LinearDodge,

        BlendMode::Overlay,
        BlendMode::SoftLight,
        BlendMode::HardLight,
        BlendMode::VividLight,
        BlendMode::LinearLight,
        BlendMode::PinLight,

        BlendMode::Difference,
        BlendMode::Exclusion,

        BlendMode::Hue,
        BlendMode::Saturation,
        BlendMode::Color,
        BlendMode::Luminosity
    };
}

QString resampleMethodName(ResampleMethod method)
{
    switch (method) {
    case ResampleMethod::Nearest:  return QObject::tr("Nearest (Hard Edges)");
    case ResampleMethod::Bilinear: return QObject::tr("Bilinear");
    case ResampleMethod::Bicubic:  return QObject::tr("Bicubic (Smooth, Recommended)");
    case ResampleMethod::Lanczos:  return QObject::tr("Lanczos (Sharpest)");
    }
    return QObject::tr("Bicubic (Smooth, Recommended)");
}

} // namespace zl
