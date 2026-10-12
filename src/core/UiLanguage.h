#ifndef ZLPE_CORE_UILANGUAGE_H
#define ZLPE_CORE_UILANGUAGE_H

// ============================================================================
//  UiLanguage.h —— 界面语言的「决策 + 持久化 + 装载」
// ----------------------------------------------------------------------------
//  ★ 为什么单独一个文件 ★
//
//    "当前界面是什么语言"这件事有三个消费方：
//      ① main.cpp    —— 启动时决定装不装翻译器（必须早于第一个窗口）
//      ② MainWindow  —— 语言菜单的当前项、写入用户的选择
//      ③ AppInfo     —— 窗口标题里的显示名（中文界面显示中文名，英文界面英文名）
//
//    三方各写一份"怎么读设置、怎么推断 locale"，迟早出现
//    "菜单说已经切成英文、标题还是中文"这种对不上的 bug —— 而这种 bug
//    双构建 0 错 0 警，只有真的去看窗口才会发现。
//
//  ★ 路线 B（源码英文 + 各语言 .qm 翻译）下的角色 ★
//
//    源码文本是英文（阶段 2 英文化之后），界面语言只有两种真实状态：
//      · "en"     —— 不装翻译器，控件显示的就是 tr() 里的英文源码；
//      · "zh_CN"  —— 装上内嵌的 zlphotoedit_zh_CN.qm，tr() 走中文翻译。
//    本文件把"选哪个、存哪、怎么装"收成一处，调用方只管问结论。
//
//  ★ 与 QSettings 的关系 ★
//
//    键名 "ui/language" 一旦发布就不要改（改了等于重置用户的选择）。
//    空值（从未选过）= 按系统 locale 推断；推断结果与显式选择一样，
//    都被"可用语言表"钳制 —— 所以不存在"存了个不存在的语言"这种状态，
//    也就不需要下游到处写兜底分支。
//
//  ★ 翻译文件不落盘 ★
//
//    .qm 由构建期 lrelease 生成并**内嵌进可执行文件**（见 ZlPhotoEdit.pro
//    与 CMakeLists.txt 的「多语言」段），运行时从 ":/i18n/..." 读。
//    用户机器上不存在独立的翻译文件夹 —— 也就没有"忘了带目录"
//    这种部署事故（这是用户明确提出的硬约束）。
// ============================================================================

#include <QString>
#include <QStringList>

class QCoreApplication;

namespace zl {
namespace uilang {

/// 源码文本所用的语言。
/// ★ 阶段 1 是 "zh_CN"（源码中文）；阶段 2 英文化时改成 "en" ★
///   改它一行的效果：界面语言决策整体翻转 —— 英文从"翻译目标"
///   变成"源码本身"，中文从"源码"变成"唯一需要 .qm 的语言"。
QString sourceLanguage();

/// 当前可用的界面语言列表（源码语言永远排在第一位）。
/// 内容 = 源码语言 + 构建期内嵌的翻译语言（见 .cpp 的 kEmbeddedLanguages）。
/// 列表只有一项时，语言菜单不该出现（见 MainWindowActions.cpp）。
QStringList availableLanguages();

/// 当前生效的界面语言：
///   ① QSettings "ui/language" 里用户显式选过的值；
///   ② 没选过 → 按系统 locale 推断（中文环境 → zh_CN，其余 → en）；
///   ③ 结果不在可用列表里 → 回落源码语言。
/// 任何情况下返回值一定在 availableLanguages() 里。
QString currentLanguage();

/// 记录用户的选择（**下次启动生效** —— 运行中切换要重翻译整个 UI，
/// v1 不做，见 MainWindowActions.cpp 的提示框）。
/// code 不在可用列表里时忽略（绝不把垃圾写进设置）。
void setCurrentLanguage(const QString &code);

/// 该语言是否需要加载翻译文件（源码语言本身不需要）。
bool needsTranslation(const QString &code);

/// 给 app 安装/卸载翻译。返回 true 表示"此刻有翻译在生效"。
///   · code == 源码语言  → 卸载既有翻译，返回 false；
///   · .qm 加载失败      → qWarning 并返回 false（界面保持源码文本，
///                         不让"少一个翻译文件"把程序挡在启动之前）。
/// 进程内同时只存在一个翻译器（重复安装会让两份翻译叠在一起）。
bool installTranslator(QCoreApplication *app, const QString &code);

/// 菜单里用的语言显示名。
/// ★ 按"母语显示"的惯例，不走 tr() ★
///   中国人看到「简体中文」、英国人看到 "English" —— 与系统设置、
///   浏览器一致。若把语言名也翻译成当前语言，反而要用户先认字。
QString displayName(const QString &code);

} // namespace uilang
} // namespace zl

#endif // ZLPE_CORE_UILANGUAGE_H
