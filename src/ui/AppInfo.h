#ifndef ZLPE_UI_APPINFO_H
#define ZLPE_UI_APPINFO_H

#include <QIcon>
#include <QString>
#include <QStringList>

// ============================================================================
//  AppInfo —— 应用名称与图标的**唯一来源**
// ----------------------------------------------------------------------------
//  ★ 为什么要有这个文件 ★
//
//    程序名此前是散落的字面量：窗口标题里有两处、关于对话框里有三处、
//    退出提示里有一处、启动日志里有两处。加一个中文名意味着要同步改
//    七八个地方 —— 漏掉哪一处，用户就会在某个角落看到"半中半英"的名字，
//    而编译器完全不管（都是合法的字符串）。
//
//    更麻烦的是**图标的两种失效都是静默的**：
//      · 资源路径写错 → QIcon 为空 → 标题栏/任务栏没有图，不报错；
//      · SVG 渲染不出来（XML 里带 DOCTYPE 等）→ 同样只是"没有图"。
//    所以图标也必须有一个集中入口，好让自检能直接量它。
//
//  ★ 名称的"两个身份"必须分开 ★
//
//    `kOrganizationName` / `kApplicationName`（见 main.cpp）是 **QSettings
//    的存储路径**，一旦发布就不能改 —— 改了等于把用户的全部设置重置。
//
//    而 displayName() 只是**给人看的字符串**，随时可改。
//    本文件提供的全是后者。绝不要把 QCoreApplication::setApplicationName()
//    换成中文名。
//
//  ★ 图标分两层，别混 ★
//
//    · windowIcon()  —— Qt 层的窗口/任务栏图标，来自 qrc 里的 logo.png。
//    · exe 图标      —— Windows 资源里的那个（resources/app.rc + logo.ico），
//                       资源管理器里看到的文件图标。它**运行时改不了**，
//                       只能靠 .rc 在链接期嵌入。
//    两者必须用同一张图，否则"任务栏一个样、桌面文件另一个样"。
//    （2026-10-15 起窗口图标源图是 logo.png；macOS .icns 与 Linux
//     桌面图标也由同一张 logo.png 派生。）
// ============================================================================

namespace zl {
namespace appinfo {

/// 中文显示名："逐鹿 图片编辑器"
QString displayNameCn();

/// 英文显示名（= 工程名，也是 QSettings 里的应用名）："ZlPhotoEdit"
QString displayNameEn();

/// 完整显示名："逐鹿 图片编辑器" —— 默认给中文
QString displayName();

/// 主窗口标题里的后缀："逐鹿 图片编辑器" / "ZlPhotoEdit"
/// ★ 只有当两个名字不同时才会写成 "中文 (English)" ★
///   名字相同时拼成重复串（"ZlPhotoEdit (ZlPhotoEdit)"）比不写更糟。
QString titleSuffix();

/// 带文件名的窗口标题："foo.png [100%] - 逐鹿 图片编辑器"
QString windowTitleFor(const QString &fileName, const QString &zoomText);

/// 应用程序图标（资源的唯一入口）。
/// ★ 为什么还保留集中入口，而不是各处写 QIcon(":/logo.png") ★
///   PNG 由 Qt 内置解码器处理，不依赖 qsvg 插件（这一点与当年的
///   .svg 不同）；但"资源没打进 qrc / 文件损坏"的失败形态仍然是
///   **静默的空图标**。集中一个入口，自检才能逐档量像素
///   （windowIconPixmap），把"有没有画出来"变成可判定的事。
/// 失败返回空 QIcon，调用方（main / MainWindow）必须有降级方案。
QIcon windowIcon();

/// 应用的 .ico 文件路径（仅供构建与自检对账，运行时并不读它 —— 
/// exe 图标是链接期嵌进去的）。返回绝对路径，文件不存在时也能返回。
QString exeIconSourcePath();

/// 是否为当前平台启用了 exe 资源图标（只在构建时由宏决定）。
bool hasExeIcon();

/// 自检用：把 windowIcon() 渲染成 px×px 位图，失败返回空 QPixmap。
QPixmap windowIconPixmap(int px);

} // namespace appinfo
} // namespace zl

#endif // ZLPE_UI_APPINFO_H
