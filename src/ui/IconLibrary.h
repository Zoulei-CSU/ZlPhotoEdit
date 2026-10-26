#ifndef ZLPE_UI_ICONLIBRARY_H
#define ZLPE_UI_ICONLIBRARY_H

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

// ============================================================================
//  IconLibrary —— 应用图标的唯一来源
// ----------------------------------------------------------------------------
//  图标本体是 resources/icons/*.svg（由 resources/gen_icons.py 生成），
//  经 resources/resources.qrc 编进可执行文件，运行时路径形如
//  ":/icons/tool_brush.svg"。
//
//  ★ 为什么不用 QIcon(":/icons/x.svg") 直接加载 ★
//    QIcon 从 .svg 文件加载会走 QImageReader → svg 图片格式插件（qsvg.dll）。
//    插件没部署时它**静默返回空图标** —— 没有任何报错，界面上只表现为
//    "按钮上没有图"，而这类问题最难查（看不出是路径问题、插件问题还是文件问题）。
//    这里改成显式用 QSvgRenderer 自己光栅化：
//      · SVG 写坏 / 文件缺失时 isValid() 立刻为假，我们能主动发现并降级；
//      · 一次渲染多档尺寸交给 QIcon，工具栏（24px）、菜单（16px）、
//        列表（20px）各自取最合适的一档，小尺寸下不会糊成一团。
//
//  ★ 名字 ⇄ 文件必须成对 ★
//    `icons::icon("tool_brush")` 里的字符串写错一个字母，图标就会静默消失。
//    `tests/action_check.py` 会对账"代码里引用过的名字"与"实际存在的 .svg"，
//    `tests/icon_test.cpp` 则逐个把图标渲染出来量像素（防"文件在但画不出东西"）。
//
//  ★ 上色：渲染时染色，不带第二套图 ★
//    SVG 文件里写的是一种固定颜色（#33393f，见 resources/gen_icons.py）。
//    在深色主题下这种深色图标压在深色工具栏上根本看不清，而两套图既要有
//    同步维护的两份文件、又要在 qrc 里成对登记。
//    做法改成：渲染完立刻用 CompositionMode_SourceIn 把整块位图染成
//    **当前 palette 的前景色**（深色主题下自动变成浅色），一套图两边都清楚。
//    代价是所有图标必须**单色** —— 这一点由 icon_test 的断言守着。
//    （提示：也可用 QIcon::Disabled 拿到压暗的一档，见 icon() 里的说明。）
// ============================================================================

namespace zl {
namespace icons {

/// "name" → ":/icons/name.svg"
QString pathOf(const QString &name);

/// 该名字的图标是否真的在 resources.qrc 里
bool exists(const QString &name);

/// 取图标（多档尺寸 + 进程内缓存）。
/// 名字不存在或 SVG 渲染失败时返回**空 QIcon** —— 调用方必须有降级方案
/// （工具箱会退回"工具名首字"的字母占位图）。
///
/// 颜色**不是**参数：按当前 palette 的前景色自动上色，
/// 主题变了由调用方 clearCache() 后重新 setIcon（见 clearCache 的说明）。
QIcon icon(const QString &name);

/// 指定像素尺寸的图标位图（给自绘控件用；同样带缓存）。
/// color 有效时按该色上色（自绘控件常在非默认底色上画图，例如选中行），
/// 不传则用当前 palette 的前景色。
QPixmap pixmap(const QString &name, int px, const QColor &color = QColor());

/// 当前主题下的图标前景色。
/// 主题切换后由界面层拿它与"上次上色用的颜色"比较，决定要不要重取图标。
QColor currentColor();

/// 工具 id → 图标名（未知 id 返回空串）。
QString iconNameForTool(const QString &toolId);

/// 工具 id → 图标。未知 id 返回空 QIcon。
/// ★ 工具的 id 是持久化键（QSettings），图标映射只在 ui 层做 ——
///   工具类本身不允许 include ui/，所以这个映射放这里而不是工具里。
QIcon iconForTool(const QString &toolId);

/// qrc 里全部图标名（升序）。自检用它遍历所有图标做渲染检查。
QStringList allNames();

/// 已缓存的名字数量（自检用：验证缓存确实在生效）
int cachedCount();

/// 清空缓存。
///
/// ★ 换主题后必须调用 ★
///   图标是按 palette 前景色在**渲染时刻**定色并缓存的，切到深/浅主题后
///   旧缓存仍是旧颜色。除了 clearCache()，还必须把所有已经 setIcon 过的
///   菜单/工具栏重新 setIcon 一遍（缓存空着不会自己刷新界面上已有图标）。
void clearCache();

} // namespace icons
} // namespace zl

#endif // ZLPE_UI_ICONLIBRARY_H
