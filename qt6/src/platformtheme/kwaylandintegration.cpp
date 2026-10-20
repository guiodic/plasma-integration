/*  This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "kwaylandintegration.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <QWindow>
#include <QVariant>
#include <QDebug>
#include <QtWaylandClient/private/qwaylandwindow_p.h>
#include <KConfigGroup>
#include <KSharedConfig>

#include "qwayland-server-decoration-palette.h"
#include "qwayland-xdg-session-management-v1.h"
#include "qwayland-xdg-shell.h"

#include <KWindowEffects>

static const QByteArray s_schemePropertyName = QByteArrayLiteral("KDE_COLOR_SCHEME_PATH");
static const QByteArray s_blurBehindPropertyName = QByteArrayLiteral("ENABLE_BLUR_BEHIND_HINT");
static const QString s_sessionConfigGroup = QStringLiteral("XdgSessionManagement");
static const char s_windowRestoreIdProperty[] = "KDE_SESSION_RESTORE_ID";
static const char s_toplevelSessionAttachedProperty[] = "org.kde.plasma.integration.xdgSessionAttached";

class XdgSessionManagerV1 : public QWaylandClientExtensionTemplate<XdgSessionManagerV1, &QtWayland::xdg_session_manager_v1::destroy>,
                            public QtWayland::xdg_session_manager_v1
{
public:
    XdgSessionManagerV1()
        : QWaylandClientExtensionTemplate<XdgSessionManagerV1, &QtWayland::xdg_session_manager_v1::destroy>(1)
    {
        initialize();
    }
};

class XdgSessionV1 : public QtWayland::xdg_session_v1
{
public:
    void setIntegration(KWaylandIntegration *integration)
    {
        m_integration = integration;
    }

    ~XdgSessionV1() override
    {
        destroy();
    }

protected:
    void xdg_session_v1_created(const QString &id) override
    {
        if (m_integration) {
            m_integration->saveSessionId(id);
        }
    }

    void xdg_session_v1_restored() override
    {
    }

    void xdg_session_v1_replaced() override
    {
        destroy();
    }

private:
    KWaylandIntegration *m_integration = nullptr;
};

class XdgToplevelSessionV1 : public QtWayland::xdg_toplevel_session_v1
{
public:
    using QtWayland::xdg_toplevel_session_v1::xdg_toplevel_session_v1;

    ~XdgToplevelSessionV1() override
    {
        destroy();
    }
};

class ServerSideDecorationPaletteManager : public QWaylandClientExtensionTemplate<ServerSideDecorationPaletteManager>,
                                           public QtWayland::org_kde_kwin_server_decoration_palette_manager
{
    Q_OBJECT
public:
    ServerSideDecorationPaletteManager()
        : QWaylandClientExtensionTemplate<ServerSideDecorationPaletteManager>(1)
    {
        initialize();
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

Q_DECLARE_METATYPE(ServerSideDecorationPalette *);

KWaylandIntegration::KWaylandIntegration(KdePlatformTheme *platformTheme)
    : QObject()
    , m_platformTheme(platformTheme)
{
    // KMainWindow can set a window role after the platform surface is created.
    // On QtWayland 6.12 this becomes a session restore ID and Qt automatically
    // associates the xdg_toplevel with a session, conflicting with the
    // association managed by plasma-integration below.
    QCoreApplication::setAttribute(Qt::AA_DisableSessionManager);

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
    if (event->type() == QEvent::ApplicationPaletteChange) {
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
            if (auto waylandWindow = w->nativeInterface<QNativeInterface::Private::QWaylandWindow>()) {
                // QtWayland 6.12 may call restore_toplevel() while creating the
                // xdg_toplevel if this ID is non-empty. Session restoration is
                // handled below by plasma-integration instead, so clear it
                // before the shell role is created to avoid associating one
                // toplevel with two sessions.
                qDebug() << "xdg-session: clearing Qt restore ID"
                         << "window=" << w;
                waylandWindow->setSessionRestoreId(QString());
                qDebug() << "xdg-session: Qt restore ID cleared"
                         << "window=" << w;

                connect(waylandWindow, &QNativeInterface::Private::QWaylandWindow::surfaceCreated, this, [this, w] {
                    shellSurfaceCreated(w);
                });
                connect(waylandWindow, &QNativeInterface::Private::QWaylandWindow::surfaceDestroyed, this, [this, w] {
                    shellSurfaceDestroyed(w);
                });
                connect(waylandWindow, &QNativeInterface::Private::QWaylandWindow::surfaceRoleCreated, this, [this, w] {
                    registerToplevelSession(w);
                });
                if (waylandWindow->surface()) {
                    shellSurfaceCreated(w);
                }
            }
        }
    }
    return false;
}

void KWaylandIntegration::shellSurfaceCreated(QWindow *w)
{
    if (!isRelevantTopLevel(w)) {
        return;
    }
    // set colorscheme hint
    if (qApp->property(s_schemePropertyName.constData()).isValid()) {
        installColorScheme(w);
    }
    const auto blurBehindProperty = w->property(s_blurBehindPropertyName.constData());
    if (blurBehindProperty.isValid()) {
        KWindowEffects::enableBlurBehind(w, blurBehindProperty.toBool());
    }
}

void KWaylandIntegration::shellSurfaceDestroyed(QWindow *w)
{
    delete m_toplevelSessions.take(w);
    m_toplevelSessionNames.remove(w);
    w->setProperty(s_toplevelSessionAttachedProperty, false);

    auto decoPallete = w->property("org.kde.plasma.integration.palette").value<ServerSideDecorationPalette *>();
    if (decoPallete) {
        delete decoPallete;
    }
    w->setProperty("org.kde.plasma.integration.palette", QVariant());
}

void KWaylandIntegration::initializeSessionManagement()
{
    // QtWayland has its own experimental xdg-session-management client.
    // Running both clients in the same process can associate one xdg_toplevel
    // with two sessions, which is a fatal protocol error. When Qt's native
    // client is explicitly enabled, leave session management to Qt instead.
    const QByteArray qtSessionManager = qgetenv("QT_WAYLAND_ENABLE_XX_SESSION_MANAGER");
    if (!qtSessionManager.isEmpty() && qtSessionManager != "0") {
        qWarning() << "xdg-session: Qt native session management is enabled;"
                      " disabling plasma-integration session management to avoid"
                      " duplicate toplevel associations";
        return;
    }

    m_sessionManager.reset(new XdgSessionManagerV1);
    connect(m_sessionManager.data(), &QWaylandClientExtensionTemplate<XdgSessionManagerV1, &QtWayland::xdg_session_manager_v1::destroy>::activeChanged,
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

    m_sessionAppId = QGuiApplication::desktopFileName().isEmpty()
        ? QCoreApplication::applicationName()
        : QGuiApplication::desktopFileName();
    if (m_sessionAppId.isEmpty()) {
        return;
    }

    const auto config = KSharedConfig::openConfig();
    qDebug() << "xdg-session: identity"
             << "desktopFileName=" << QGuiApplication::desktopFileName()
             << "applicationName=" << QCoreApplication::applicationName()
             << "organizationName=" << QCoreApplication::organizationName()
             << "config=" << config->name();

    KConfigGroup group(config, s_sessionConfigGroup);
    m_sessionId = group.readEntry(m_sessionAppId, QString());

    qDebug() << "xdg-session: start"
             << "appId=" << m_sessionAppId
             << "config=" << config->name()
             << "storedSessionId=" << m_sessionId;

    m_session.reset(new XdgSessionV1);
    m_session->setIntegration(this);
    if (m_sessionId.isEmpty()) {
        m_session->init(m_sessionManager->get_session(XdgSessionManagerV1::reason_launch, QString()));
    } else {
        m_session->init(m_sessionManager->get_session(XdgSessionManagerV1::reason_session_restore, m_sessionId));
    }

    // A window can create its xdg_toplevel before the session-manager global
    // becomes active. Its surfaceRoleCreated signal would then be missed by
    // registerToplevelSession(), so also inspect windows that already exist.
    const auto topLevelWindows = QGuiApplication::topLevelWindows();
    for (QWindow *w : topLevelWindows) {
        registerToplevelSession(w);
    }
}

void KWaylandIntegration::saveSessionId(const QString &id)
{
    if (id.isEmpty()) {
        return;
    }

    // Keep the same key that was used when starting the session. In some
    // applications desktopFileName() is populated only after startup.
    if (m_sessionAppId.isEmpty()) {
        return;
    }

    m_sessionId = id;
    const auto config = KSharedConfig::openConfig();
    qDebug() << "xdg-session: identity"
             << "desktopFileName=" << QGuiApplication::desktopFileName()
             << "applicationName=" << QCoreApplication::applicationName()
             << "organizationName=" << QCoreApplication::organizationName()
             << "config=" << config->name();

    KConfigGroup group(config, s_sessionConfigGroup);

    qDebug() << "xdg-session: save"
             << "appId=" << m_sessionAppId
             << "config=" << config->name()
             << "sessionId=" << id;

    group.writeEntry(m_sessionAppId, id);
    group.sync();

    qDebug() << "xdg-session: save complete"
             << "appId=" << m_sessionAppId
             << "storedSessionId=" << group.readEntry(m_sessionAppId, QString());
}

QString KWaylandIntegration::toplevelSessionName(QWindow *w) const
{
    const QVariant explicitId = w->property(s_windowRestoreIdProperty);
    QString name;
    if (explicitId.isValid() && !explicitId.toString().isEmpty()) {
        // Honour application-provided IDs when possible. The collision check
        // below also protects against applications reusing the same explicit ID.
        name = explicitId.toString();
    } else {
        // Keep the identifier stable across runs for windows with a stable
        // objectName. Titles and class/type are fallbacks, not guaranteed unique.
        QString identity = w->objectName();
        if (identity.isEmpty()) {
            identity = w->title();
        }
        if (identity.isEmpty()) {
            identity = QStringLiteral("%1:%2").arg(w->metaObject()->className()).arg(int(w->type()));
        }

        // Use the same application identity that was fixed when the session started.
        // desktopFileName() can be populated later and must not change the window IDs.
        const QString appId = m_sessionAppId;
        const QByteArray material = (appId + QLatin1Char('/') + identity).toUtf8();
        name = QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
    }

    // xdg-session-management requires every toplevel ID in a session to be
    // unique. Different QWindows can have the same objectName/title (or the
    // same explicit restore ID), so disambiguate against currently registered
    // windows while retaining the original stable ID when there is no clash.
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
        qWarning() << "xdg-session: duplicate toplevel identity; using unique ID"
                   << "window=" << static_cast<void *>(w)
                   << "objectName=" << w->objectName()
                   << "title=" << w->title()
                   << "originalName=" << baseName
                   << "uniqueName=" << name;
    }
    return name;
}

void KWaylandIntegration::registerToplevelSession(QWindow *w)
{
    if (!m_sessionManager || !m_sessionManager->isActive() || !m_session || !isRelevantTopLevel(w)) {
        return;
    }
    auto waylandWindow = w->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!waylandWindow) {
        return;
    }
    auto *toplevel = waylandWindow->surfaceRole<xdg_toplevel>();
    if (!toplevel) {
        return;
    }

    // A QWindow may emit surfaceRoleCreated more than once over its lifetime,
    // and multiple platform-theme integrations must not associate the same
    // xdg_toplevel with more than one session object.
    if (w->property(s_toplevelSessionAttachedProperty).toBool() || m_toplevelSessionNames.contains(w)) {
        return;
    }

    const QString name = toplevelSessionName(w);
    if (name.isEmpty()) {
        return;
    }

    // Set the guard before sending the protocol request: the compositor treats
    // a second association as a fatal protocol error.
    w->setProperty(s_toplevelSessionAttachedProperty, true);
    m_toplevelSessionNames.insert(w, name);

    auto *toplevelSession = new XdgToplevelSessionV1;
    toplevelSession->init(m_session->restore_toplevel(toplevel, name));
    m_toplevelSessions.insert(w, toplevelSession);

    connect(w, &QObject::destroyed, this, [this, w] {
        delete m_toplevelSessions.take(w);
        m_toplevelSessionNames.remove(w);
    });

    qDebug() << "xdg-session: restore_toplevel"
             << "window=" << static_cast<void *>(w)
             << "surface=" << static_cast<void *>(waylandWindow->surface())
             << "toplevel=" << static_cast<void *>(toplevel)
             << "session=" << static_cast<void *>(m_session.data())
             << "name=" << name;
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

wl_surface *KWaylandIntegration::surfaceFromWindow(QWindow *window)
{
    auto waylandWindow = window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
    if (!waylandWindow) {
        return nullptr;
    }
    return waylandWindow->surface();
}

#include "kwaylandintegration.moc"

#include "moc_kwaylandintegration.cpp"
