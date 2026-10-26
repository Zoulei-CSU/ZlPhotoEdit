#ifndef ZLPE_UI_ICONLIBRARY_OLD_H
#define ZLPE_UI_ICONLIBRARY_OLD_H

// ============================================================================
//  ★ 这是一份"冻结的旧版"，只为 A/B 对照存在，不参与应用构建 ★
// ----------------------------------------------------------------------------
//  取自引入"渲染时上色"之前的版本（提交 31b9e5a 的 IconLibrary）。
//  整体挪进 zl::icons_old 命名空间，于是新旧两版能在同一个进程里并存、
//  交替跑同一组场景 —— 否则跨进程比较会被编译差异和机器抖动干扰。
//
//  ★ include guard 必须改名 ★
//    它原本与应用版同名（ZLPE_UI_ICONLIBRARY_H）。两者一起 include 时，
//    后到的那个会被整个跳过，报"zl::icons has not been declared"。
//
//  新版改了什么、为什么改、代价多少，见 docs/实现铁律.md §10.8.1 ~ §10.8.2。
//  改完上色实现后，别忘同步这里不需要动（它是基线），但要重跑对照。
// ============================================================================

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
// ============================================================================

namespace zl {
namespace icons_old {

/// "name" → ":/icons/name.svg"
QString pathOf(const QString &name);

/// 该名字的图标是否真的在 resources.qrc 里
bool exists(const QString &name);

/// 取图标（多档尺寸 + 进程内缓存）。
/// 名字不存在或 SVG 渲染失败时返回**空 QIcon** —— 调用方必须有降级方案
/// （工具箱会退回"工具名首字"的字母占位图）。
QIcon icon(const QString &name);

/// 指定像素尺寸的图标位图（给自绘控件用；同样带缓存）
QPixmap pixmap(const QString &name, int px);

/// 工具 id → 图标。未知 id 返回空 QIcon。
/// ★ 工具的 id 是持久化键（QSettings），图标映射只在 ui 层做 ——
///   工具类本身不允许 include ui/，所以这个映射放这里而不是工具里。
QIcon iconForTool(const QString &toolId);

/// qrc 里全部图标名（升序）。自检用它遍历所有图标做渲染检查。
QStringList allNames();

/// 已缓存的名字数量（自检用：验证缓存确实在生效）
int cachedCount();

/// 清空缓存（自检用）
void clearCache();

} // namespace icons_old
} // namespace zl

#endif // ZLPE_UI_ICONLIBRARY_OLD_H
