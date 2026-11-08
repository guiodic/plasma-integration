/*
    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "kwaylandsessionmanagement.h"

#include <QGuiApplication>

#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0) && !defined(QT_NO_SESSIONMANAGER)
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QPlatformSurfaceEvent>
#include <QSessionManager>
#include <QStandardPaths>
#include <QSysInfo>
#include <QWindow>
#include <private/qguiapplication_p.h>
#include <private/qsessionmanager_p.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformsessionmanager.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <memory>

namespace
{
// Keep separate persistence scopes for different machines, desktops, Wayland
// sockets and runtime directories, while allowing reuse across logins.
QString sessionEnvironmentKey(const QString &runtimePath)
{
    QByteArray machine = QSysInfo::machineUniqueId();
    if (machine.isEmpty()) {
        machine = QSysInfo::machineHostName().toUtf8();
    }
    const QByteArray desktop = qgetenv("XDG_CURRENT_DESKTOP");

    QString socketPath = qEnvironmentVariable("WAYLAND_DISPLAY");
    if (socketPath.isEmpty()) {
        socketPath = QStringLiteral("wayland-0");
    }
    if (QDir::isRelativePath(socketPath)) {
        socketPath = QDir(runtimePath).absoluteFilePath(socketPath);
    }
    const QString canonicalPath = QFileInfo(socketPath).canonicalFilePath();
    if (!canonicalPath.isEmpty()) {
        socketPath = canonicalPath;
    }

    // Distinct runtime directories keep independent sessions even when they
    // share the state file and connect to the same absolute Wayland socket.
    const QString canonicalRuntimePath = QFileInfo(runtimePath).canonicalFilePath();
    const QString runtimeDirectory = canonicalRuntimePath.isEmpty() ? QDir::cleanPath(runtimePath) : canonicalRuntimePath;
    const QByteArray environment = machine + '\0' + desktop + '\0' + QDir::cleanPath(socketPath).toUtf8() + '\0' + runtimeDirectory.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(environment, QCryptographicHash::Sha256).toHex());
}

QString stateConfigPath()
{
    const auto config = KSharedConfig::openStateConfig();
    const QString name = config->name();
    if (QDir::isAbsolutePath(name)) {
        return name;
    }
    return QDir(QStandardPaths::writableLocation(config->locationType())).absoluteFilePath(name);
}

QString sessionRestoreLockPath(const QString &configPath, const QString &appId, const QString &environment, const QString &runtimePath)
{
    const QFileInfo configFileInfo(configPath);
    QString canonicalPath = configFileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        // The state file need not exist on the application's first run.
        const QString directory = configFileInfo.dir().canonicalPath();
        canonicalPath = directory.isEmpty() ? configFileInfo.absoluteFilePath() : QDir(directory).filePath(configFileInfo.fileName());
    }
    const QByteArray identity = (QDir::cleanPath(canonicalPath) + QLatin1Char('/') + appId + QLatin1Char('/') + environment).toUtf8();
    const QString lockName = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
    return runtimePath + QLatin1String("/plasma-qt-session-") + lockName + QLatin1String(".lock");
}

// The compositor assigns session IDs, but Qt does not automatically reconnect
// a new application launch to its previously saved session. Persist the ID
// under the application identity so a later launch can request restoration.
// Keep separate IDs for each machine, desktop, Wayland display and runtime
// directory, since the same application can have independent sessions in them.
class NativeSessionPersistence : public QObject
{
public:
    explicit NativeSessionPersistence(QObject *parent)
        : QObject(parent)
    {
        qApp->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::PlatformSurface) {
            return false;
        }
        const auto *surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
        if (surfaceEvent->surfaceEventType() != QPlatformSurfaceEvent::SurfaceCreated) {
            return false;
        }
        const auto *window = qobject_cast<QWindow *>(object);
        if (!window || window->parent()) {
            return false;
        }
        if (window->flags().testFlag(Qt::ForeignWindow) || window->type() == Qt::Popup || window->type() == Qt::ToolTip) {
            return false;
        }

        // Qt 6.12 Wayland creates the wl_surface in the platform window
        // constructor, but defers the xdg_toplevel until setVisible().
        // Never retry after an eligible window, even if initialization
        // returns early: the window may acquire its xdg_toplevel next.
        qApp->removeEventFilter(this);
        if (!initialize()) {
            deleteLater();
            return false;
        }
        // Save only on shutdown so startup cannot overwrite a just-closed session.
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            saveSessionId();
        });
        return false;
    }

private:
    bool initialize()
    {
        auto *application = QGuiApplicationPrivate::instance();
        if (!application->session_manager) {
            return false;
        }
        if (qApp->testAttribute(Qt::AA_DisableSessionManager) || qApp->isSessionRestored()) {
            return false;
        }
        const QString appId = QGuiApplication::desktopFileName().isEmpty() ? QCoreApplication::applicationName() : QGuiApplication::desktopFileName();
        if (appId.isEmpty()) {
            return false;
        }

        // A passed connection FD can bypass WAYLAND_DISPLAY. Skip persistence
        // rather than assign it to an unrelated/default socket path.
        if (qEnvironmentVariableIsSet("WAYLAND_SOCKET")) {
            return false;
        }
        const QString runtimePath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtimePath.isEmpty()) {
            return false;
        }
        m_environment = sessionEnvironmentKey(runtimePath);

        m_stateConfigPath = stateConfigPath();
        m_appId = appId;
        m_restoreLock = std::make_unique<QLockFile>(sessionRestoreLockPath(m_stateConfigPath, appId, m_environment, runtimePath));
        // Disable age-based expiry; QLockFile still recovers locks of dead owners.
        m_restoreLock->setStaleLockTime(0);
        // Only one process restores the saved session; the others keep their
        // independent Qt sessions and may still save them on exit.
        if (!m_restoreLock->tryLock(0)) {
            m_sessionId = qApp->sessionId();
            return true;
        }

        // The shared config may have been cached before another process saved.
        // Read independently without reparsing the application's shared object.
        KConfig config(m_stateConfigPath, KConfig::NoGlobals);
        const KConfigGroup sessions(&config, QStringLiteral("XdgSessionManagement"));
        const QString savedId = sessions.group(m_environment).readEntry(appId, QString());
        if (!savedId.isEmpty() && savedId != qApp->sessionId()) {
            auto *manager = static_cast<QSessionManagerPrivate *>(QObjectPrivate::get(application->session_manager));
            auto *previous = manager->platformSessionManager;
            // Qt has already requested a new session from the compositor and
            // exposes its ID through qApp->sessionId(). Qt 6.12 has no public
            // setter, so replace the backend to request the saved session before
            // any xdg_toplevel is associated with it. On exit, save the current ID.
            manager->platformSessionManager = application->platformIntegration()->createPlatformSessionManager(savedId, qApp->sessionKey());
            delete previous;
        }
        m_sessionId = qApp->sessionId();
        return true;
    }

    void saveSessionId()
    {
        if (!QGuiApplicationPrivate::instance()->session_manager) {
            return;
        }
        const QString id = qApp->sessionId();
        if (id.isEmpty() && m_sessionId.isEmpty()) {
            return;
        }
        // All writers participate, including processes without the restore lock.
        // Hold this lock from the fresh read through sync() so invalidation
        // cannot delete an ID written between the comparison and the update.
        QLockFile updateLock(m_restoreLock->fileName() + QLatin1String(".update"));
        updateLock.setStaleLockTime(0);
        if (!updateLock.tryLock(1000)) {
            qWarning() << "Failed to lock native Wayland session state in" << m_stateConfigPath;
            return;
        }
        KConfig config(m_stateConfigPath, KConfig::NoGlobals);
        KConfigGroup sessions(&config, QStringLiteral("XdgSessionManagement"));
        KConfigGroup group = sessions.group(m_environment);
        const QString savedId = group.readEntry(m_appId, QString());
        if (id.isEmpty()) {
            // Qt clears a previously assigned ID when the compositor replaces
            // our session, e.g. after an explicit -session launch bypasses the
            // restore lock. Do not let a later launch take that session back.
            // Only invalidate our own ID; another process may have saved a
            // different session since we started.
            if (savedId != m_sessionId) {
                return;
            }
            group.deleteEntry(m_appId);
        } else {
            if (savedId == id) {
                return;
            }
            group.writeEntry(m_appId, id);
        }
        if (!group.sync()) {
            qWarning() << "Failed to persist native Wayland session ID in" << config.name();
        }
    }

    QString m_stateConfigPath;
    QString m_appId;
    QString m_sessionId;
    QString m_environment;
    std::unique_ptr<QLockFile> m_restoreLock;
};
}
#endif

void initializeWaylandSessionManagement()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0) && !defined(QT_NO_SESSIONMANAGER)
    constexpr auto property = "org.kde.plasma.integration.nativeSessionPersistence";
    if (!qApp->property(property).toBool()) {
        qApp->setProperty(property, true);
        new NativeSessionPersistence(qApp);
    }
#endif
}
