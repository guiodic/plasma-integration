/*  This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "kwaylandintegration.h"

#include <QExposeEvent>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QVariant>
#include <QDebug>
#include <KConfigGroup>
#include <KSharedConfig>
#include <QGuiApplication>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <qtwaylandclientversion.h>

#include "qdbusmenubar_p.h"
#include "qwayland-appmenu.h"
#include "qwayland-server-decoration-palette.h"
#include "qwayland-xdg-session-management-v1.h"
#include "qwayland-xdg-shell.h"

#include <KWindowEffects>

static const QByteArray s_schemePropertyName = QByteArrayLiteral("KDE_COLOR_SCHEME_PATH");
static const QByteArray s_blurBehindPropertyName = QByteArrayLiteral("ENABLE_BLUR_BEHIND_HINT");
static const QString s_sessionConfigGroup = QStringLiteral("XdgSessionManagement");
static const char s_windowRestoreIdProperty[] = "KDE_SESSION_RESTORE_ID";
static const char s_toplevelSessionAttachedProperty[] = "org.kde.plasma.integration.xdgSessionAttached";

class AppMenuManager : public QWaylandClientExtensionTemplate<AppMenuManager>, public QtWayland::org_kde_kwin_appmenu_manager
{
    Q_OBJECT
public:
    AppMenuManager()
        : QWaylandClientExtensionTemplate<AppMenuManager>(2)
    {
        // QWaylandClientExtensionTemplate invokes this with a QueuedConnection
        QMetaObject::invokeMethod(this, "addRegistryListener");
    }
    ~AppMenuManager()
    {
        if (isActive() && QWaylandClientExtension::version() >= ORG_KDE_KWIN_APPMENU_MANAGER_RELEASE_SINCE_VERSION) {
            release();
        }
    }
};

class AppMenu : public QtWayland::org_kde_kwin_appmenu
{
public:
    using org_kde_kwin_appmenu::org_kde_kwin_appmenu;
    ~AppMenu()
    {
        release();
    }
};


class XdgSessionManagerV1 : public QWaylandClientExtensionTemplate<XdgSessionManagerV1>,
                            public QtWayland::xdg_session_manager_v1
{
public:
    XdgSessionManagerV1()
        : QWaylandClientExtensionTemplate<XdgSessionManagerV1>(1)
    {
        QMetaObject::invokeMethod(this, "addRegistryListener");
    }
    ~XdgSessionManagerV1() override
    {
        if (isActive()) {
            destroy();
        }
    }
};

class XdgSessionV1 : public QtWayland::xdg_session_v1
{
public:
    void setIntegration(KWaylandIntegration *integration) { m_integration = integration; }
    ~XdgSessionV1() override { destroy(); }
protected:
    void xdg_session_v1_created(const QString &id) override
    {
        if (m_integration) m_integration->saveSessionId(id);
    }
    void xdg_session_v1_restored() override {}
    void xdg_session_v1_replaced() override { destroy(); }
private:
    KWaylandIntegration *m_integration = nullptr;
};

class XdgToplevelSessionV1 : public QtWayland::xdg_toplevel_session_v1
{
public:
    using QtWayland::xdg_toplevel_session_v1::xdg_toplevel_session_v1;
    ~XdgToplevelSessionV1() override { destroy(); }
};

class ServerSideDecorationPaletteManager : public QWaylandClientExtensionTemplate<ServerSideDecorationPaletteManager>,
                                           public QtWayland::org_kde_kwin_server_decoration_palette_manager
{
    Q_OBJECT
public:
    ServerSideDecorationPaletteManager()
        : QWaylandClientExtensionTemplate<ServerSideDecorationPaletteManager>(1)
    {
        QMetaObject::invokeMethod(this, "addRegistryListener");
    }
    ~ServerSideDecorationPaletteManager() override
    {
        if (isActive()) {
            org_kde_kwin_server_decoration_palette_manager_destroy(object());
        }
    }
};

class ServerSideDecorationPalette : public QtWayland::org_kde_kwin_server_decoration_palette
{
public:
    using org_kde_kwin_server_decoration_palette::org_kde_kwin_server_decoration_palette;
    ~ServerSideDecorationPalette() override
    {
        release();
    }
};

Q_DECLARE_METATYPE(AppMenu *);
Q_DECLARE_METATYPE(ServerSideDecorationPalette *);

KWaylandIntegration::KWaylandIntegration(KdePlatformTheme *platformTheme)
    : QObject()
    , m_platformTheme(platformTheme)
{
    QCoreApplication::instance()->installEventFilter(this);
    initializeSessionManagement();
}

KWaylandIntegration::~KWaylandIntegration()
{
    for (auto *session : m_toplevelSessions) {
        delete session;
    }
    m_toplevelSessions.clear();
}

bool KWaylandIntegration::isRelevantTopLevel(QWindow *w)
{
    if (!w || w->parent()) {
        return false;
    }

    // ignore  windows that map to XdgPopup
    if (w->type() == Qt::ToolTip || w->type() == Qt::Popup) {
        return false;
    }
    return true;
}

bool KWaylandIntegration::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Expose) {
        auto ee = static_cast<QExposeEvent *>(event);
        if (ee->region().isNull()) {
            return false;
        }
        QWindow *w = qobject_cast<QWindow *>(watched);
        if (!isRelevantTopLevel(w)) {
            return false;
        }
        if (!w->isVisible()) {
            return false;
        }

        if (w->property("org.kde.plasma.integration.shellSurfaceCreated").isNull()) {
            shellSurfaceCreated(w);
        }
    } else if (event->type() == QEvent::Hide) {
        QWindow *w = qobject_cast<QWindow *>(watched);
        if (!isRelevantTopLevel(w)) {
            return false;
        }
        shellSurfaceDestroyed(w);
    } else if (event->type() == QEvent::ApplicationPaletteChange) {
        if (watched != QGuiApplication::instance()) {
            return false;
        }
        const auto topLevelWindows = QGuiApplication::topLevelWindows();
        for (QWindow *w : topLevelWindows) {
            if (isRelevantTopLevel(w)) {
                installColorScheme(w);
            }
        }
    } else if (event->type() == QEvent::PlatformSurface) {
        QWindow *w = qobject_cast<QWindow *>(watched);
        QPlatformSurfaceEvent *pe = static_cast<QPlatformSurfaceEvent *>(event);
        if (w && !w->flags().testFlag(Qt::ForeignWindow) && pe->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
            m_platformTheme->windowCreated(w);
            registerToplevelSession(w);
        }
    }

    return false;
}

void KWaylandIntegration::shellSurfaceCreated(QWindow *w)
{
    // set colorscheme hint
    if (qApp->property(s_schemePropertyName.constData()).isValid()) {
        installColorScheme(w);
    }
    const auto blurBehindProperty = w->property(s_blurBehindPropertyName.constData());
    if (blurBehindProperty.isValid()) {
        KWindowEffects::enableBlurBehind(w, blurBehindProperty.toBool());
    }
    // create deco
    wl_surface *s = surfaceFromWindow(w);
    if (!s) {
        return;
    }

    registerToplevelSession(w);
    w->setProperty("org.kde.plasma.integration.shellSurfaceCreated", true);

#ifndef KF6_TODO_DBUS_MENUBAR
    if (!m_appMenuManager) {
        m_appMenuManager.reset(new AppMenuManager());
    }
    if (m_appMenuManager->isActive()) {
        auto menu = new AppMenu(m_appMenuManager->create(s));
        w->setProperty("org.kde.plasma.integration.appmenu", QVariant::fromValue(menu));
        auto menuBar = QDBusMenuBar::menuBarForWindow(w);
        if (!menuBar) {
            menuBar = QDBusMenuBar::globalMenuBar();
        }
        if (menuBar) {
            menu->set_address(QDBusConnection::sessionBus().baseService(), menuBar->objectPath());
        }
    }
#endif
}

void KWaylandIntegration::shellSurfaceDestroyed(QWindow *w)
{
    delete m_toplevelSessions.take(w);
    m_toplevelSessionNames.remove(w);
    w->setProperty(s_toplevelSessionAttachedProperty, false);

    w->setProperty("org.kde.plasma.integration.shellSurfaceCreated", QVariant());

    auto appMenu = w->property("org.kde.plasma.integration.appmenu").value<AppMenu *>();
    if (appMenu) {
        delete appMenu;
    }
    w->setProperty("org.kde.plasma.integration.appmenu", QVariant());

    auto decoPallete = w->property("org.kde.plasma.integration.palette").value<ServerSideDecorationPalette *>();
    if (decoPallete) {
        delete decoPallete;
    }
    w->setProperty("org.kde.plasma.integration.palette", QVariant());
}


void KWaylandIntegration::initializeSessionManagement()
{
    m_sessionManager.reset(new XdgSessionManagerV1);
    connect(m_sessionManager.data(), &QWaylandClientExtensionTemplate<XdgSessionManagerV1>::activeChanged,
            this, &KWaylandIntegration::startSessionManagement);
    if (m_sessionManager->isActive()) {
        startSessionManagement();
    }
}

void KWaylandIntegration::startSessionManagement()
{
    if (!m_sessionManager || !m_sessionManager->isActive() || m_session) {
        return;
    }

    const QString appId = QGuiApplication::desktopFileName().isEmpty()
        ? QCoreApplication::applicationName()
        : QGuiApplication::desktopFileName();
    if (appId.isEmpty()) {
        return;
    }

    const auto config = KSharedConfig::openConfig();
    KConfigGroup group(config, s_sessionConfigGroup);
    m_sessionId = group.readEntry(appId, QString());
    qDebug() << "xdg-session: start" << "appId=" << appId << "storedSessionId=" << m_sessionId;

    m_session.reset(new XdgSessionV1);
    m_session->setIntegration(this);
    if (m_sessionId.isEmpty()) {
        m_session->init(m_sessionManager->get_session(XdgSessionManagerV1::reason_launch, QString()));
    } else {
        m_session->init(m_sessionManager->get_session(XdgSessionManagerV1::reason_session_restore, m_sessionId));
    }
}

void KWaylandIntegration::saveSessionId(const QString &id)
{
    if (id.isEmpty()) {
        return;
    }

    const QString appId = QGuiApplication::desktopFileName().isEmpty()
        ? QCoreApplication::applicationName()
        : QGuiApplication::desktopFileName();
    if (appId.isEmpty()) {
        return;
    }

    m_sessionId = id;
    const auto config = KSharedConfig::openConfig();
    KConfigGroup group(config, s_sessionConfigGroup);
    group.writeEntry(appId, id);
    group.sync();
    qDebug() << "xdg-session: saved session ID" << appId << id;
}

QString KWaylandIntegration::toplevelSessionName(QWindow *w) const
{
    const QVariant explicitId = w->property(s_windowRestoreIdProperty);
    QString name;
    if (explicitId.isValid() && !explicitId.toString().isEmpty()) {
        name = explicitId.toString();
    } else {
        QString identity = w->objectName();
        if (identity.isEmpty()) {
            identity = w->title();
        }
        if (identity.isEmpty()) {
            identity = QStringLiteral("%1:%2").arg(w->metaObject()->className()).arg(int(w->type()));
        }

        const QString appId = QGuiApplication::desktopFileName().isEmpty()
            ? QCoreApplication::applicationName()
            : QGuiApplication::desktopFileName();
        name = QString::fromLatin1(QCryptographicHash::hash((appId + QLatin1Char('/') + identity).toUtf8(),
                                                             QCryptographicHash::Sha256).toHex());
    }

    const QString baseName = name;
    int suffix = 2;
    auto nameInUse = [this, w](const QString &candidate) {
        for (auto it = m_toplevelSessionNames.cbegin(); it != m_toplevelSessionNames.cend(); ++it) {
            if (it.key() != w && it.value() == candidate) {
                return true;
            }
        }
        return false;
    };
    while (nameInUse(name)) {
        name = baseName + QLatin1Char('-') + QString::number(suffix++);
    }
    if (name != baseName) {
        qWarning() << "xdg-session: duplicate toplevel identity; using unique ID" << baseName << name;
    }
    return name;
}

void KWaylandIntegration::registerToplevelSession(QWindow *w)
{
    if (!m_sessionManager || !m_sessionManager->isActive() || !m_session || !isRelevantTopLevel(w)) {
        return;
    }

    QPlatformNativeInterface *native = qGuiApp->platformNativeInterface();
    if (!native) {
        return;
    }
    auto *toplevel = static_cast<xdg_toplevel *>(native->nativeResourceForWindow("xdg_toplevel", w));
    if (!toplevel) {
        return;
    }
    if (w->property(s_toplevelSessionAttachedProperty).toBool() || m_toplevelSessionNames.contains(w)) {
        return;
    }

    const QString name = toplevelSessionName(w);
    if (name.isEmpty()) {
        return;
    }

    w->setProperty(s_toplevelSessionAttachedProperty, true);
    m_toplevelSessionNames.insert(w, name);

    auto *session = new XdgToplevelSessionV1;
    session->init(m_session->restore_toplevel(toplevel, name));
    m_toplevelSessions.insert(w, session);
    connect(w, &QObject::destroyed, this, [this, w] {
        delete m_toplevelSessions.take(w);
        m_toplevelSessionNames.remove(w);
    });

    qDebug() << "xdg-session: restore_toplevel" << w << name;
}

void KWaylandIntegration::installColorScheme(QWindow *w)
{
    if (!m_paletteManager) {
        m_paletteManager.reset(new ServerSideDecorationPaletteManager());
    }
    if (!m_paletteManager->isActive()) {
        return;
    }
    auto palette = w->property("org.kde.plasma.integration.palette").value<ServerSideDecorationPalette *>();
    if (!palette) {
        auto s = surfaceFromWindow(w);
        if (!s) {
            return;
        }
        palette = new ServerSideDecorationPalette(m_paletteManager->create(s));
        w->setProperty("org.kde.plasma.integration.palette", QVariant::fromValue(palette));
    }
    if (palette) {
        palette->set_palette(qApp->property(s_schemePropertyName.constData()).toString());
    }
}

void KWaylandIntegration::setAppMenu(QWindow *window, const QString &serviceName, const QString &objectPath)
{
    auto menu = window->property("org.kde.plasma.integration.appmenu").value<AppMenu *>();
    if (menu) {
        menu->set_address(serviceName, objectPath);
    }
}

wl_surface *KWaylandIntegration::surfaceFromWindow(QWindow *window)
{
    QPlatformNativeInterface *nativeInterface = qGuiApp->platformNativeInterface();
    if (!nativeInterface) {
        return nullptr;
    }
    return static_cast<wl_surface *>(nativeInterface->nativeResourceForWindow("surface", window));
}

#include "kwaylandintegration.moc"
