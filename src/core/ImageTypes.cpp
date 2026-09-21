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
    case BlendMode::Normal:      return QObject::tr("正常");
    case BlendMode::Dissolve:    return QObject::tr("溶解");
    case BlendMode::Darken:      return QObject::tr("变暗");
    case BlendMode::Multiply:    return QObject::tr("正片叠底");
    case BlendMode::ColorBurn:   return QObject::tr("颜色加深");
    case BlendMode::LinearBurn:  return QObject::tr("线性加深");
    case BlendMode::Lighten:     return QObject::tr("变亮");
    case BlendMode::Screen:      return QObject::tr("滤色");
    case BlendMode::ColorDodge:  return QObject::tr("颜色减淡");
    case BlendMode::LinearDodge: return QObject::tr("线性减淡");
    case BlendMode::Overlay:     return QObject::tr("叠加");
    case BlendMode::SoftLight:   return QObject::tr("柔光");
    case BlendMode::HardLight:   return QObject::tr("强光");
    case BlendMode::VividLight:  return QObject::tr("亮光");
    case BlendMode::LinearLight: return QObject::tr("线性光");
    case BlendMode::PinLight:    return QObject::tr("点光");
    case BlendMode::Difference:  return QObject::tr("差值");
    case BlendMode::Exclusion:   return QObject::tr("排除");
    case BlendMode::Hue:         return QObject::tr("色相");
    case BlendMode::Saturation:  return QObject::tr("饱和度");
    case BlendMode::Color:       return QObject::tr("颜色");
    case BlendMode::Luminosity:  return QObject::tr("明度");
    }
    return QObject::tr("正常");
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
    case ResampleMethod::Nearest:  return QObject::tr("邻近（硬边缘）");
    case ResampleMethod::Bilinear: return QObject::tr("两次线性");
    case ResampleMethod::Bicubic:  return QObject::tr("两次立方（平滑，推荐）");
    case ResampleMethod::Lanczos:  return QObject::tr("Lanczos（最锐利）");
    }
    return QObject::tr("两次立方（平滑，推荐）");
}

} // namespace zl
