#include "core/UiTheme.h"

#include "compat/QtCompat.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QtGlobal>

namespace zl {
namespace uitheme {

namespace {

// QSettings 键名。发布后不要改（改了等于重置用户的选择）。
//   "ui/" 分组与 "ui/language" 平级，都装"应用级偏好"。
constexpr const char *kSettingsKey = "ui/theme";

// 取值的字符串形式。"auto" 是默认态，也是"没存过"的等价物 ——
// 菜单的勾选判断因此不用区分这两者。
constexpr const char *kAuto  = "auto";
constexpr const char *kLight = "light";
constexpr const char *kDark  = "dark";

// ---------------------------------------------------------------------------
//  系统原始状态（captureSystem 记下，"默认"靠它还原）
// ---------------------------------------------------------------------------
//  ★ 只记样式名，不记 palette ★
//    palette 有更好的还原手段：传空 palette 让 Qt 重新向平台要（见 applyTheme
//    的 auto 分支）。快照是"启动那一刻"的，用户之后改了系统配色就过期；
//    而样式名没有查询接口，只能靠记。
//
//  ★ 必须记样式名，不是记 QStyle 指针 ★
//    QApplication::setStyle() 如果传的对象与新样式同类，Qt 会直接复用；
//    而传进来的指针所有权归 QApplication，存着不用管释放。
//    记名字则与"谁持有对象"彻底无关，也不会因为有指针就以为能改它。
QString g_systemStyleName;
bool    g_captured = false;

QString keyOf(const QColor &c)
{
    return c.name(QColor::HexRgb);
}

// ---------------------------------------------------------------------------
//  两套显式配色
// ---------------------------------------------------------------------------
//  ★ 只写"会真正影响观感"的那十几个角色 ★
//    不写的角色由 Fusion 从写了的角色推导（它本身是 palette 驱动的样式），
//    硬凑几十个反而容易写歪一处而没人发现。
//
//  ★ 深浅两套必须成对出现 ★
//    只加一处而漏了另一处，症状是"切到某个主题后某个角落看不清"——
//    所以两个函数紧挨着放，改一个就能看到另一个。

QPalette lightPalette()
{
    QPalette p;
    const QColor window(0xF2, 0xF2, 0xF2);
    const QColor base(0xFF, 0xFF, 0xFF);
    const QColor text(0x20, 0x20, 0x20);
    const QColor button(0xEC, 0xEC, 0xEC);
    // ★ Mid 取 #A6 而不是"更浅的边框灰" ★
    //   本工程大量用 palette(mid) 当**次要文字色**（提示语、计数标签），
    //   而 Qt 的 Mid 本职是边框色。取太浅会让这些提示在白底上看不见。
    const QColor mid(0xA6, 0xA6, 0xA6);
    const QColor disabled(0x8C, 0x8C, 0x8C);
    const QColor accent(0x2F, 0x6F, 0xB8);

    p.setColor(QPalette::Window,          window);
    p.setColor(QPalette::WindowText,      text);
    p.setColor(QPalette::Base,            base);
    p.setColor(QPalette::AlternateBase,   QColor(0xF7, 0xF7, 0xF7));
    p.setColor(QPalette::Text,            text);
    p.setColor(QPalette::Button,          button);
    p.setColor(QPalette::ButtonText,      text);
    p.setColor(QPalette::BrightText,      QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::ToolTipBase,     QColor(0xFF, 0xFF, 0xDC));
    p.setColor(QPalette::ToolTipText,     QColor(0x20, 0x20, 0x20));
    p.setColor(QPalette::Highlight,       accent);
    p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Link,            accent);
    p.setColor(QPalette::Mid,             mid);

    // 禁用态：三组都要给，漏一组就有控件在禁用时仍是黑字
    p.setColor(QPalette::Disabled, QPalette::WindowText,      disabled);
    p.setColor(QPalette::Disabled, QPalette::Text,            disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText,      disabled);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QPalette darkPalette()
{
    QPalette p;
    const QColor window(0x35, 0x35, 0x38);
    const QColor base(0x25, 0x25, 0x28);
    const QColor text(0xE6, 0xE6, 0xE6);
    const QColor button(0x3E, 0x3E, 0x42);
    // 与浅色同理（那里 Mid 兼作次要文字色）。Fusion 的默认 Mid 是 #b8b8b8，
    // 深色底上要**提亮**到接近它，取太暗会让提示语与标尺次刻度糊进背景。
    const QColor mid(0x8E, 0x8E, 0x92);
    const QColor disabled(0x8A, 0x8A, 0x8E);
    const QColor accent(0x3E, 0x7F, 0xC8);

    p.setColor(QPalette::Window,          window);
    p.setColor(QPalette::WindowText,      text);
    p.setColor(QPalette::Base,            base);
    p.setColor(QPalette::AlternateBase,   QColor(0x2C, 0x2C, 0x30));
    p.setColor(QPalette::Text,            text);
    p.setColor(QPalette::Button,          button);
    p.setColor(QPalette::ButtonText,      text);
    p.setColor(QPalette::BrightText,      QColor(0xFF, 0xFF, 0xFF));
    // 提示框也走深色：留浅黄会在深色界面上刺出一块亮斑
    p.setColor(QPalette::ToolTipBase,     QColor(0x2A, 0x2A, 0x2D));
    p.setColor(QPalette::ToolTipText,     text);
    p.setColor(QPalette::Highlight,       accent);
    p.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
    p.setColor(QPalette::Link,            QColor(0x6E, 0xAF, 0xEF));
    p.setColor(QPalette::Mid,             mid);

    p.setColor(QPalette::Disabled, QPalette::WindowText,      disabled);
    p.setColor(QPalette::Disabled, QPalette::Text,            disabled);
    p.setColor(QPalette::Disabled, QPalette::ButtonText,      disabled);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

// 相同的一条 palette 同时给 Active / Inactive / Disabled 三组：
// 只设 Active 的话，窗口一失焦（Inactive）就会回落到默认的浅色，
// 表现是"点一下别的窗口，界面唰地变白"。
void applyPaletteRoles(QPalette &target, const QPalette &source)
{
    for (int role = 0; role < QPalette::NColorRoles; ++role) {
        const auto r = QPalette::ColorRole(role);
        target.setColor(QPalette::Active,   r, source.color(QPalette::Active, r));
        target.setColor(QPalette::Inactive, r, source.color(QPalette::Inactive, r));
        target.setColor(QPalette::Disabled, r, source.color(QPalette::Disabled, r));
    }
}

/// 把 Fusion 换成当前样式。只在样式真的不同才调 ——
/// 每次 applyTheme 都 setStyle 会让所有控件重建一次 style，
/// 切主题本来就要重绘一遍，没必要再叠一层无谓的抖动。
void useStyle(const QString &name)
{
    if (!qApp || name.isEmpty())
        return;
    if (qApp->style() && qApp->style()->objectName().compare(
                             name, Qt::CaseInsensitive) == 0)
        return;
    if (QStyle *s = QStyleFactory::create(name))
        qApp->setStyle(s);
}

} // namespace

// ---------------------------------------------------------------------------
//  取值表
// ---------------------------------------------------------------------------

const QList<Mode> &allModes()
{
    static const QList<Mode> modes{Mode::Auto, Mode::Light, Mode::Dark};
    return modes;
}

QString keyOf(Mode mode)
{
    switch (mode) {
    case Mode::Light: return QLatin1String(kLight);
    case Mode::Dark:  return QLatin1String(kDark);
    case Mode::Auto:  break;
    }
    return QLatin1String(kAuto);
}

QString displayName(Mode mode)
{
    switch (mode) {
    case Mode::Light: return QCoreApplication::translate("zl", "Light");
    case Mode::Dark:  return QCoreApplication::translate("zl", "Dark");
    case Mode::Auto:  break;
    }
    // 与语言菜单的"自动（跟随系统）"同一个说法：说清楚"跟谁走"，
    // 否则用户会以为是"程序自己挑一个"。
    return QCoreApplication::translate("zl", "Default (Follow System)");
}

// ---------------------------------------------------------------------------
//  偏好
// ---------------------------------------------------------------------------

Mode preference()
{
    QSettings settings;
    const QString saved = settings.value(QLatin1String(kSettingsKey)).toString();

    if (saved == QLatin1String(kLight))
        return Mode::Light;
    if (saved == QLatin1String(kDark))
        return Mode::Dark;
    // 没存过 / 存了垃圾 / 存的正是 "auto" —— 三种都按自动，
    // 与 languagePreference() 同一条口径，不让坏设置文件变成花屏。
    return Mode::Auto;
}

void setPreference(Mode mode)
{
    QSettings settings;
    settings.setValue(QLatin1String(kSettingsKey), keyOf(mode));
    // 立刻落盘：用户可能点完主题就直接 Alt+F4，靠析构写盘会丢。
    settings.sync();
}

// ---------------------------------------------------------------------------
//  应用
// ---------------------------------------------------------------------------

void captureSystem()
{
    if (g_captured || !qApp)
        return;
    g_captured = true;
    if (qApp->style())
        g_systemStyleName = qApp->style()->objectName();
}

bool isDark()
{
    return zl::isDarkTheme();
}

void applyTheme(Mode mode)
{
    if (!qApp)
        return;
    // 调用方可能跳过 captureSystem()（自检里就有），补一次。
    // 补的时候记下的就是"当前状态"，仍比没有好。
    captureSystem();

    if (mode == Mode::Auto) {
        // ★ "默认" = 撤销全部改动 ★
        //   顺序与下面相反：先把配色交回系统，再换回原样式 ——
        //   反过来的话，换样式的瞬间会拿深色 palette 去配系统样式，
        //   闪一下（样式表驱动不了 Fusion 之外的样式，观感上很明显）。
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        QGuiApplication::styleHints()->unsetColorScheme();
#endif
        // ★ 传空 palette，而不是塞回 captureSystem() 存的那份 ★
        //   快照是**启动那一刻**的。用户之后在系统里改了配色，快照就过期了，
        //   塞回去等于"退回到开机时的样子"，而不是"跟随现在的系统" ——
        //   实测：系统转深色后点默认，界面退回浅色，跟系统正好相反。
        //   传空 palette 是把覆盖整个扔掉，Qt 会重新向平台要当前配色，
        //   这才是"跟随系统"的字面意思。
        qApp->setPalette(QPalette());
        if (!g_systemStyleName.isEmpty())
            useStyle(g_systemStyleName);
        return;
    }

    const bool dark = (mode == Mode::Dark);

    // ★ 顺序：样式 → colorScheme → palette ★
    //   · 样式最先：Fusion 起来之后下面设的 palette 才会被它真正采纳；
    //   · colorScheme 其次：这是给**样式内部**看的提示（滚动条、菜单阴影
    //     那类不走 palette 的部件靠它），Qt5 没有这一层；
    //   · palette 最后：它是实际生效的东西，也是触发 PaletteChange
    //     （→ 图标重上色）的那一下。放最后能保证重绘时新配色已经就位。
    useStyle(QStringLiteral("Fusion"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QGuiApplication::styleHints()->setColorScheme(
        dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif

    QPalette p = qApp->palette();          // 以现有为底，别丢掉没写的角色
    applyPaletteRoles(p, dark ? darkPalette() : lightPalette());
    qApp->setPalette(p);
}

} // namespace uitheme
} // namespace zl
