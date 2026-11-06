#pragma once

// ============================================================================
//  GlobalHotkey.h —— 应用不在前台时也能收到的那类快捷键（"全局热键"）
// ----------------------------------------------------------------------------
//  需求场景很简单：程序最小化到任务栏之后，用户按一下快捷键仍然能截图。
//
//  ★ 为什么必须碰系统 API ★
//    Qt 的 QShortcut / QAction::setShortcut 只在**本程序有焦点时**有效 ——
//    窗口最小化之后键盘事件根本不进本进程，Qt 层面无论怎么写都办不到。
//    必须向操作系统登记：Windows 上是 RegisterHotKey，按下的那一下系统
//    会把一条 WM_HOTKEY 投进本线程的消息队列（哪怕窗口最小化、在后台）。
//
//  ★ 平台判断只允许出现在本文件与 ScreenCapture ★
//    与 QtCompat.h 同一条约定：`#if` 集中在 src/compat/，其余代码不写。
//    非 Windows 平台上本类**编译得过、构造函数可用**，只是
//    registerSequence() 返回 false 并给出说明 —— 于是上层无需任何平台判断，
//    也永远不会"某天在别的平台上编译不过"。
//
//  ★ 失败必须能说清原因（本类的核心纪律）★
//    全局热键是**稀缺资源**：同一个组合键全系统只能被一个进程登记，
//    被 QQ / 微信 / 截图工具占着时 RegisterHotKey 直接失败。
//    而失败的症状是"按了没反应" —— 与"程序坏了"完全无法区分。
//    所以本类把失败做成一等公民：返回 false + lastError() 里一句
//    **可以直接给用户看**的话，由上层负责说出来（★ 绝不静默）。
// ============================================================================

#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QKeySequence>
#include <QObject>
#include <QString>
#include <QtGlobal>

// 只在 X11 分支里用到，且只用指针 —— 前向声明即可，不必把
// QtCore 的头部再拉一份进来。
class QSocketNotifier;

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_QTDBUS)
// 门户信号的槽参数要用到它们的完整类型（Qt 要据此做 D-Bus 类型转换），
// 前向声明在 moc 生成的胶水代码里不够用。
#  include <QDBusObjectPath>
#  include <QVariantMap>
#endif

namespace zl {

// ★ 唯一的版本判断，且就在 compat 里 ★
//   Qt 自己的 qabstractnativeeventfilter.h 也是这么写的：
//   Qt5 的 result 形参是 `long *`，Qt6 换成了 `qintptr *`。
//   两者不能共用一个签名（override 必须完全一致），只能分版本。
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
using NativeEventResult = qintptr;
#else
using NativeEventResult = long;
#endif

class GlobalHotkey : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // ── 登记 / 注销 ──
    //
    // id 由调用方给（同一个 id 重复登记 = 覆盖掉上一次）。
    // 用一个显式 id 而不是"返回一个句柄"，是因为自检与截图工具需要
    // **能自己造一条 WM_HOTKEY** 来驱动这条通路（见 simulateActivation），
    // 而那需要它知道 id 是多少 —— 句柄式的 API 在这里反而更难用。
    //
    // 返回 false 时 lastError() 一定有话说（这是契约）：
    //   · 平台不支持 / 没跑在 QApplication 里
    //   · 组合键里没有可用的主键（例如只按了 Ctrl）
    //   · 主键不在支持表里（本类不追求覆盖全部按键，见 .cpp 的映射表）
    //   · ★ 系统里已经有别的程序占着这个组合键（最常见的失败原因）★
    bool registerSequence(const QKeySequence &sequence, int id);
    void unregister(int id);
    void unregisterAll();

    bool isRegistered(int id) const;
    int  registeredCount() const { return m_regs.size(); }

    // 某个 id 上**实际登记成功**的那个组合键（没登记上就是空 QKeySequence）。
    //
    // ★ 为什么需要读回来 ★
    //   上层可能"首选键被占用了就换一个"（见 MainWindow 的候选表）。
    //   那时如果界面还显示首选键，用户拿到的就是一把**错的钥匙** ——
    //   菜单写着 Ctrl+Alt+S、真正管用的是另一个键，这种错最难自查。
    //   所以界面上显示的键必须从这里取，而不是从"我们本来想要哪个"取。
    QKeySequence registeredSequence(int id) const { return m_regs.value(id); }

    // 最后一次失败的原因（可以直接给用户看）。成功时为空。
    QString lastError() const { return m_lastError; }

    // ── 自动化入口（仅自检使用）──────────────────────────────────────────
    //
    // 向**本线程**的消息队列投一条 WM_HOTKEY，效果等同于"用户真的按下了
    // 那个组合键"。它走的是与真实按键**完全同一条**后半段通路：
    //   消息队列 → Qt 事件循环 → nativeEventFilter → activated()
    // 只跳过 RegisterHotKey 那一步（那一步没法在自检里自动化 ——
    // 总不能真的去按键盘）。
    //
    // ★ 为什么值得留这个口子 ★
    //   "WM_HOTKEY 到底会不会被 Qt 交到 nativeEventFilter 手上"是本功能
    //   唯一没把握的地方（它取决于 Qt 事件循环对线程消息的处理）。
    //   没有这条通路，这个不确定性就只能靠"用户按一下试试"来验证。
    bool simulateActivation(int id);

    // 当前平台能不能伪造一次"热键被按下"。
    //
    // ★ 为什么要把这件事做成查询，而不是让调用方自己去猜 ★
    //   simulateActivation() 在不能伪造的平台上**如实返回 false**（见 .cpp），
    //   于是"返回 false"有两种含义：真的失败，或这个平台做不到。
    //   自检拿到后者时会把它当成失败去报 —— 一条永远红着的用例比没有更糟。
    //   有了这个查询，自检能明确区分"没通过"与"这里不适用"。
    static bool supportsActivationSimulation();

signals:
    // 某个已登记的热键被按下（系统投来了 WM_HOTKEY）。
    // 参数是登记时给的 id —— 调用方据此区分是哪一个热键。
    void activated(int id);

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           NativeEventResult *result) override;

private:
    //  id → 本次登记的原始组合键（只用于可读性与注销时的核对）
    QHash<int, QKeySequence> m_regs;

    QString m_lastError;

#if defined(Q_OS_LINUX)
    // Linux 有两条完全不同的路：X11 用 XGrabKey，Wayland 必须走桌面门户。
    // 选哪条由**运行时**的会话类型决定，不是编译期 —— 同一份二进制
    // 既可能在 X11 上跑也可能在 Wayland 上跑。
    static bool isWaylandSession();
#endif

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_QTDBUS)
    // ── Wayland：org.freedesktop.portal.GlobalShortcuts ──
    bool registerViaPortal(const QKeySequence &sequence, int id);
    bool ensurePortalSession(QString *error);
    void closePortalSession();

    QString m_portalSession;                 // session handle（对象路径）
    QHash<QString, int> m_portalShortcuts;   // shortcut_id → 调用方的 id
    bool m_portalSubscribed = false;         // Activated 信号是否已接上
    bool m_portalSessionActive = false;      // session 是否已建好并可用

private slots:
    // ★ 参数类型必须与门户信号的签名逐个对上，否则 Qt 会把事件丢掉 ★
    //   Activated(o session_handle, s shortcut_id, t timestamp, a{sv} options)
    //   —— t 是无符号 64 位，在 C++ 侧就是 qulonglong。
    void onPortalActivated(const QDBusObjectPath &session,
                           const QString &shortcutId,
                           qulonglong timestamp,
                           const QVariantMap &options);

private:
#endif

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_X11)
    // ── X11：XGrabKey ──
    bool registerViaX11(const QKeySequence &sequence, int id);

    // ------------------------------------------------------------------
    //  X11 实现用到的状态
    // ------------------------------------------------------------------
    //  ★ 这里用 `void *` 存 Display，而不是 include <X11/Xlib.h> ★
    //    这个头在 Windows / macOS 上根本不存在；把它引进来会把一份
    //    "只属于 Linux"的依赖泄漏到所有翻译单元里（X11 的宏出了名地
    //    爱和别的头冲突，`Bool` / `None` / `Status` 都是常见撞车点）。
    //    代价只是每次用时 cast 一下，而这个文件里只有三处。
    void *m_x11Display = nullptr;

    // 独立 X 连接上的"有事件可读"提醒。
    // ★ 必须是**另一个**连接 ★ 用 Qt 自己那条会与 Qt 的事件循环抢事件，
    //   抢输的那一方表现为"快捷键偶尔不灵"。
    QSocketNotifier *m_x11Notifier = nullptr;

    // id → 当前键码。键盘布局一改键码就会变，所以它必须能和 m_regs 分开
    // 重算（MappingNotify 到了就整体重抓，见 .cpp）。
    QHash<int, quint32> m_x11KeyCodes;

    bool openX11();
    void closeX11();
    void pumpX11Events();
    void regrabAllX11();
    void grabX11Key(quint32 keySym, quint32 mods);
    void ungrabX11Key(quint32 keySym, quint32 mods);
#endif

    Q_DISABLE_COPY(GlobalHotkey)
};

} // namespace zl
