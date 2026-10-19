/*  This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "kwaylandintegration.h"

#include <QExposeEvent>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QWindow>
#include <QVariant>
#include <QDebug>
#include <KConfigGroup>
#include <KSharedConfig>
#include <qpa/qplatformwindow_p.h>

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
    QCoreApplication::instance()->installEventFilter(this);
    initializeSessionManagement();
}

KWaylandIntegration::~KWaylandIntegration()
{
    for (auto *session : std::as_const(m_toplevelSessions)) {
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
                waylandWindow->setSessionRestoreId(QString());

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
    // create deco
    wl_surface *s = surfaceFromWindow(w);
    if (!s) {
        return;
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

    const QString appId = QGuiApplication::desktopFileName().isEmpty()
        ? QCoreApplication::applicationName()
        : QGuiApplication::desktopFileName();
    if (appId.isEmpty()) {
        return;
    }

    KConfigGroup group(KSharedConfig::openConfig(), s_sessionConfigGroup);
    m_sessionId = group.readEntry(appId, QString());
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
    KConfigGroup group(KSharedConfig::openConfig(), s_sessionConfigGroup);
    group.writeEntry(appId, id);
    group.sync();
}

QString KWaylandIntegration::toplevelSessionName(QWindow *w) const
{
    const QVariant explicitId = w->property(s_windowRestoreIdProperty);
    if (explicitId.isValid() && !explicitId.toString().isEmpty()) {
        return explicitId.toString();
    }

    // objectName is the only Qt property intended to be a stable, application-defined object identifier.
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
    const QByteArray material = (appId + QLatin1Char('/') + identity).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
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
