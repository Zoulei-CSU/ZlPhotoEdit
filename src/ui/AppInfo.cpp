#include "ui/AppInfo.h"

// ★ 界面语言决定显示名用中文还是英文 ★
//   中文界面 → "逐鹿 图片编辑器 (ZlPhotoEdit)"；英文界面 → "ZlPhotoEdit"。
//   依赖方向是 ui → core（UiLanguage 在 core 层），单向，不成环。
#include "core/UiLanguage.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPixmap>

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
    return QLatin1String(kDisplayNameEn);
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
//   gen_icons.py 用 prefix="/" + <file>logo.png</file>，
//   prefix 会与相对路径**拼接**，所以最终路径是 ":/logo.png"（不是 :/icons/…）。
//   少一个斜杠、多一个斜杠都会静默拿到空图标。
//   ★ 2026-10-15：logo.svg → logo.png ★
//     源图换成用户提供的 512×512 成品位图后，窗口图标不再经过
//     QSvgRenderer —— 少一层"解析失败但静默"的风险，也不再依赖
//     qsvg 插件。exe 图标仍走 logo.ico → app.rc（PE 资源段）；
//     macOS 的 .icns 与 Linux 的 256×256 桌面图标也由同一张
//     logo.png 派生（见 packaging/）。
const char *kIconResourcePath = ":/logo.png";

// 源图只解码一次：windowIcon() 要渲染 8 档 × 2 个 dpr = 16 次，
// 每次重新解码同一张 PNG 是白工。静态局部量在 C++11 起初始化线程安全。
const QImage &sourceLogo()
{
    static const QImage img(QString::fromUtf8(kIconResourcePath));
    return img;
}

QPixmap renderLogo(int px, qreal dpr)
{
    const QImage &src = sourceLogo();
    if (src.isNull())
        return QPixmap();          // 资源缺失 / PNG 解码失败

    const int phys = qMax(1, qRound(px * dpr));
    QPixmap pm(phys, phys);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // 512×512 源图缩到各档逻辑尺寸，SmoothPixmapTransform 保证
    // 16px 小图不糊。目标矩形必须是逻辑尺寸 px×px —— 与
    // IconLibrary.cpp 的 SVG 分支同一实测结论：不传 bounds 时
    // dpr>1 只画左上角一小块（Qt5/Qt6 同罪）。
    p.drawImage(QRectF(0, 0, px, px), src);
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
