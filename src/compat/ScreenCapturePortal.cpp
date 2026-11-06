#include "compat/ScreenCapture.h"
#include "compat/PortalRequest_p.h"

#include <QImage>

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_QTDBUS)

#  include <QAtomicInt>
#  include <QCoreApplication>
#  include <QDBusArgument>
#  include <QDBusConnection>
#  include <QDBusMessage>
#  include <QDBusObjectPath>
#  include <QEventLoop>
#  include <QFile>
#  include <QTimer>
#  include <QUrl>
#  include <QVariantMap>

namespace zl {

namespace {

const QString kPortalService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath    = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kOptionsPath   = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kShotIface     = QStringLiteral("org.freedesktop.portal.Screenshot");
const QString kRequestIface  = QStringLiteral("org.freedesktop.portal.Request");

// 调方法本身的超时。门户是被 D-Bus 按需激活的，第一次可能要拉起后端进程，
// 所以不能给得太紧；但它毕竟只是"看一眼服务在不在"，也不需要太久。
constexpr int kCallTimeoutMs = 4000;

// 等 Response 的上限。
//
// ★ 这个值同时是"用户在多长时间内可以决定"和"界面最多被冻多久" ★
//   non-interactive 的门户截图通常 1 秒内就回来；给到 20 秒是为了兜住
//   "后端插入了一次授权对话框、用户正在犹豫"这种情况 —— 超过这个时间
//   还当作有人会用，只会让一次卡住变成永久的卡住。
constexpr int kResponseTimeoutMs = 20000;

//   handle 路径里"发送方唯一名"的写法由 PortalRequest_p.h 的
//   portalSenderToken() 负责 —— 全局热键那边也要用同一条规则。

// 上一次失败的说明。空 = 没有额外信息。
//
// ★ 用静态变量而不是返回值一条链传出去 ★
//   grabScreen() 的契约（返回 QImage、失败给空图）是给上层的稳定接口，
//   为了带一句"为什么"就把它改成返回结构体，会牵连每一个调用点。
//   这条信息只在上层"已经决定要报错"时才用得上，独立于主返回值最省事。
QString g_lastFailure;

// 自由函数里的翻译。上下文名固定成 "zl::ScreenCapture" ——
// 手工维护 .ts 时按这个 context 找。
QString trPortal(const char *text)
{
    return QCoreApplication::translate("zl::ScreenCapture", text);
}

} // namespace

// ---------------------------------------------------------------------------
//  这条路通不通
// ---------------------------------------------------------------------------
//  ★ 为什么查 version 属性，而不是"服务名在不在" ★
//    服务名在只说明 xdg-desktop-portal 这个转发层活着。它背后必须有一个
//    桌面相关的后端（xdg-desktop-portal-gnome / -kde / -wlr）真正实现了
//    Screenshot。只装了转发层时，服务名照样能查到，但它会把请求转给一个
//    不存在的后端 —— 于是"检查通过、调用无声无息地失败"。
//    Properties.Get("version") 由实现了该接口的后端提供，问得到就说明
//    接口真的在。
bool screenCapturePortalAvailable()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    QDBusMessage msg = QDBusMessage::createMethodCall(
        kPortalService, kOptionsPath,
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    msg << kShotIface << QStringLiteral("version");

    const QDBusMessage reply = bus.call(msg, QDBus::Block, kCallTimeoutMs);
    return reply.type() == QDBusMessage::ReplyMessage;
}

// ---------------------------------------------------------------------------
//  抓
// ---------------------------------------------------------------------------
QImage grabScreenViaPortal()
{
    // 每次进来先清空：上一次的失败原因留到下一次会让"这次为什么失败"
    // 指向一个早已过去的事件。
    g_lastFailure.clear();

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        g_lastFailure = trPortal("Cannot reach the desktop session bus.");
        return QImage();
    }

    // ★ 重入保护 ★
    //   这条路径会开一个嵌套事件循环（要等 Response）。嵌套期间菜单/热键
    //   仍然可能被再次触发 —— 那会再开一层循环、再发一次门户请求，
    //   多出来的截图会叠进下一次调用里。挡在门口最省事。
    static bool s_busy = false;
    if (s_busy)
        return QImage();
    s_busy = true;
    struct BusyGuard {
        bool &flag;
        ~BusyGuard() { flag = false; }
    } busyGuard{s_busy};

    static QAtomicInt sequence{0};
    const QString token =
        QStringLiteral("zlpe_shot_%1").arg(sequence.fetchAndAddRelaxed(1));

    const QString expectPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
            .arg(portalSenderToken(bus.baseService()), token);

    PortalResponseWaiter waiter;
    QEventLoop loop;
    waiter.attach(&loop);

    // ★★ 先订阅，后调用 ★★
    //   门户在"图已经截好"时会**立刻**发 Response，而 handle 是随方法返回
    //   一起给出的 —— 两者之间没有先后保证。先调用再订阅的话，快机器上
    //   信号会在订阅之前就发完，于是永远等不到（铁律第九章：连接建立之前
    //   就可能已经发生的事，顺序必须钉死）。
    auto subscribe = [&](const QString &path) {
        return bus.connect(kPortalService, path, kRequestIface,
                           QStringLiteral("Response"), &waiter,
                           SLOT(onResponse(QDBusMessage)));
    };
    subscribe(expectPath);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        kPortalService, kOptionsPath, kShotIface, QStringLiteral("Screenshot"));

    QVariantMap options;
    options.insert(QStringLiteral("handle_token"), token);
    // false = 让合成器直接截，不弹"选区域/选窗口"那套交互界面。
    // 代价是**没法指定截哪一块屏**（规范里没有这个参数），多屏时给什么
    // 由后端决定 —— 这一点由 grabScreen() 那边的尺寸对账兜住。
    options.insert(QStringLiteral("interactive"), false);
    options.insert(QStringLiteral("modal"), false);

    msg << QString() << options;   // parent_window 留空：不依附具体窗口

    const QDBusMessage reply = bus.call(msg, QDBus::Block, kCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        g_lastFailure = trPortal("The desktop portal did not answer the capture request.");
        return QImage();
    }

    // 后端版本不同时返回的 handle 可能与规范推出来的路径不一致
    // （老版本用 /request/<唯一名>/<token>，个别实现带别的段）。
    // 补一次订阅是幂等的：同一条信号被连两次也只会各自收一份。
    const QString handle =
        reply.arguments().value(0).value<QDBusObjectPath>().path();
    if (!handle.isEmpty() && handle != expectPath)
        subscribe(handle);

    if (!waiter.answered) {
        QTimer::singleShot(kResponseTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }

    // 0 = 用户同意且截成功；1 = 用户取消；2 = 其它错误。三者都不给图。
    //
    // ★ 三种必须分开说 ★
    //   "没收到应答"和"用户明确拒绝"对用户意味着完全不同的下一步：
    //   前者可以重试，后者重试多少次都一样。压成一句"截图失败"，
    //   用户只会一直点。
    if (!waiter.answered) {
        g_lastFailure = trPortal("The desktop portal did not respond in time. "
                                 "The screen capture request was dropped.");
        return QImage();
    }
    if (waiter.code != 0) {
        g_lastFailure = (waiter.code == 1)
            ? trPortal("The screen capture request was declined.")
            : trPortal("The desktop portal reported an error while capturing the screen.");
        return QImage();
    }

    const QString uri = waiter.results.value(QStringLiteral("uri")).toString();
    if (uri.isEmpty()) {
        g_lastFailure = trPortal("The desktop portal returned no image.");
        return QImage();
    }

    // ★ 门户把图写在 $XDG_RUNTIME_DIR 下，是一个**临时文件** ★
    //   读走必须删掉：不然用户每截一次就多一份完整屏幕图悄悄留在内存盘里，
    //   既不回收也不可见 —— 属于"看不见的泄漏"，还会被别的东西读到。
    const QString localPath = QUrl(uri).toLocalFile();
    if (localPath.isEmpty()) {
        g_lastFailure = trPortal("The desktop portal returned an unreadable location.");
        return QImage();
    }

    QImage img(localPath);
    QFile::remove(localPath);

    if (img.isNull()) {
        g_lastFailure = trPortal("The captured image could not be read.");
        return QImage();
    }

    // 门户给的是"屏幕上的像素"，与本地抓屏同语义：设备像素 + dpr 归一。
    img.setDevicePixelRatio(1.0);
    return img;
}

QString screenCaptureLastFailure()
{
    return g_lastFailure;
}

} // namespace zl

#else   // ── 非 Linux / 没编进 QtDBus：这条路不存在，安静地不存在 ──

namespace zl {

// ★ 返回 false 而不是"抛错/打日志" ★
//   这两条在 Windows / macOS 上**是正常的** —— 那些平台用本地抓屏，
//   从来不走门户。把它们当成异常来报，只会在正常平台上刷出无意义的告警。
bool screenCapturePortalAvailable()
{
    return false;
}

QImage grabScreenViaPortal()
{
    return QImage();
}

// 这些平台上抓屏走本地接口，不存在"门户为什么拒绝"这回事 ——
// 永远没有额外说明，上层自然用通用文案。
QString screenCaptureLastFailure()
{
    return QString();
}

} // namespace zl

#endif
