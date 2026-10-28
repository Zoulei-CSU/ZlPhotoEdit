#ifndef ZLPE_CORE_UITHEME_H
#define ZLPE_CORE_UITHEME_H

// ============================================================================
//  UiTheme.h —— 界面主题的「偏好 + 持久化 + 应用」
// ----------------------------------------------------------------------------
//  三种取值：默认（跟随系统）／浅色／深色。
//
//  ★ 为什么单独一个文件，而不是在菜单里直接 setPalette ★
//
//    "用哪个主题"有两拨消费者，时机完全不同：
//      ① main.cpp   —— 启动时必须在建窗口**之前**应用，否则用户会看到
//                      先闪一下系统配色再跳到所选主题；
//      ② MainWindow —— 菜单里改完要立刻应用 + 落盘。
//    两边各写一份"怎么读设置、怎么造配色"，迟早出现"菜单勾着深色、
//    程序起来是浅色"这种对不上的 bug。
//
//  ★ 为什么要自己造 palette，不能只靠 setColorScheme ★
//    QStyleHints::setColorScheme() 是 Qt 6.5 才有的，而本工程还要支持
//    Qt 5.15。Qt5 下根本没有"告诉样式该用深色"的通道，只能自己给一套
//    palette。为了两版行为一致，Qt6 下同样以自造 palette 为准，
//    setColorScheme 只作为**给样式看的提示**（见 .cpp 里的顺序说明）。
//
//  ★ 显式主题必须配 Fusion 样式 ★
//    Windows 的 windowsvista 样式会自己画菜单栏、工具栏、滚动条，
//    相当一部分颜色不走 palette，深色主题会变成"一半深一半浅"的花脸。
//    Fusion 是纯 palette 驱动的，是 Qt 官方对自定义配色的推荐样式。
//    所以显式主题下切 Fusion，选"默认"时再切回用户的原始样式 ——
//    "默认"的含义必须是"完全交回系统"，包括样式。
//
//  ★ "默认"不是第四套配色 ★
//    它就是"把我改过的东西全撤销"：palette 交回平台、样式回原位、
//    colorScheme 取消覆盖。因此本模块在启动时要先 captureSystem() 存一份
//    原始样式名 —— 样式没有查询接口，那名字是 auto 能还原的唯一凭据。
//    配色则不用存：传空 palette 让 Qt 重新向平台要，比快照准确 ——
//    快照是启动那一刻的，用户之后在系统里改了配色就过期了。
//
//  ★ 与 QSettings 的关系 ★
//    键 "ui/theme"，取值 "auto"（默认）／"light"／"dark"。
//    与 "ui/language" 平级，都装应用级偏好。发布后不要改键名。
// ============================================================================

#include <QList>
#include <QString>

namespace zl {
namespace uitheme {

/// 主题偏好。顺序即菜单顺序，"自动"必须在最前（它是默认值）。
enum class Mode {
    Auto,
    Light,
    Dark,
};

/// 全部取值，供建菜单用。不要写死数量。
const QList<Mode> &allModes();

/// 当前偏好。没存过 = Auto；存了不认识的值也按 Auto 处理
/// （用户手改坏设置文件时，界面不该因此变成花屏）。
Mode preference();

/// 写入偏好。只接受枚举内的值，不接受字符串 ——
/// 调用方本来就只有菜单，多一层字符串解析只会多一条出错路径。
/// 会自动 sync()：用户可能点完就 Alt+F4。
void setPreference(Mode mode);

/// 菜单项的显示文本（已翻译）。
QString displayName(Mode mode);

/// 偏好 → QSettings 值（"auto" / "light" / "dark"）。
QString keyOf(Mode mode);

/// ★ 启动时调用一次，必须在 QApplication 构造之后、applyTheme 之前 ★
/// 记下系统的原始样式名 —— 样式没有查询接口，这是"默认"能还原的唯一凭据。
/// 重复调用无副作用（只记第一次）。
void captureSystem();

/// 应用主题。三件事一起做：样式、palette、colorScheme 提示。
/// 顺序在 .cpp 里有说明，不要调换。
void applyTheme(Mode mode);

/// 当前生效的是不是深色（供自检断言用，不参与绘制决策）。
bool isDark();

/// 富文本里用的语义色。
///
/// ★ 为什么需要这一组，而不是直接写 #666666 ★
///   富文本（QLabel 的 RichText）里写不了 palette(mid) 这类**动态引用**，
///   样式表才支持。写死一个灰在深色底上就几乎看不见。
///   这里按当前主题返回深浅两套里的那一套，调用方拼进 HTML 即可。
///
/// ★ 深浅两套必须成对改 ★
///   只改一个主题下的取值，症状是"某个主题下这段提示字看不清"。
enum class Semantic {
    Primary,    ///< 正文（与 WindowText 同义，供图例等与正文并排的场合）
    Secondary,  ///< 次要说明（等价于 palette(mid)）
    Info,       ///< 中性提示
    Success,    ///< 保留、无损失
    Warning,    ///< 变化、需留意
    Danger,     ///< 裁切、破坏性
};

/// 语义色 → "#RRGGBB"（可直接拼进富文本）。
QString semanticColor(Semantic role);

} // namespace uitheme
} // namespace zl

#endif // ZLPE_CORE_UITHEME_H
