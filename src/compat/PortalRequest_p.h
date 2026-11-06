#pragma once

// ============================================================================
//  PortalRequest_p.h —— 桌面门户（xdg-desktop-portal）请求的**应答收件箱**
// ----------------------------------------------------------------------------
//  门户的调用模型是**两段式**：方法立刻返回一个 request handle（对象路径），
//  真正的结果过一会儿从
//      Response(uint response, a{sv} results)
//  信号回来。这个类只做一件事：把那条信号收进两个字段，并叫停外层的等待循环。
//
//  截图（Screenshot）与全局热键（GlobalShortcuts）用的是同一套请求机制，
//  所以这个收件箱放在 compat 层共用 —— 各写一份的话，"成功码是 0"这种
//  约定就会有两个版本，早晚有一天只改了一边。
//
//  ★ 为什么单独一个头、而不是把类塞进 .cpp ★
//    D-Bus 的信号只能用 `const char *slot` 那种重载去收（Qt6 的
//    QDBusConnection 没有 functor 版本），接收者必须是一个带 Q_OBJECT
//    槽的 QObject。而 Q_OBJECT 要 moc 处理 —— 放在 .cpp 里靠
//    `#include "xxx.moc"` 也能跑，但本项目所有 moc 都由头文件驱动
//    （CMake 的 AUTOMOC 与 run_tests.sh 里那份手工 moc 清单都是照
//    HEADERS 列的），另起一种模式会让两套构建都多一处要记的地方。
//
//  ★ 整个文件被 `#if` 包住 ★
//    Windows / macOS 上留空。moc 对空内容只会产出一个空文件，
//    既不产生符号也不影响链接 —— 于是"别的平台不受影响"是结构上成立的，
//    不是靠记得去改清单。
// ============================================================================

#include <QtGlobal>

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_QTDBUS)

#  include <QDBusArgument>
#  include <QDBusMessage>
#  include <QEventLoop>
#  include <QMetaType>
#  include <QObject>
#  include <QString>
#  include <QVariant>
#  include <QVariantMap>

namespace zl {

// ":1.42" → "1_42"
//
// 门户返回的 handle 路径是
//   /org/freedesktop/portal/desktop/request/<发送方唯一名>/<handle_token>
// 唯一名里的 ':' 与 '.' 在对象路径中不合法，规范要求换成下划线。
// 我们据此**预先算出**路径并提前订阅 —— 截图与全局热键都要用这一条，
// 各写一份的话，某个角落写错一个字符就是"订阅了一个永远不来的信号"。
inline QString portalSenderToken(const QString &uniqueName)
{
    QString s = uniqueName;
    if (s.startsWith(QLatin1Char(':')))
        s.remove(0, 1);
    s.replace(QLatin1Char('.'), QLatin1Char('_'));
    return s;
}

class PortalResponseWaiter : public QObject
{
    Q_OBJECT

public:
    explicit PortalResponseWaiter(QObject *parent = nullptr)
        : QObject(parent) {}

    // 等谁被叫停。生命周期由调用方（栈上的 QEventLoop）保证更长。
    void attach(QEventLoop *loop) { m_loop = loop; }

    // 把一次请求重置回"还没应答"的初始态，便于同一个对象复用。
    void reset()
    {
        code     = 0xFFFFFFFFu;
        answered = false;
        results.clear();
    }

    // ★ 初值刻意是 0xFFFFFFFF 而不是 0 ★
    //   门户的成功码就是 0。若初值也用 0，一次"根本没收到应答"的超时
    //   会被当成成功 —— 然后拿着空 results 去要东西，拿到空值，
    //   最后表现成一次莫名其妙的失败，把"门户没回话"这件事彻底埋掉。
    uint        code     = 0xFFFFFFFFu;
    QVariantMap results;
    bool        answered = false;

public slots:
    void onResponse(const QDBusMessage &message)
    {
        const QList<QVariant> args = message.arguments();
        if (args.size() >= 1)
            code = args.at(0).toUInt();

        if (args.size() >= 2) {
            // a{sv} 落地时是 QDBusArgument，不能直接 value<QVariantMap>()。
            // 但不同 Qt 版本/路径下也可能已经解包成 QVariantMap，
            // 所以两种都接住 —— 猜错一种的后果是"results 永远是空的"。
            const QVariant payload = args.at(1);
            if (payload.metaType() == QMetaType::fromType<QDBusArgument>())
                payload.value<QDBusArgument>() >> results;
            else
                results = payload.toMap();
        }

        answered = true;
        if (m_loop)
            m_loop->quit();
    }

private:
    QEventLoop *m_loop = nullptr;
};

} // namespace zl

#endif   // Q_OS_LINUX && ZLPE_HAVE_QTDBUS
