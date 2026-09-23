#pragma once

// ============================================================================
//  Blend.h —— 图层混合模式算法
// ----------------------------------------------------------------------------
//  engine 层契约（必须遵守）：
//    · 全是**无副作用的纯函数**：输入 QImage + 参数 → 返回新 QImage
//    · **严禁 include 任何 QtWidgets 头文件** —— 本层必须能脱离界面独立测试
//    · 不做内存分配失败之外的任何错误处理，由调用方检查结果 isNull()
//
//  关键：本文件的所有运算都定义在【预乘 alpha 空间】。
//
//  这与 Lut.cpp 的做法正好相反，必须区分清楚：
//    · 混合模式（本文件）—— 运算本来就定义在预乘空间，保持预乘格式**不做转换**。
//      预乘空间里 RGB 已经乘过 alpha，混合公式天然正确且省一次乘法。
//    · LUT 查表调整（Lut.cpp）—— 查表是按原始 RGB 值算的，若在预乘空间
//      直接查表，半透明像素会查到一个"已经被 alpha 压暗"的值，导致色偏。
//      因此必须在 Lut 内部做非预乘往返。
//
//  把这条区分写进头文件，是因为它是本项目最容易搞错、且症状最隐蔽的地方：
//  错误不会崩溃，只会在半透明区域出现轻微色偏，人眼很难判断是 bug 还是原图如此。
// ============================================================================

#include "core/ImageTypes.h"

#include <QImage>

namespace zl {
namespace blend {

// ---------------------------------------------------------------------------
//  单像素混合（标量版本，供参考与测试）
// ---------------------------------------------------------------------------
//  base 与 blend 均为 [0,1] 范围的归一化值，返回混合结果。
//  这些函数只做"混合公式"本身，不涉及 alpha 合成 —— alpha 合成由
//  compositeLayer() 统一处理。
// ---------------------------------------------------------------------------

// 每个混合模式的公式。命名与 Photoshop 一致，便于对照文档。
qreal blendNormal     (qreal base, qreal blend);
qreal blendDarken     (qreal base, qreal blend);
qreal blendMultiply   (qreal base, qreal blend);
qreal blendColorBurn  (qreal base, qreal blend);
qreal blendLinearBurn (qreal base, qreal blend);
qreal blendLighten    (qreal base, qreal blend);
qreal blendScreen     (qreal base, qreal blend);
qreal blendColorDodge (qreal base, qreal blend);
qreal blendLinearDodge(qreal base, qreal blend);
qreal blendOverlay    (qreal base, qreal blend);
qreal blendSoftLight  (qreal base, qreal blend);
qreal blendHardLight  (qreal base, qreal blend);
qreal blendVividLight (qreal base, qreal blend);
qreal blendLinearLight(qreal base, qreal blend);
qreal blendPinLight   (qreal base, qreal blend);
qreal blendDifference (qreal base, qreal blend);
qreal blendExclusion  (qreal base, qreal blend);

// 统一的标量入口：按 mode 分派到上面的具体实现。
// 不直接在批量函数里内联 switch，是为了让单元测试可以逐个验证公式。
qreal blendChannel(BlendMode mode, qreal base, qreal blend);

// ---------------------------------------------------------------------------
//  批量混合（实际使用的主入口）
// ---------------------------------------------------------------------------

// 把 blend 图层按其混合模式与不透明度合成到 base 上，返回新图像。
//
// 参数：
//   base        —— 下层图像（预乘格式）。不会被修改。
//   blend       —— 上层图像（预乘格式）。尺寸若与 base 不同，按 base 尺寸处理，
//                  超出部分裁掉、不足部分视为全透明。
//   mode        —— 混合模式
//   opacity     —— 图层不透明度，0.0 ~ 1.0
//
// 返回值：合成结果（预乘格式）。base 或 blend 为空时返回 base 的副本。
//
// 注意色相/饱和度/颜色/明度这四种"分量模式"需要 RGB↔HSL 转换，
// 由本函数内部处理，调用方无需关心。
//
// fastPixelsOut —— 可选诊断输出（默认不传）：本次调用中"走了
//   『完全不透明上层直接覆盖』快路径"的像素个数（**按层累加**，
//   所以两层各覆盖一遍整张图时会给出 2×像素数，不是算重了）。
//   产品代码一律不传它。
//   存在的唯一理由是自检：快路径的产物与慢路径**逐位相同**，
//   于是"结果正确"根本证明不了快路径被走到了 —— 只能靠计数。
//   （同理见 docs/性能优化评估.md §4.5、铁律 §20 的遥测纪律）
QImage compositeLayer(const QImage &base, const QImage &blend,
                      BlendMode mode, qreal opacity,
                      quint64 *fastPixelsOut = nullptr);

// ---------------------------------------------------------------------------
//  整组图层合成
// ---------------------------------------------------------------------------

// 把一个图层栈按从底到顶的顺序合成到一张图上。
// 这是 Document::composite() 的实现主体，独立出来是为了能单独测试。
//
// 参数为三个等长列表（索引 0 = 最底部），由 Document 组装后传入：
//   images    —— 各图层位图
//   modes     —— 各图层的混合模式
//   opacities —— 各图层的不透明度（0.0~1.0）
//   visible   —— 各图层是否可见
//   bounds    —— 输出画布尺寸
//   background—— 初始底色（通常为透明）
QImage compositeLayers(const QList<QImage> &images,
                       const QList<BlendMode> &modes,
                       const QList<qreal> &opacities,
                       const QList<bool> &visible,
                       const QSize &bounds,
                       const QColor &background = Qt::transparent);

// ---------------------------------------------------------------------------
//  整组图层合成（只算一个矩形）★ 性能关键 ★
// ---------------------------------------------------------------------------

// 与 compositeLayers 语义**完全相同**，但只计算 region 内的结果，
// 返回 region.size() 大小的图（region 已与画布求交；region 为空则返回空图）。
//
// ★ 为什么"只算一块"能保证结果一模一样，而不是近似 ★
//   本文件全部 16 种混合模式都是**逐像素（pointwise）**运算 ——
//   每个输出像素只依赖**相同坐标**的输入像素，没有任何空间邻域运算
//   （无模糊、无位移、无卷积、无形态学）。因此
//       "某矩形内独立合成的结果" ≡ "全图合成结果的对应子区域"
//   是**逐位相等**。这条是整个脏矩形优化的正确性基石，
//   由 tests/perf_test.cpp 对 16 种模式逐一钉死。
//
// ★ 用法约束 ★
//   region 必须已与 QRect(QPoint(0,0), bounds) 求交。传入越界矩形会被
//   这里裁掉（不会越界写入），但**脏矩形算小了会留下旧内容的残影** ——
//   那种 bug 不崩不报，只能靠自检抓。
//
// ★ 实现约束 ★
//   必须复用 compositeLayer() 这一个内核，**禁止**为了"顺手优化"在这里
//   再写一份合成逻辑 —— 两份实现漂移的症状是"预览与实际不一致"，
//   本项目在蚂蚁线、画笔/形状写回上都吃过这个亏（见 docs/实现铁律.md）。
QImage compositeLayersRegion(const QList<QImage> &images,
                             const QList<BlendMode> &modes,
                             const QList<qreal> &opacities,
                             const QList<bool> &visible,
                             const QSize &bounds,
                             const QRect &region,
                             const QColor &background = Qt::transparent,
                             quint64 *fastPixelsOut = nullptr);

} // namespace blend
} // namespace zl
