// ============================================================================
//  main.cpp —— 程序入口
// ----------------------------------------------------------------------------
//  这里做的事情看似很少，但每一件的顺序都有讲究：
//
//    1. 高 DPI 属性      —— 必须在 QApplication 构造**之前**设置
//    2. QApplication     —— 之后才能用图形相关的一切
//    3. 应用图标         —— 必须在 QApplication 之后、建窗口之前
//    4. 组织名/应用名    —— 必须在读 QSettings 之前设置
//    4.5 界面语言        —— 必须在第一个窗口（第一个 tr()）之前
//    5. 编码设置         —— 必须在任何中文字符串比较/转换之前
//    6. 启动日志
//    7. MainWindow       —— 最后创建主窗口
//
//  ★ 本文件是 Qt5/Qt6 版本判断的合法位置之一 ★
//    另一个是 src/compat/QtCompat.h。除这两处外，
//    工程内不应出现任何 #if QT_VERSION 分支。
// ============================================================================

#include "compat/QtCompat.h"
#include "ui/MainWindow.h"
// ★ 应用图标的唯一来源 ★
//   不在这边直接写 QIcon(":/logo.svg")：那条路会在 qsvg 插件缺失时
//   **静默**返回空图标，而且路径写错也不报错 —— 症状只是"没有图标"。
//   AppInfo 自己用 QSvgRenderer 光栅化，失败能主动发现（见其文件头）。
#include "ui/AppInfo.h"
// ★ 界面语言 ★ 启动时决定装不装翻译器，必须早于第一个窗口
//   （tr() 的结果在控件构造那一刻就定下来了）。
#include "core/UiLanguage.h"

#include <QApplication>
#include <QSettings>
#include <QFont>
#include <QFontDatabase>
#include <QString>
// Qt5 与 Qt6 都把 QTextCodec 放在这里（Qt6 需要 Qt5Compat 模块；
// 本机是 Qt5，所以在本文件的 Qt5 分支里使用）。
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#  include <QTextCodec>
#endif

#include <QImageReader>
#include <QImageWriter>
#include <QDebug>
#include <QByteArray>
#include <QStringList>

#include <cstdio>

// ============================================================================
//  应用标识
// ============================================================================
//
//  ★ 为什么要显式设置 ★
//
//    QSettings 的位置由"组织名 + 应用名"决定：
//      Windows 注册表：HKEY_CURRENT_USER\Software\<组织>\<应用>
//      Linux/Mac：~/.config/<组织>/<应用>.conf
//
//    如果不设置，QSettings 会用一个空的/默认的路径 ——
//    结果是"设置能存能读，但下次启动读不到"，或者写入了一个
//    非常奇怪的位置（例如注册表里的 Software\Unknown Organization）。
//
//    这个 bug 的表现是"窗口大小、工具参数每次启动都回到默认值"，
//    而查起来又很费劲，所以必须在 main 里就设好。
//
//  ★ 这两个字符串一旦发布就不能改 ★
//    改了会让用户丢失全部既有设置（相当于"重置"）。
//
//  ★ 中文名不在这里 ★
//    程序的**显示名**是"逐鹿 图片编辑器"（见 src/ui/AppInfo.h），
//    但那只用于窗口标题、关于对话框等"给人看"的地方。
//    QSettings 认的还是下面的 kOrganizationName / kApplicationName。
//    千万别把 kApplicationName 改成中文 —— 那是重置用户设置，
//    而且路径里的中文在某些系统上还会带来额外的编码麻烦。
// ============================================================================

namespace {
constexpr const char *kOrganizationName = "ZlStudio";
constexpr const char *kApplicationName  = "ZlPhotoEdit";
constexpr const char *kApplicationVer   = "0.1.0";
} // anonymous namespace

// ============================================================================
//  高 DPI 设置
// ============================================================================
//
//  ★ Qt5 与 Qt6 的差异是"根本性"的 ★
//
//    Qt 5：高 DPI 缩放默认**关闭**，需要显式打开两个属性：
//            AA_EnableHighDpiScaling     —— 让逻辑像素与设备像素分离
//            AA_UseHighDpiPixmaps        —— 让 QPixmap 按设备像素比渲染
//          并且这两个属性必须在 QApplication 构造前设置，
//          构造之后设置无效（Qt 会打印警告但不会崩溃）。
//
//    Qt 6：高 DPI 缩放**永远开启**，上面两个属性已废弃。
//          在 Qt6 下设置它们会得到"deprecated"警告。
//          取而代之的是 AA_UseHighDpiPixmaps 的替代品不需要了，
//          而 AA_EnableHighDpiScaling 在 Qt6 里设了也无害（被忽略）。
//
//  ★ 所以这里必须做版本判断 ★
//    这是 QtCompat.h 之外的第二个（也是最后一个）合法判断点 ——
//    因为高 DPI 必须在 QApplication 构造前设置，而 QtCompat.h
//    提供的工具函数都是运行期的，来不及。
//
//  ★ 为什么要设 rounding policy ★
//    Qt5.14+ 提供了 PassThrough 舍入策略，让 1.5 倍缩放下的
//    1 像素线条不会因为取整而时粗时细。对图像编辑程序尤其重要 ——
//    画布的 1 像素边框在 150% 缩放下必须始终是 1 个物理像素的粗细。
static void setupHighDpi()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // ── Qt 5 分支 ──
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
#endif

    // 舍入策略在 Qt5.14 与 Qt6 都有，不需要判断版本。
    // PassThrough 表示"不做额外的整数取整"，直接把逻辑坐标
    // 乘以缩放系数交给系统 —— 这是目前在分数缩放下最清晰的选择。
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif
}

// ============================================================================
//  编码设置
// ============================================================================
//
//  ★ Qt5 与 Qt6 的差异 ★
//
//    Qt 5.15：QTextCodec 仍然存在但已标记废弃。
//             常用做法是 QTextCodec::setCodecForLocale(
//                         QTextCodec::codecForName("UTF-8"));
//             但更推荐用 QTextStream 的默认编码设置。
//
//    Qt 6：QTextCodec 被整个移出 QtCore（挪到了 Qt5Compat 模块）。
//          取而代之的是 QStringConverter + QStringEncoder。
//          设置默认编码要用 QStringConverter::setDefaultCodec
//          （注意这个 API 在 6.0 引入，6.5 又被改名，
//            所以这里用最保守的方式）
//
//  ★ 实际上本工程"不需要"做这件事 ★
//
//    因为我们：
//      · 源码是 UTF-8 with BOM，字符串常量在编译期就正确了
//      · 通过 /utf-8 显式告诉 MSVC 了
//      · 读写文件全部走 QImage / QImageReader，不涉及文本编码
//      · 没有用 QTextStream 读写用户文件
//
//    所以这里只做一件事：把"本地编码"确认为 UTF-8，
//    以防某个第三方代码（例如某个插件的日志）用了
//    QString::fromLocal8Bit —— 在中文 Windows 上那会得到 GBK 解析结果。
//
//  ★ 保守实现：只在 Qt5 下做，Qt6 下不动 ★
//    Qt6 在 Windows 上默认就是 UTF-8（因为 Qt6 内部统一用 UTF-8 且
//    ::fromLocal8Bit 会走 QStringConverter 的 UTF-8 实现），
//    主动设置反而可能踩到 API 变动的坑。
static void setupUtf8Encoding()
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // 该调用在 Qt5 里有轻微性能开销（首次调用时构造编解码器），
    // 但只在启动时执行一次，可以忽略。
    QTextCodec *utf8 = QTextCodec::codecForName("UTF-8");
    if (utf8)
        QTextCodec::setCodecForLocale(utf8);
#endif
}

// ============================================================================
//  界面语言
// ============================================================================
//
//  ★ 为什么必须放在这里（步骤 3 之后、第一个窗口之前）★
//
//    1. 晚于步骤 3：currentLanguage() 要读 QSettings，而 QSettings 的
//       存储路径由组织名/应用名决定 —— 顺序反了会读到别处的设置
//       （或者更糟：写到一个野路径里去）。
//    2. 早于 MainWindow：tr() 的结果在**控件构造时**就定下来了
//       （菜单/按钮/标签的文本在构造函数里一次性生成，之后不会自动
//       重算）。翻译器装晚了，先建好的控件永远停在源码文本 ——
//       这正是"明明装了翻译却没效果"最常见的原因，而且它不报任何错。
//
//  ★ 失败不致命 ★
//    翻译文件缺失/损坏时回落到源码文本（英文或中文，见
//    core/UiLanguage.cpp 的 sourceLanguage()），只打一条 qWarning ——
//    不让"少一个 .qm"把整个程序挡在启动之前。
//
//  ★ app 由调用方传入而不是直接用 qApp ★
//    本文件的其它 setup 函数都是无参的静态函数，保持同一个风格；
//    更重要的是"显式传进来"让依赖一眼可见 —— 翻译器要装到
//    **这个** app 上，装错了对象同样是静默无效。
static void setupUiLanguage(QCoreApplication *app)
{
    const QString pref = zl::uilang::languagePreference();
    const QString lang = zl::uilang::currentLanguage();
    const bool loaded = zl::uilang::installTranslator(app, lang);

    // 启动日志里打出结论：用户报"界面语言不对"时，这一行能直接告诉
    // 排查者是"没装上"还是"装错了语言"，还是"自动推断给了一个他没想
    // 要的语言"（偏好=自动 时 locale 推断那一跳是看不见的，打出来才可查）。
    //
    // ★ 为什么用 fprintf 而不是 qInfo ★
    //   与上面的版本行一致：qInfo() 在 MSYS/Git Bash 的重定向环境下会
    //   **静默丢失**（实测：同一程序在 PowerShell 下 qInfo 正常、在
    //   Git Bash 下四个日志级别全部无声，只有 fprintf 可靠）。
    //   诊断行的全部价值就在于"出问题时能看见"—— 宁可少一个
    //   QT_LOGGING_RULES 开关，也不能接受它时有时无。
    std::fprintf(stderr, "[ZlPhotoEdit] 界面语言: %s → %s（%s）\n",
                 pref == zl::uilang::autoLanguage() ? "自动" : "指定",
                 qPrintable(lang),
                 loaded ? "已加载内嵌翻译" : "源码文本");
}

// ============================================================================
//  启动日志：把本机可用的图像格式打出来
// ============================================================================
//
//  ★ 为什么用 fprintf 而不是 qInfo/qDebug ★
//
//    实测（2026-10-12，Qt 5.15.16 + MSVC）：qInfo() 在 MSYS/Git Bash 的
//    重定向环境下会**静默丢失**——同一个程序在 PowerShell 下
//    qDebug/qInfo/qWarning/qCritical 全部正常输出，在 Git Bash 下
//    全部无声，连重定向到文件也抓不到。诊断行的全部价值就在于
//    "出问题时能看见"，所以这里与上面的版本行统一用 fprintf：
//    代价是少一个 QT_LOGGING_RULES 开关，收益是它在任何环境都出现。
//
//    用户最常见的抱怨之一是"这张图打不开"。可能的原因有三种：
//      ① 文件真的损坏
//      ② 这个格式的 Qt 插件没装（例如没装 WebP 插件）
//      ③ 我们的代码把它过滤掉了
//
//    把 QImageReader::supportedImageFormats() 打出来，就能一眼区分
//    ② 和 ③ —— 如果列表里没有 "webp"，那就是插件缺失，与我们的代码无关。
//    排查时间从"半小时"降到"一眼"。
static void logSupportedImageFormats()
{
    QStringList readFormats;
    const QList<QByteArray> rawRead = QImageReader::supportedImageFormats();
    readFormats.reserve(rawRead.size());
    for (const QByteArray &f : rawRead)
        readFormats << QString::fromLatin1(f);

    QStringList writeFormats;
    const QList<QByteArray> rawWrite = QImageWriter::supportedImageFormats();
    writeFormats.reserve(rawWrite.size());
    for (const QByteArray &f : rawWrite)
        writeFormats << QString::fromLatin1(f);

    std::fprintf(stderr, "[ZlPhotoEdit] 可读格式: %s\n",
                 qPrintable(readFormats.join(QStringLiteral(" "))));
    std::fprintf(stderr, "[ZlPhotoEdit] 可写格式: %s\n",
                 qPrintable(writeFormats.join(QStringLiteral(" "))));
}

// ============================================================================
//  main
// ============================================================================

int main(int argc, char *argv[])
{
    // ── 步骤 1：高 DPI（必须在 QApplication 之前）──
    setupHighDpi();

    // ── 步骤 2：QApplication ──
    QApplication app(argc, argv);

    // ── 步骤 2.5：应用程序图标 ──
    //
    //  ★ 这里设的是"窗口/任务栏"图标，与 exe 文件图标是两码事 ★
    //
    //    · 本行（setWindowIcon）：运行时生效，图来自 qrc 里的 logo.svg。
    //      窗口标题栏、任务栏、Alt+Tab 用的是它。
    //    · exe 文件图标：链接期由 rc.exe 把 resources/logo.ico 嵌进 PE
    //      资源段（见 resources/app.rc 与 .pro / CMakeLists.txt 里的接法）。
    //      资源管理器里那个文件的图标是它 —— **运行时改不了**。
    //
    //    两者同源（都来自用户那张 logo），所以看起来是一套。
    //
    //  ★ 必须在 QApplication 之后、建窗口之前 ★
    //    在 QApplication 之前设，Qt 会警告 "setWindowIcon must be called
    //    after QApplication" 且无效；在 MainWindow 之后设，窗口已经带着
    //    默认图标显示出来了（部分平台上不会刷新）。
    //
    //  ★ 失败是静默的 → 所以要有下面这条日志与自检 ★
    //    SVG 解析失败 / 资源没打进 qrc 时，AppInfo::windowIcon() 返回空
    //    QIcon，setWindowIcon(空) 不报任何错，只是没有图标。
    //    用一个 qInfo 把结论打出来，排查时一眼能看到。
    {
        const QIcon appIcon = zl::appinfo::windowIcon();
        QApplication::setWindowIcon(appIcon);
        if (appIcon.isNull()) {
            qWarning().noquote()
                << "[ZlPhotoEdit] 应用程序图标加载失败：logo.svg 缺失或无法渲染"
                   "（窗口与任务栏将显示系统默认图标）";
        }
    }

    // ★ 关于图标设置顺序的一个细节 ★
    //   setWindowIcon 只依赖 QApplication（QIcon 不需要 qrc 之外的任何东西），
    //   所以放在这里比放在 setOrganizationName 之后更贴题 —— 顺序无实质差别，
    //   但"图标"与"应用标识"是两件事，分开写更清楚。

    // ── 步骤 3：应用标识（必须在任何 QSettings 之前）──
    QCoreApplication::setOrganizationName(QString::fromLatin1(kOrganizationName));
    QCoreApplication::setApplicationName(QString::fromLatin1(kApplicationName));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(kApplicationVer));

    // ★ 关于 QApplication 构造顺序的一个细节 ★
    //   setOrganizationName 在 QApplication 构造**之后**调用是没问题的，
    //   只要它早于第一次使用 QSettings 即可。
    //   MainWindow 的构造函数里会读 QSettings，所以这里必须在前。

    // ── 步骤 4：编码 ──
    setupUtf8Encoding();

    // ── 步骤 4.5：界面语言 ──
    setupUiLanguage(&app);

    // ── 步骤 5：启动日志 ──
    //
    //  ★ 这一行不是"调试残留"，是刻意保留的 ★
    //    图像编辑程序在用户机器上出问题时（"打开某张图就崩"），
    //    第一件要知道的事就是"它跑在哪个 Qt 版本上"。
    //    Qt5 与 Qt6 的图片插件能力不同，很多"格式打不开"的问题
    //    其实是 Qt 版本的插件缺失而不是我们的 bug。
    //
    //    输出到 stderr：在 Windows GUI 模式下不可见（没有控制台），
    //    但从命令行启动 or 用调试器附加时能看到。
    //    代价是零，收益是排查问题时的第一手线索。
    std::fprintf(stderr,
                 "[ZlPhotoEdit %s] Qt %s | 编译期 Qt %s\n",
                 kApplicationVer,
                 qPrintable(zl::compat::qtVersionString()),
                 QT_VERSION_STR);

    // 打印本机可用的图像格式 —— 同样是为了排查"为什么打不开这个文件"。
    // 用 qInfo 而不是 fprintf，这样能用 QT_LOGGING_RULES 关掉。
    logSupportedImageFormats();

    // ── 步骤 6：主窗口 ──
    //
    // ★ 命令行参数 = 要打开的文件 ★
    //   `ZlPhotoEdit a.png b.png` 开两个标签、不额外送一个空白画布；
    //   不带参数时内部会回落到一个空白画布。
    //
    //   这一步从"多标签"才开始有意义 —— 单文档时代第二个参数只能被
    //   丢掉。Qt 已经把自己识别的选项（如 -style）从 arguments() 里
    //   摘掉了，所以这里剩下的基本都是文件路径。
    QStringList initialFiles = app.arguments();
    if (!initialFiles.isEmpty())
        initialFiles.removeFirst();     // 第 0 个是程序自身路径

    zl::MainWindow window(nullptr, initialFiles);
    window.show();

    return app.exec();
}
