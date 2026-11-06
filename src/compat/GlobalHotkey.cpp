#include "compat/GlobalHotkey.h"

#include <QCoreApplication>
#include <QDebug>

#if defined(Q_OS_LINUX)
// 判断会话类型（X11 / Wayland）只能问 QGuiApplication::platformName()
#  include <QGuiApplication>
#endif

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_QTDBUS)
// Wayland 下的全局热键走桌面门户，机制与截图那套是同一个收件箱
#  include "compat/PortalRequest_p.h"

#  include <QAtomicInt>
#  include <QDBusArgument>
#  include <QDBusConnection>
#  include <QDBusMessage>
#  include <QDBusMetaType>
#  include <QDBusObjectPath>
#  include <QEventLoop>
#  include <QStringList>
#  include <QTimer>
#  include <QVariantMap>
#endif

#ifdef Q_OS_WIN
//  ★ 包含 windows.h 之前的两条惯例 ★
//    WIN32_LEAN_AND_MEAN：砍掉 winsock1 之类的老古董，减少与 Qt 头文件的冲突面
//    NOMINMAX：不要定义 min/max 宏 —— 否则任何 `std::min(...)` /
//    `qMin(...)` 都会被宏替换成一段语法垃圾，报错位置还指向 Qt 头文件内部，
//    极难定位。
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

// ★ 链接 user32 ★
//   RegisterHotKey / UnregisterHotKey / PostThreadMessage 都在 user32 里，
//   而 MSVC 的默认库不含它 —— Qt5Widgets.lib 是**导入库**，它的 user32
//   依赖在运行期由 Qt5Widgets.dll 自己满足，帮不到我们这些目标文件。
//
//   ★ 为什么用 #pragma comment，而不是在 .pro / CMakeLists.txt /
//     run_tests.sh 里各加一行 ★
//     · 那三处是**三份手工维护的清单**，而自检的链接行尤其容易漏
//       （本工程已经在这类"清单没同步"上栽过好几次：漏了只在链接期报
//       "无法解析的外部符号"，看起来像少编译了文件）。
//     · 写在这里，谁读这个文件就看到了"它依赖 Win32"—— 不可能漏。
//   MinGW 会忽略这条 pragma（并给一条警告）；本工程用 MSVC 构建。
#  ifdef _MSC_VER
#    pragma comment(lib, "user32.lib")
#  endif

// 老版本 SDK 没有这个标志（Win7 起才有）
#  ifndef MOD_NOREPEAT
#    define MOD_NOREPEAT 0x4000
#  endif
#endif

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_X11)
//  ★ X11 的两个头都要 ★
//    Xlib.h 给连接与抓键，keysym.h 给 XK_F1 这一组常量 ——
//    后者常被漏掉，然后 XK_Print 之类变成未定义标识符。
#  include <X11/Xlib.h>
#  include <X11/keysym.h>

#  include <QSocketNotifier>
#endif

namespace zl {

#ifdef Q_OS_WIN

// ---------------------------------------------------------------------------
//  Qt::Key → Win32 虚拟键码（VK_*）
// ---------------------------------------------------------------------------
//  ★ 为什么是一张"有限支持"的表，而不是查遍全部按键 ★
//    全局热键的合理候选就那么几类：字母、数字、功能键、几个编辑键。
//    剩下的（多媒体键、输入法键、厂商自定义键）各家驱动不一样，
//    硬猜一个 VK 出来会让 RegisterHotKey 静默登记到一个**别的键**上 ——
//    用户按 A 没反应、按 B 反而截图了，这种错法比"不支持"糟糕得多。
//    所以不认识的一律返回 0，由调用方**响亮地拒绝**。
static quint32 virtualKeyFor(int key)
{
    // 字母与数字：Qt::Key_A..Z 就是 'A'..'Z'，数值上等于 VK 码；数字同理。
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return quint32(key);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return quint32(key);

    // 功能键：VK_F1 == 0x70，而 Qt::Key_F1 == 0x01000030 —— 一一对应（含 F13~F24）
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return 0x70u + quint32(key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Print:     return VK_SNAPSHOT;
    case Qt::Key_Insert:    return VK_INSERT;
    case Qt::Key_Delete:    return VK_DELETE;
    case Qt::Key_Home:      return VK_HOME;
    case Qt::Key_End:       return VK_END;
    case Qt::Key_PageUp:    return VK_PRIOR;
    case Qt::Key_PageDown:  return VK_NEXT;
    case Qt::Key_Left:      return VK_LEFT;
    case Qt::Key_Up:        return VK_UP;
    case Qt::Key_Right:     return VK_RIGHT;
    case Qt::Key_Down:      return VK_DOWN;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return VK_RETURN;
    case Qt::Key_Escape:    return VK_ESCAPE;
    case Qt::Key_Space:     return VK_SPACE;
    case Qt::Key_Tab:       return VK_TAB;
    case Qt::Key_Backspace: return VK_BACK;
    case Qt::Key_Pause:     return VK_PAUSE;
    default:                return 0;
    }
}

// Qt 的修饰键 → MOD_* 位标志
static UINT winModifiersFor(Qt::KeyboardModifiers mods)
{
    UINT m = 0;
    if (mods & Qt::ControlModifier)  m |= MOD_CONTROL;
    if (mods & Qt::AltModifier)      m |= MOD_ALT;
    if (mods & Qt::ShiftModifier)    m |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)     m |= MOD_WIN;
    return m;
}

#endif   // Q_OS_WIN

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_X11)

// ---------------------------------------------------------------------------
//  Qt::Key → X11 KeySym → 键码
// ---------------------------------------------------------------------------
//  ★ 为什么不能直接把 Qt::Key 当键码用 ★
//    X 用的是两级映射：keysym（“符号”，与键盘布局无关）→ keycode
//    （“这个键盘上的第几个键”）。同一个 keysym 在美式布局和法语布局上
//    的 keycode 不一样，而 XGrabKey 收的是 keycode。
//    所以必须过一次 XKeysymToKeycode —— 直接传 keysym 会抓到一个
//    **毫不相干的键**上，按 A 没反应、按别的键反而截图了。
//
//  ★ 支持的按键范围与 Windows 那张表保持一致 ★
//    两个平台能用同一批键，用户换系统时不会突然发现"这个组合键在
//    Linux 上没用了"。不认识的一律返回 0，由调用方响亮地拒绝。
static quint32 x11KeySymFor(int key)
{
    // 字母与数字：XK_A..Z / XK_0..9 的数值等于 ASCII，可直接转换
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return quint32(key);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return quint32(key);

    // 功能键：XK_F1 起连续排列
    if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
        return XK_F1 + quint32(key - Qt::Key_F1);

    switch (key) {
    case Qt::Key_Print:     return XK_Print;
    case Qt::Key_Insert:    return XK_Insert;
    case Qt::Key_Delete:    return XK_Delete;
    case Qt::Key_Home:      return XK_Home;
    case Qt::Key_End:       return XK_End;
    case Qt::Key_PageUp:    return XK_Prior;
    case Qt::Key_PageDown:  return XK_Next;
    case Qt::Key_Left:      return XK_Left;
    case Qt::Key_Up:        return XK_Up;
    case Qt::Key_Right:     return XK_Right;
    case Qt::Key_Down:      return XK_Down;
    case Qt::Key_Return:
    case Qt::Key_Enter:     return XK_Return;
    case Qt::Key_Escape:    return XK_Escape;
    case Qt::Key_Space:     return XK_space;
    case Qt::Key_Tab:       return XK_Tab;
    case Qt::Key_Backspace: return XK_BackSpace;
    case Qt::Key_Pause:     return XK_Pause;
    default:                return 0;
    }
}

// Qt 的修饰键 → X11 的修饰位
//
//  ★ Mod1 / Mod4 是"约定"而不是规范 ★
//    几乎所有的窗口管理器和键盘配置都把 Alt 映射到 Mod1、Super 映射到
//    Mod4，但 X 本身没有承诺这件事。真要严谨得去查修饰键映射表
//    （XGetModifierMapping）。这里按事实标准来：查表的代价换不来
//    更好的兼容性 —— 不按这个约定的桌面，连窗口管理器都跑不起来。
static quint32 x11ModifiersFor(Qt::KeyboardModifiers mods)
{
    quint32 m = 0;
    if (mods & Qt::ControlModifier)  m |= ControlMask;
    if (mods & Qt::AltModifier)      m |= Mod1Mask;
    if (mods & Qt::ShiftModifier)    m |= ShiftMask;
    if (mods & Qt::MetaModifier)     m |= Mod4Mask;
    return m;
}

// X 的错误是**异步**报的：XGrabKey 不会立刻返回失败，错误事件要等到
// 下一次 XSync 才冒出来。要判断"这个键被别人占着"，只能临时换上一个
// 只记录 BadAccess 的处理器，抓完 XSync 一次，再换回去。
//
// ★ 全局变量，因为 XErrorHandler 的函数签名里没有"用户数据"这一格 ★
//   代价是它不是线程安全的 —— 但抓键只发生在 GUI 线程，且这段区间
//   只包着几次 XGrabKey，没有并发窗口。
static bool s_x11GrabBadAccess = false;

static int x11GrabErrorHandler(Display *, XErrorEvent *event)
{
    if (event && event->error_code == BadAccess)
        s_x11GrabBadAccess = true;
    return 0;
}

#endif   // Q_OS_LINUX && ZLPE_HAVE_X11

// ============================================================================
//  GlobalHotkey
// ============================================================================

#if defined(Q_OS_LINUX)

// ★ 为什么要问平台名，而不是看 $XDG_SESSION_TYPE / $WAYLAND_DISPLAY ★
//   环境变量是"启动这个进程的 shell"留下的，可以被继承、被覆盖、
//   在 sudo / 从 IDE 启动 / 从 .desktop 启动时各不相同。
//   platformName() 是 Qt **实际用的那个平台插件**的名字 ——
//   它才是"我们现在到底在跟谁打交道"的权威答案。
//   两者不一致时信它，不信环境变量。
bool GlobalHotkey::isWaylandSession()
{
    if (!QGuiApplication::instance())
        return false;
    return QGuiApplication::platformName().contains(
        QStringLiteral("wayland"), Qt::CaseInsensitive);
}

#endif   // Q_OS_LINUX

GlobalHotkey::GlobalHotkey(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    // ★ 把本对象挂到 QCoreApplication 的原生事件过滤器上 ★
    //   这是"系统投来的消息"进入 Qt 世界的唯一入口。
    //   install 必须在 registerSequence 之前完成（这里在构造里做，
    //   而登记是后面的显式调用）—— 反过来的话，注册成功之后的第一次
    //   按键会落进真空：消息到了，但没人接。
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
#elif defined(Q_OS_LINUX) && defined(ZLPE_HAVE_X11)
    // ── X11：连接**按需建立** ──
    //   不在构造里连，因为构造发生在 MainWindow 装配早期，那时可能还没有
    //   任何一次"用户真的想要全局热键"的调用。XOpenDisplay 本身也会在
    //   没有 $DISPLAY 时报错刷屏 —— 而"没有 X 会话"是完全正常的情况
    //   （Wayland、纯终端、自检的 offscreen）。真正的连接推迟到
    //   registerSequence 里，届时失败可以说清楚"为什么"。
#else
    m_lastError = tr("Global hotkeys are not supported on this platform.");
#endif
}

GlobalHotkey::~GlobalHotkey()
{
    unregisterAll();

#ifdef Q_OS_WIN
    // 松手要对称：过滤器挂在 QCoreApplication 上，而它比本对象活得久。
    // 不摘的话，本对象析构后 Qt 仍会把消息转给它 —— 典型的 use-after-free。
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
#endif

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_X11)
    closeX11();
#endif
}

bool GlobalHotkey::registerSequence(const QKeySequence &sequence, int id)
{
    m_lastError.clear();

#ifdef Q_OS_WIN

    if (!QCoreApplication::instance()) {
        m_lastError = tr("No QApplication instance - cannot register the global hotkey.");
        return false;
    }
    if (sequence.isEmpty()) {
        m_lastError = tr("The key sequence is empty.");
        return false;
    }

    // ★ QKeySequence 的取项 ★
    //   Qt5 的 operator[] 返回 int（键码 | 修饰位），Qt6 换成了 QKeyCombination。
    //   本工程当前按 Qt5.15 编译；将来若要上 Qt6，只需在这一行分版本
    //   （QKeyCombination 有 key() / keyboardModifiers()）。
    const int combined = int(sequence[0]);
    const Qt::KeyboardModifiers mods =
        Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask);

    const QString seqText = sequence.toString(QKeySequence::NativeText);

    const quint32 vk = virtualKeyFor(combined & ~Qt::KeyboardModifierMask);
    if (vk == 0) {
        m_lastError = tr("The main key of %1 is out of the supported "
                         "range - try another (letters / digits / "
                         "F1~F24 / cursor keys, etc.).").arg(seqText);
        return false;
    }

    const UINT winMods = winModifiersFor(mods);
    if (winMods == 0) {
        // ★ 单键的全局热键一律不收 ★
        //   登记一个裸的 'S' 意味着**全系统**那个键都被本程序吃掉：
        //   用户在任何一个程序里打字都不会出 S。这不是"功能"，是事故。
        //   只有极少数专门做这个的软件才那样干，而它们要用户明确确认。
        m_lastError = tr("The global hotkey %1 needs at least one modifier (Ctrl / Alt / Shift).")
                          .arg(seqText);
        return false;
    }

    // 同一个 id 重复登记 = 覆盖上一次（不做"两个都生效"—— 那只会让
    // 上层多出一个"忘记注销"的机会）
    unregister(id);

    // ★ MOD_NOREPEAT 必须带上 ★
    //   不带的后果：按住快捷键不放会**连续触发**（系统自动重复），
    //   一秒钟截出几十张图。这一点在"按一下没反应就多按一会儿"的用户
    //   身上特别容易触发。
    if (!RegisterHotKey(nullptr, id, winMods | MOD_NOREPEAT, vk)) {
        const DWORD err = ::GetLastError();

        if (err == ERROR_HOTKEY_ALREADY_REGISTERED) {
            // 实测里最常见的失败原因：被 QQ / 微信 / 截图工具 / 输入法占了
            m_lastError = tr("The hotkey %1 is already taken by another "
                             "program (a combination can only be "
                             "registered by one program system-wide).")
                              .arg(seqText);
        } else {
            m_lastError = tr("Failed to register the global hotkey %1 (system error code %2).")
                              .arg(seqText).arg(uint(err));
        }
        return false;
    }

    m_regs.insert(id, sequence);
    return true;

#elif defined(Q_OS_LINUX)   // ── Linux：X11 与 Wayland 是两条完全不同的路 ──

    if (!QCoreApplication::instance()) {
        m_lastError = tr("No QApplication instance - cannot register the global hotkey.");
        return false;
    }
    if (sequence.isEmpty()) {
        m_lastError = tr("The key sequence is empty.");
        return false;
    }

    // ★ 选哪条路由**运行时**决定，不是编译期 ★
    //   同一份二进制既可能跑在 X11 上也可能跑在 Wayland 上
    //   （Ubuntu 登录界面就能切换）。按编译期宏二选一的话，
    //   为 Wayland 编的包在 X11 上会直接说"不支持"。
    if (isWaylandSession()) {
#if defined(ZLPE_HAVE_QTDBUS)
        return registerViaPortal(sequence, id);
#else
        m_lastError = tr("This Wayland session needs the desktop portal "
                         "(QtDBus), which this build does not include.");
        return false;
#endif
    }

#if defined(ZLPE_HAVE_X11)
    return registerViaX11(sequence, id);
#else
    m_lastError = tr("This X11 session needs Xlib support, which this build "
                     "does not include.");
    return false;
#endif

#else   // ── 其余平台：编译得过，但明确地不支持 ──

    Q_UNUSED(sequence)
    Q_UNUSED(id)
    m_lastError = tr("Global hotkeys are not supported on this platform.");
    return false;

#endif
}

void GlobalHotkey::unregister(int id)
{
    if (!m_regs.contains(id))
        return;

#ifdef Q_OS_WIN
    ::UnregisterHotKey(nullptr, id);
#elif defined(Q_OS_LINUX)
    if (isWaylandSession()) {
#if defined(ZLPE_HAVE_QTDBUS)
        // ★ 门户没有"解绑其中一个快捷键"的方法，只有 Close 整个 session ★
        //   所以注销一个就等于关掉整场。本项目只登记一个热键，代价为零；
        //   将来若真要登记多个，这里得改成"关掉再按剩下的重建"。
        if (m_portalSessionActive)
            closePortalSession();
#endif
    } else {
#if defined(ZLPE_HAVE_X11)
        if (m_x11Display) {
            const QKeySequence seq = m_regs.value(id);
            const int combined = int(seq[0]);
            const quint32 keySym =
                x11KeySymFor(combined & ~Qt::KeyboardModifierMask);
            const quint32 mods = x11ModifiersFor(
                Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask));
            if (keySym && mods) {
                ungrabX11Key(keySym, mods);
                XFlush(static_cast<Display *>(m_x11Display));
            }
        }
        m_x11KeyCodes.remove(id);
#endif
    }
#endif

    m_regs.remove(id);
}

void GlobalHotkey::unregisterAll()
{
    const QList<int> ids = m_regs.keys();
    for (int id : ids)
        unregister(id);
}

bool GlobalHotkey::isRegistered(int id) const
{
    return m_regs.contains(id);
}

bool GlobalHotkey::supportsActivationSimulation()
{
#ifdef Q_OS_WIN
    // PostThreadMessage 投进去的 WM_HOTKEY 与真实按键走同一条后半段通路
    return true;
#else
    // X11 的被动抓键只认真实按键（XSendEvent 造的带 send_event 标记，
    // 会被 grab 忽略）；Wayland 更没有应用侧入口。
    return false;
#endif
}

bool GlobalHotkey::simulateActivation(int id)
{
#ifdef Q_OS_WIN
    // ★ 刻意不检查"是否登记过" ★
    //   未登记的 id 投进去，正好用来验证"没登记的 id 不该触发任何东西"
    //   （自检里的负向用例）。在这里先挡掉的话，那条用例就变成了
    //   "函数返回 false" —— 什么都没测到。
    return ::PostThreadMessageW(::GetCurrentThreadId(), WM_HOTKEY,
                                WPARAM(id), 0) != FALSE;
#elif defined(Q_OS_LINUX)
    // ── Linux 下这条口子走不通，如实说不 ──
    //    Windows 能用 PostThreadMessage，是因为热键最终以一条消息进线程队列，
    //    而那类消息我们自己造的也照样会被分发。
    //    · X11：被动抓键（XGrabKey）只对**真实按键**产生的 KeyPress 生效 ——
    //      用 XSendEvent 造出来的事件带 send_event 标记，会被 grab 直接忽略。
    //    · Wayland：按键是合成器通过门户的 Activated 信号送来的，应用侧
    //      根本没有"往队列里塞一次按键"的入口。
    //    ★ 与其伪造一条"看起来走了同一条路、其实绕过了抓键"的假通路，
    //      不如返回 false：自检据此知道这条用例在当前平台不适用。★
    Q_UNUSED(id)
    return false;
#else
    Q_UNUSED(id)
    return false;
#endif
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message,
                                     NativeEventResult *result)
{
    Q_UNUSED(eventType)
    Q_UNUSED(result)

#ifdef Q_OS_WIN
    //  ★ 两类事件名都要看一眼 ★
    //    "windows_generic_MSG"   —— Qt 事件分发器取到的普通消息
    //    "windows_dispatcher_MSG" —— Qt 自己的内部窗口那一趟
    //    WM_HOTKEY 是投给**线程**的（其 hwnd 为空），Qt 把它归到哪一类
    //    没有明确承诺，也不保证将来不变 —— 所以这里不挑事件名。
    MSG *msg = static_cast<MSG *>(message);
    if (msg && msg->message == WM_HOTKEY) {
        const int id = int(msg->wParam);

        // 只认自己登记过的 id："别的程序"的热键消息不可能投到本线程，
        // 但同一进程里将来可能有第二个使用者 —— 由 id 分清。
        if (m_regs.contains(id)) {
            if (result)
                *result = 0;
            emit activated(id);
            return true;          // 已消费，别再往下发
        }
    }
#endif

    return false;
}

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_X11)

// ============================================================================
//  X11 后端
// ============================================================================
//
//  ★ 为什么自己开一条 X 连接，而不是用 Qt 那条 ★
//    被动抓键（XGrabKey）抓到的 KeyPress 会投递到**抓键所用的那条连接**。
//    借 Qt 的连接来抓，事件就混进了 Qt 自己的 xcb 事件流里：Qt 会尝试
//    把它分给某个窗口，没有窗口要就丢掉 —— 于是"快捷键时灵时不灵"，
//    而且取决于当时焦点在哪个子窗口上。
//    自己开一条，事件只进我们自己的队列，读法完全可控。

// ---------------------------------------------------------------------------
//  X11 登记
// ---------------------------------------------------------------------------
bool GlobalHotkey::registerViaX11(const QKeySequence &sequence, int id)
{
    const int combined = int(sequence[0]);
    const Qt::KeyboardModifiers mods =
        Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask);
    const QString seqText = sequence.toString(QKeySequence::NativeText);

    const quint32 keySym = x11KeySymFor(combined & ~Qt::KeyboardModifierMask);
    if (keySym == 0) {
        m_lastError = tr("The main key of %1 is out of the supported "
                         "range - try another (letters / digits / "
                         "F1~F24 / cursor keys, etc.).").arg(seqText);
        return false;
    }

    const quint32 x11Mods = x11ModifiersFor(mods);
    if (x11Mods == 0) {
        // 与 Windows 侧同一条纪律：不收裸键（理由见那边）
        m_lastError = tr("The global hotkey %1 needs at least one modifier (Ctrl / Alt / Shift).")
                          .arg(seqText);
        return false;
    }

    if (!m_x11Display && !openX11()) {
        m_lastError = tr("Cannot connect to the X server, so global hotkeys are unavailable.");
        return false;
    }

    Display *dpy = static_cast<Display *>(m_x11Display);

    // ★ 必须过一次键码转换 ★（理由见 x11KeySymFor 上方）
    const KeyCode code = XKeysymToKeycode(dpy, KeySym(keySym));
    if (code == 0) {
        m_lastError = tr("The key %1 has no keycode in the current keyboard layout.")
                          .arg(seqText);
        return false;
    }

    // 同一个 id 重复登记 = 覆盖上一次
    unregister(id);

    // ★ 先假设成功，让 X 自己说话 ★
    //   XGrabKey 返回 void —— 它没有"成功/失败"的返回值，被占用是从
    //   错误通道异步报回来的。所以这里只能临时挂上错误处理器再 XSync。
    s_x11GrabBadAccess = false;
    XErrorHandler oldHandler = XSetErrorHandler(x11GrabErrorHandler);
    grabX11Key(keySym, x11Mods);
    XSync(dpy, False);
    XSetErrorHandler(oldHandler);

    if (s_x11GrabBadAccess) {
        // ★ 把自己刚抓的那份松开 ★
        //   不松的话这个键在本进程里一直处于"被我们占着但没生效"的状态：
        //   用户看不到任何变化，而下一个想用它的程序会失败 ——
        //   相当于我们成了一块占着位置不干活的石头。
        ungrabX11Key(keySym, x11Mods);
        XFlush(dpy);
        m_lastError = tr("The hotkey %1 is already taken by another "
                         "program (a combination can only be "
                         "registered by one program system-wide).")
                          .arg(seqText);
        return false;
    }

    m_x11KeyCodes.insert(id, quint32(code));
    m_regs.insert(id, sequence);
    return true;
}

bool GlobalHotkey::openX11()
{
    Display *dpy = XOpenDisplay(nullptr);    if (!dpy)
        return false;   // 没有 X 会话（纯 Wayland / 无图形环境）

    m_x11Display = dpy;

    // ★ 用连接自己的 fd 驱动读取，而不是定时轮询 ★
    //   轮询在空闲时也要反复醒来，一个"只是在等按键"的功能不该有这种开销。
    m_x11Notifier = new QSocketNotifier(ConnectionNumber(dpy),
                                        QSocketNotifier::Read, this);

    // ★ 信号签名在 Qt6 变过 ★
    //   Qt5 是 activated(int)，Qt6 换成了 activated(QSocketDescriptor, Type)。
    //   两个签名不能共用一个 lambda（参数个数不同），只能分版本。
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(m_x11Notifier, &QSocketNotifier::activated, this,
            [this](QSocketDescriptor, QSocketNotifier::Type) { pumpX11Events(); });
#else
    connect(m_x11Notifier, &QSocketNotifier::activated, this,
            [this](int) { pumpX11Events(); });
#endif
    return true;
}

void GlobalHotkey::closeX11()
{
    if (m_x11Notifier) {
        m_x11Notifier->setEnabled(false);
        delete m_x11Notifier;
        m_x11Notifier = nullptr;
    }
    if (m_x11Display) {
        XCloseDisplay(static_cast<Display *>(m_x11Display));
        m_x11Display = nullptr;
    }
    m_x11KeyCodes.clear();
}

void GlobalHotkey::grabX11Key(quint32 keySym, quint32 mods)
{
    Display *dpy = static_cast<Display *>(m_x11Display);
    if (!dpy)
        return;

    const KeyCode code = XKeysymToKeycode(dpy, KeySym(keySym));
    if (code == 0)
        return;

    const Window root = DefaultRootWindow(dpy);

    // ★★ 四种 Lock 组合都要抓一遍 ★★
    //   XGrabKey 的 modifiers 是**精确匹配**：抓 Ctrl+Alt+S 时，
    //   一旦用户开着 NumLock（Mod2），按下那一刻的 state 变成
    //   Ctrl+Alt+Mod2+S —— 与登记的 Ctrl+Alt 不符，事件不会派给我们。
    //   症状是"我这台机器上这个快捷键从来不管用"，而且只在开了小键盘
    //   的机器上复现，最难查的那一类。
    //   CapsLock 同理（LockMask）。所以四个组合一次性全抓。
    const quint32 locks[4] = { 0u, quint32(LockMask), quint32(Mod2Mask),
                               quint32(LockMask | Mod2Mask) };
    for (quint32 lock : locks) {
        XGrabKey(dpy, int(code), mods | lock, root, True,
                 GrabModeAsync, GrabModeAsync);
    }
}

void GlobalHotkey::ungrabX11Key(quint32 keySym, quint32 mods)
{
    Display *dpy = static_cast<Display *>(m_x11Display);
    if (!dpy)
        return;

    const KeyCode code = XKeysymToKeycode(dpy, KeySym(keySym));
    if (code == 0)
        return;

    const Window root = DefaultRootWindow(dpy);
    const quint32 locks[4] = { 0u, quint32(LockMask), quint32(Mod2Mask),
                               quint32(LockMask | Mod2Mask) };
    for (quint32 lock : locks)
        XUngrabKey(dpy, int(code), mods | lock, root);
}

void GlobalHotkey::regrabAllX11()
{
    Display *dpy = static_cast<Display *>(m_x11Display);
    if (!dpy)
        return;

    // 旧键码在布局切换之后已经指向别的键了，先按**旧的**松一遍 ——
    // 只重抓不松手的话，旧键码上会留下一份没人认领的 grab，
    // 那个物理键从此在本进程里一直"被占用但无响应"。
    for (auto it = m_x11KeyCodes.cbegin(); it != m_x11KeyCodes.cend(); ++it) {
        const QKeySequence seq = m_regs.value(it.key());
        if (seq.isEmpty())
            continue;
        const int combined = int(seq[0]);
        const quint32 keySym =
            x11KeySymFor(combined & ~Qt::KeyboardModifierMask);
        const quint32 mods = x11ModifiersFor(
            Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask));
        if (!keySym || !mods)
            continue;

        const quint32 locks[4] = { 0u, quint32(LockMask), quint32(Mod2Mask),
                                   quint32(LockMask | Mod2Mask) };
        for (quint32 lock : locks)
            XUngrabKey(dpy, int(it.value()), mods | lock, DefaultRootWindow(dpy));
    }
    m_x11KeyCodes.clear();

    // 再按**新的**映射重抓
    for (auto it = m_regs.cbegin(); it != m_regs.cend(); ++it) {
        const int combined = int(it.value()[0]);
        const quint32 keySym =
            x11KeySymFor(combined & ~Qt::KeyboardModifierMask);
        const quint32 mods = x11ModifiersFor(
            Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask));
        if (!keySym || !mods)
            continue;

        const KeyCode code = XKeysymToKeycode(dpy, KeySym(keySym));
        if (code == 0)
            continue;   // 新布局下这个符号没有对应键位

        grabX11Key(keySym, mods);
        m_x11KeyCodes.insert(it.key(), quint32(code));
    }

    XFlush(dpy);
}

void GlobalHotkey::pumpX11Events()
{
    Display *dpy = static_cast<Display *>(m_x11Display);
    if (!dpy)
        return;

    // ★ 一次把攒下的事件读完 ★
    //   每次读一条就返回，剩下的要等下一次 fd 可读通知 —— 而 fd 在
    //   "队列非空"时是**一直可读**的，notifier 未必再触发一次，
    //   于是积压的事件会一直躺在那里。
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == KeyPress) {
            const quint32 code  = ev.xkey.keycode;
            // LockMask/Mod2Mask 是抓键时额外补的锁定位，比对时要抹掉，
            // 否则开着 NumLock 时永远匹配不上
            const quint32 state =
                quint32(ev.xkey.state) & ~quint32(LockMask | Mod2Mask);

            for (auto it = m_x11KeyCodes.cbegin();
                 it != m_x11KeyCodes.cend(); ++it) {
                if (it.value() != code)
                    continue;

                const QKeySequence seq = m_regs.value(it.key());
                if (seq.isEmpty())
                    continue;

                const int combined = int(seq[0]);
                const quint32 want = x11ModifiersFor(
                    Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask));
                if (want != state)
                    continue;   // 同一个键位、不同修饰组合 —— 不是它

                emit activated(it.key());
                break;   // 一个键位只对应一个 id
            }
        } else if (ev.type == MappingNotify) {
            // 用户换了键盘布局 / 改了修饰键映射
            XRefreshKeyboardMapping(&ev.xmapping);
            if (ev.xmapping.request == MappingKeyboard
                || ev.xmapping.request == MappingModifier) {
                regrabAllX11();
            }
        }
    }
}

#endif   // Q_OS_LINUX && ZLPE_HAVE_X11

#if defined(Q_OS_LINUX) && defined(ZLPE_HAVE_QTDBUS)

// ============================================================================
//  Wayland 后端：org.freedesktop.portal.GlobalShortcuts
// ============================================================================
//
//  ★ 为什么 Wayland 下不能像 X11 那样直接抓键 ★
//    Wayland 的设计前提就是"客户端看不见、也碰不到别的客户端的输入"。
//    键盘事件由合成器统一分发，应用连"全系统按了哪个键"都无从得知。
//    唯一被允许的路径是求合成器替我们登记：这就是 GlobalShortcuts 门户 ——
//    我们提交一组"想要的快捷键"，合成器弹一次授权框，用户点了同意之后，
//    按键时合成器主动给我们发 Activated 信号。
//
//  ★ 三个必须接受的代价（不是实现没做好，是这条路本身的形状）★
//    ① **一定会弹一次系统授权框**，用户不点就一直等；
//    ② **需要 app_id**：门户要能认出"是哪个应用在要权限"，
//       靠的是已安装的 .desktop 文件。从 build 目录直接跑的程序拿不到
//       app_id，会被拒绝 —— 这一条决定了开发环境里几乎没法验证；
//    ③ **桌面支持面窄得多**：GNOME 46+ / KDE Plasma 5.27+ 有，
//       wlroots 系（Sway、Hyprland 等）的 portal 后端至今没实现。
//       不支持时只能明确告诉用户"这里做不到"，不能假装成功。

namespace {

const QString kPortalService   = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPortalPath      = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kShortcutsIface  = QStringLiteral("org.freedesktop.portal.GlobalShortcuts");
const QString kSessionIface    = QStringLiteral("org.freedesktop.portal.Session");
const QString kRequestIface    = QStringLiteral("org.freedesktop.portal.Request");

constexpr int kPortalCallTimeoutMs = 4000;

// BindShortcuts 会弹系统授权框 —— 用户可能要看一眼说明、可能去干别的了。
// 给 90 秒：比这更久的等待，与其继续冻着界面，不如报失败让用户重来。
constexpr int kPortalBindTimeoutMs = 90000;

QString nextPortalToken()
{
    static QAtomicInt seq{0};
    return QStringLiteral("zlpe_hotkey_%1").arg(seq.fetchAndAddRelaxed(1));
}

} // namespace

// ★ 结构体与它的两个序列化算符必须留在 `namespace zl` 里（不能在匿名
//   命名空间内）★
//   QDBusArgument 找 `operator<<` 靠的是**参数相关查找（ADL）** ——
//   算符必须和类型待在同一个命名空间。放进匿名命名空间，类型有了内部
//   链接，ADL 找不到算符，表现是运行期一句 "Marshalling failed"。
struct PortalShortcut
{
    QString     id;
    QVariantMap options;
};

QDBusArgument &operator<<(QDBusArgument &arg, const PortalShortcut &s)
{
    arg.beginStructure();
    arg << s.id << s.options;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, PortalShortcut &s)
{
    arg.beginStructure();
    arg >> s.id >> s.options;
    arg.endStructure();
    return arg;
}

} // namespace zl

// ★ 必须写在 namespace 之外、且紧跟类型定义 ★
//   Q_DECLARE_METATYPE 要在全局作用域里声明"这个类型能装进 QVariant"。
Q_DECLARE_METATYPE(zl::PortalShortcut)

namespace zl {
namespace {

// ★ 自定义 D-Bus 结构必须显式注册 ★
//   不注册的话，`a(sa{sv})` 这个签名在发送时会被当成一个未知类型，
//   报的是一句 "Marshalling failed" 之类的运行期错误 —— 编译期毫无征兆。
void ensurePortalTypesRegistered()
{
    static bool done = false;
    if (done)
        return;
    qDBusRegisterMetaType<PortalShortcut>();
    qDBusRegisterMetaType<QList<PortalShortcut>>();
    done = true;
}

// 会话类型判断 + 门户是否实现了 GlobalShortcuts
bool portalGlobalShortcutsAvailable()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    // 与截图那边同样的理由：查 version 属性而不是"服务名在不在"。
    // 转发层活着但后端没实现这个接口，是非常常见的情形
    // （装了 xdg-desktop-portal 却没装对应桌面的后端）。
    QDBusMessage msg = QDBusMessage::createMethodCall(
        kPortalService, kPortalPath,
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("Get"));
    msg << kShortcutsIface << QStringLiteral("version");

    const QDBusMessage reply = bus.call(msg, QDBus::Block, kPortalCallTimeoutMs);
    return reply.type() == QDBusMessage::ReplyMessage;
}

// Qt 的键序列 → 门户要的 trigger 字符串
//
// 门户用的是 XDG shortcuts 规范里那套写法：修饰键全大写、用 '+' 连接，
// 最后一段是 **XKB keysym 名**（字母用小写）。例如 "CTRL+ALT+s"。
//
// ★ 大小写不是风格问题 ★
//   写成 "CTRL+ALT+S" 会被解析成"Shift+s"的 keysym，用户按下去不生效，
//   而且门户不会报错 —— 它只是绑到了一个到不了的组合上。
QString portalTriggerFor(const QKeySequence &sequence)
{
    if (sequence.isEmpty())
        return QString();

    const int combined = int(sequence[0]);
    const Qt::KeyboardModifiers mods =
        Qt::KeyboardModifiers(combined & Qt::KeyboardModifierMask);
    const int key = combined & ~Qt::KeyboardModifierMask;

    QStringList parts;
    if (mods & Qt::ControlModifier) parts << QStringLiteral("CTRL");
    if (mods & Qt::AltModifier)     parts << QStringLiteral("ALT");
    if (mods & Qt::ShiftModifier)   parts << QStringLiteral("SHIFT");
    if (mods & Qt::MetaModifier)    parts << QStringLiteral("LOGO");

    QString name;
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        name = QString(QChar(QLatin1Char('a' + (key - Qt::Key_A))));
    } else if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        name = QString(QChar(QLatin1Char('0' + (key - Qt::Key_0))));
    } else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) {
        name = QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
    } else {
        switch (key) {
        case Qt::Key_Print:     name = QStringLiteral("Print");     break;
        case Qt::Key_Insert:    name = QStringLiteral("Insert");    break;
        case Qt::Key_Delete:    name = QStringLiteral("Delete");    break;
        case Qt::Key_Home:      name = QStringLiteral("Home");      break;
        case Qt::Key_End:       name = QStringLiteral("End");       break;
        case Qt::Key_PageUp:    name = QStringLiteral("Prior");     break;
        case Qt::Key_PageDown:  name = QStringLiteral("Next");      break;
        case Qt::Key_Left:      name = QStringLiteral("Left");      break;
        case Qt::Key_Up:        name = QStringLiteral("Up");        break;
        case Qt::Key_Right:     name = QStringLiteral("Right");     break;
        case Qt::Key_Down:      name = QStringLiteral("Down");      break;
        case Qt::Key_Return:
        case Qt::Key_Enter:     name = QStringLiteral("Return");    break;
        case Qt::Key_Escape:    name = QStringLiteral("Escape");    break;
        case Qt::Key_Space:     name = QStringLiteral("space");     break;
        case Qt::Key_Tab:       name = QStringLiteral("Tab");       break;
        case Qt::Key_Backspace: name = QStringLiteral("BackSpace"); break;
        case Qt::Key_Pause:     name = QStringLiteral("Pause");     break;
        default:                return QString();
        }
    }

    parts << name;
    return parts.join(QLatin1Char('+'));
}

// 发一个"返回 request handle、结果另发 Response 信号"的门户调用，并等它回来。
//
// 约定：args 的最后一项必须是 a{sv} 的 options —— 函数会把 handle_token
// 注进去（门户靠它决定 request 对象路径）。两个门户方法都符合这个形状，
// 于是只需要一份等待逻辑。
bool portalRequestWithOptions(const QString &method, QVariantList args,
                              QVariantMap *outResults, int timeoutMs)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    const QString token = nextPortalToken();
    QVariantMap options = args.last().toMap();
    options.insert(QStringLiteral("handle_token"), token);
    args.last() = options;

    const QString expectPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
            .arg(portalSenderToken(bus.baseService()), token);

    PortalResponseWaiter waiter;
    QEventLoop loop;
    waiter.attach(&loop);

    // ★ 先订阅，后调用 ★（理由与截图那条完全一样：结果可能比返回先到）
    auto subscribe = [&](const QString &path) {
        return bus.connect(kPortalService, path, kRequestIface,
                           QStringLiteral("Response"), &waiter,
                           SLOT(onResponse(QDBusMessage)));
    };
    subscribe(expectPath);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        kPortalService, kPortalPath, kShortcutsIface, method);
    for (const QVariant &arg : args)
        msg << arg;

    const QDBusMessage reply = bus.call(msg, QDBus::Block, kPortalCallTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage)
        return false;

    const QString handle =
        reply.arguments().value(0).value<QDBusObjectPath>().path();
    if (!handle.isEmpty() && handle != expectPath)
        subscribe(handle);

    if (!waiter.answered) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }

    if (!waiter.answered || waiter.code != 0)
        return false;

    if (outResults)
        *outResults = waiter.results;
    return true;
}

} // namespace

bool GlobalHotkey::ensurePortalSession(QString *error)
{
    if (m_portalSessionActive)
        return true;

    ensurePortalTypesRegistered();

    // ★ 订阅放在建 session 之前 ★
    //   session 一建好，合成器随时可能因为用户按了键而发 Activated。
    //   "先建完再连"的那一小段里发生的按键会静默丢失。
    if (!m_portalSubscribed) {
        QDBusConnection bus = QDBusConnection::sessionBus();
        m_portalSubscribed = bus.connect(
            kPortalService, kPortalPath, kShortcutsIface,
            QStringLiteral("Activated"), this,
            SLOT(onPortalActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
        if (!m_portalSubscribed) {
            *error = tr("Cannot subscribe to the desktop portal's shortcut events.");
            return false;
        }
    }

    QVariantMap options;
    options.insert(QStringLiteral("session_handle_token"), nextPortalToken());

    QVariantMap results;
    if (!portalRequestWithOptions(QStringLiteral("CreateSession"),
                                  QVariantList{options}, &results,
                                  kPortalCallTimeoutMs)) {
        *error = tr("The desktop portal refused to open a shortcut session.");
        return false;
    }

    // session_handle 在 results 里的类型随实现而异（规范写 s，个别后端给 o）
    const QVariant raw = results.value(QStringLiteral("session_handle"));
    const QString handle = raw.canConvert<QDBusObjectPath>()
                               ? raw.value<QDBusObjectPath>().path()
                               : raw.toString();
    if (handle.isEmpty()) {
        *error = tr("The desktop portal returned no shortcut session.");
        return false;
    }

    m_portalSession = handle;
    m_portalSessionActive = true;
    return true;
}

void GlobalHotkey::closePortalSession()
{
    if (m_portalSessionActive && !m_portalSession.isEmpty()) {
        // 不关的话，合成器那边会一直留着这条 session 的绑定：
        // 用户按那个组合键，系统仍然认为"这个应用要它"，键被吃掉，
        // 而应用侧已经不再监听 —— 表现为"这个快捷键被一个看不见的程序占用"。
        QDBusMessage msg = QDBusMessage::createMethodCall(
            kPortalService, m_portalSession, kSessionIface,
            QStringLiteral("Close"));
        QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
    }

    m_portalSession.clear();
    m_portalSessionActive = false;
    m_portalShortcuts.clear();

    // Activated 的订阅保留不动：QDBusConnection 的 disconnect 要求
    // 参数逐个对得上，而这里再连一次比断一次更安全 —— 槽里会按
    // m_portalSessionActive 与 session 路径过滤，多余的信号进不来。
}

bool GlobalHotkey::registerViaPortal(const QKeySequence &sequence, int id)
{
    const QString seqText = sequence.toString(QKeySequence::NativeText);

    if (!portalGlobalShortcutsAvailable()) {
        // ★ 这是"这个桌面不支持"，不是"这次没成功" ★
        //   文案必须把这条区别说出来，否则用户会以为是撞了键、去换一个
        //   再试 —— 换多少个都一样。
        m_lastError = tr("This Wayland desktop does not provide the "
                         "GlobalShortcuts portal, so global hotkeys are "
                         "unavailable. While the program has focus, %1 still works.")
                          .arg(seqText);
        return false;
    }

    const QString trigger = portalTriggerFor(sequence);
    if (trigger.isEmpty()) {
        m_lastError = tr("The main key of %1 is out of the supported "
                         "range - try another (letters / digits / "
                         "F1~F24 / cursor keys, etc.).").arg(seqText);
        return false;
    }

    QString error;
    if (!ensurePortalSession(&error)) {
        m_lastError = error;
        return false;
    }

    const QString shortcutId = QStringLiteral("zlpe-capture-%1").arg(id);

    PortalShortcut shortcut;
    shortcut.id = shortcutId;
    shortcut.options.insert(QStringLiteral("description"),
                           tr("Capture the screen into a new tab"));
    shortcut.options.insert(QStringLiteral("preferred_trigger"), trigger);

    QList<PortalShortcut> shortcuts;
    shortcuts.append(shortcut);

    QVariantMap results;
    const bool ok = portalRequestWithOptions(
        QStringLiteral("BindShortcuts"),
        QVariantList{ QVariant::fromValue(QDBusObjectPath(m_portalSession)),
                      QVariant::fromValue(shortcuts),
                      QString(),        // parent_window：不依附具体窗口
                      QVariantMap() },  // options（handle_token 由上面注入）
        &results, kPortalBindTimeoutMs);

    if (!ok) {
        // ★ 这里**不能**说成"被别的程序占了" ★
        //   门户这条路根本不存在"被别人占了"这个概念：合成器不会告诉我们
        //   冲突，它只会拒绝授权。把拒绝说成占用，用户会去关别的软件 ——
        //   永远关不完。
        m_lastError = tr("The desktop did not grant the global hotkey %1 "
                         "(the shortcut request was declined, timed out, or "
                         "this application is not installed as a desktop "
                         "application).").arg(seqText);
        closePortalSession();
        return false;
    }

    m_portalShortcuts.insert(shortcutId, id);
    m_regs.insert(id, sequence);
    return true;
}

void GlobalHotkey::onPortalActivated(const QDBusObjectPath &session,
                                     const QString &shortcutId,
                                     qulonglong timestamp,
                                     const QVariantMap &options)
{
    Q_UNUSED(timestamp)
    Q_UNUSED(options)

    // ★ 必须按 session 过滤 ★
    //   同一条 D-Bus 连接上可能有多个应用会话（本进程重复建、或将来
    //   有第二个使用者）。不看 session 就发信号，等于把别人的按键
    //   当成了自己的。
    if (!m_portalSessionActive || session.path() != m_portalSession)
        return;

    const int id = m_portalShortcuts.value(shortcutId, -1);
    if (id < 0)
        return;

    emit activated(id);
}

#endif   // Q_OS_LINUX && ZLPE_HAVE_QTDBUS

} // namespace zl
