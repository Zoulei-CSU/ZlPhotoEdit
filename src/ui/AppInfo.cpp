#include "ui/AppInfo.h"

// ★ 界面语言决定显示名用中文还是英文 ★
//   中文界面 → "逐鹿 图片编辑器 (ZlPhotoEdit)"；英文界面 → "ZlPhotoEdit"。
//   依赖方向是 ui → core（UiLanguage 在 core 层），单向，不成环。
#include "core/UiLanguage.h"

#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace zl {
namespace appinfo {

// ---------------------------------------------------------------------------
//  名称
// ---------------------------------------------------------------------------
//  ★ 中文名带一个空格 ★
//    用户给的是 "逐鹿 图片编辑器"（"逐鹿" 与 "图片编辑器" 之间一个半角空格）。
//    这个空格不是排版失误 —— 它是"品牌名 + 品类名"的写法，
//    与 "Adobe Photoshop" 同类。标题栏里保留它。
// ---------------------------------------------------------------------------

namespace {

// 中文显示名 —— 给人看的
constexpr const char *kDisplayNameCn = "逐鹿 图片编辑器";

// 英文显示名 —— 与 main.cpp 的 kApplicationName 保持一致。
//   ★ 这两处不同步会怎样 ★
//     QSettings 的路径由 kApplicationName 决定，这里只用于显示。
//     不同步只会让"关于"框里的英文名与注册表路径不一致 ——
//     不致命，但会让排查问题的人困惑，所以加了一条自检盯着
//     （tests/mainwin_test.cpp 里比对 QCoreApplication::applicationName()）。
constexpr const char *kDisplayNameEn = "ZlPhotoEdit";

} // namespace

QString displayNameCn()
{
    return QString::fromUtf8(kDisplayNameCn);
}

QString displayNameEn()
{
    return QString::fromLatin1(kDisplayNameEn);
}

QString displayName()
{
    // 界面语言为英文时显示英文名；其余（中文 / 未设置）显示中文名。
    //
    // ★ 为什么不把中文名英文化后只留一个名字 ★
    //   "逐鹿 图片编辑器"是品牌名（用户给的名字，中间那个空格是
    //   "品牌 + 品类"的写法）。英文界面沿用工程名 ZlPhotoEdit 是
    //   稳妥默认；哪天要起正式的英文品牌名，只改 kDisplayNameEn 一处。
    if (zl::uilang::currentLanguage() == QStringLiteral("en"))
        return displayNameEn();
    return displayNameCn();
}

QString titleSuffix()
{
    // 两个名字一样时不要拼成 "X (X)"
    const QString cn = displayNameCn();
    const QString en = displayNameEn();
    if (cn == en)
        return cn;

    // 英文界面：标题只放英文名。再拼一遍中文（"ZlPhotoEdit (逐鹿 图片编辑器)"）
    // 对英文用户是噪音，对中文用户又会在英文模式下露馅。
    if (zl::uilang::currentLanguage() == QStringLiteral("en"))
        return en;

    return QStringLiteral("%1 (%2)").arg(cn, en);
}

QString windowTitleFor(const QString &fileName, const QString &zoomText)
{
    if (fileName.isEmpty())
        return titleSuffix();

    if (zoomText.isEmpty())
        return QStringLiteral("%1 - %2").arg(fileName, titleSuffix());

    // 标题格式： "文件名 [50%] - 逐鹿 图片编辑器 (ZlPhotoEdit)"
    return QStringLiteral("%1 [%2] - %3").arg(fileName, zoomText, titleSuffix());
}

// ---------------------------------------------------------------------------
//  图标
// ---------------------------------------------------------------------------
//  ★ 尺寸档位的选择 ★
//    窗口图标会被 Windows 用在很多地方：标题栏（16）、Alt+Tab（32/48）、
//    任务栏（32 或 48，随 DPI）、任务切换器（256）。给到 256 可以让
//    任务切换器不糊；再往上没必要 —— 系统不会向我们要 512 的。
// ---------------------------------------------------------------------------

namespace {

const int kIconSizes[] = {16, 20, 24, 32, 48, 64, 128, 256};
const int kIconSizeCount = int(sizeof(kIconSizes) / sizeof(kIconSizes[0]));

// ★ 资源路径的坑 ★
//   gen_icons.py 用 prefix="/" + <file>logo.svg</file>，
//   prefix 会与相对路径**拼接**，所以最终路径是 ":/logo.svg"（不是 :/icons/…）。
//   写成 ":/logo.svg" 少一个斜杠（":/logo.svg" vs ":/logo.svg"）都会静默拿到空图标。
const char *kIconResourcePath = ":/logo.svg";

QPixmap renderLogo(int px, qreal dpr)
{
    QSvgRenderer renderer(QString::fromLatin1(kIconResourcePath));
    if (!renderer.isValid())
        return QPixmap();          // 资源缺失 / SVG 解析失败

    const int phys = qMax(1, qRound(px * dpr));
    QPixmap pm(phys, phys);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&p);
    p.end();

    return pm;
}

} // namespace

QPixmap windowIconPixmap(int px)
{
    if (px <= 0)
        return QPixmap();
    return renderLogo(px, 1.0);
}

QIcon windowIcon()
{
    QIcon result;

    // 先用中间档探一次：探针失败 ⇒ 后面的循环全部会得到空 pixmap，
    // 直接返回空 QIcon，让调用方走降级路径（不设图标 / 用系统默认图标）。
    if (renderLogo(48, 1.0).isNull())
        return result;

    for (int i = 0; i < kIconSizeCount; ++i) {
        result.addPixmap(renderLogo(kIconSizes[i], 1.0));
        result.addPixmap(renderLogo(kIconSizes[i], 2.0));
    }
    return result;
}

QString exeIconSourcePath()
{
    // ★ 这里的相对路径是"从可执行文件所在目录往上找" ★
    //   实际上这个函数只给自检/对账用，所以按源码树布局算：
    //     <repo>/src/ui/AppInfo.cpp  →  <repo>/resources/logo.ico
    //   用 __FILE__ 而不是 QCoreApplication::applicationDirPath() ——
    //   后者在构建产物目录（build/release/）里，与源码树无关。
    const QString here = QString::fromUtf8(__FILE__);
    QDir d(QFileInfo(here).absolutePath());      // <repo>/src/ui
    d.cdUp();                                    // <repo>/src
    d.cdUp();                                    // <repo>
    return d.absoluteFilePath(QStringLiteral("resources/logo.ico"));
}

bool hasExeIcon()
{
#if defined(ZLPE_HAS_EXE_ICON) && ZLPE_HAS_EXE_ICON
    return true;
#else
    return false;
#endif
}

} // namespace appinfo
} // namespace zl
