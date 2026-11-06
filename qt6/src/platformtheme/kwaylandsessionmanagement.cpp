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
// Keep separate persistence scopes for different machines, desktops and
// Wayland socket paths, while allowing reuse across logins.
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

    const QByteArray environment = machine + '\0' + desktop + '\0' + QDir::cleanPath(socketPath).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(environment, QCryptographicHash::Sha256).toHex());
}

QString sessionRestoreLockPath(const KSharedConfig::Ptr &config, const QString &appId, const QString &environment, const QString &runtimePath)
{
    QString configPath = config->name();
    if (QDir::isRelativePath(configPath)) {
        configPath = QDir(QStandardPaths::writableLocation(config->locationType())).absoluteFilePath(configPath);
    }
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

        m_config = KSharedConfig::openStateConfig();
        m_appId = appId;
        m_restoreLock = std::make_unique<QLockFile>(sessionRestoreLockPath(m_config, appId, m_environment, runtimePath));
        // Keep live restoration locks valid regardless of age. QLockFile still
        // detects a terminated owner and can recover its stale lock file.
        m_restoreLock->setStaleLockTime(0);
        // Only one live process may restore the persisted session. Other
        // processes retain their independent Qt sessions and save on exit.
        // A saved ID can be restored even before Qt receives the new session's ID.
        if (!m_restoreLock->tryLock(0)) {
            return true;
        }

        // The shared config may have been cached before another process saved.
        // Read independently without reparsing the application's shared object.
        KConfig config(m_config->name(), KConfig::NoGlobals, m_config->locationType());
        const KConfigGroup sessions(&config, QStringLiteral("XdgSessionManagement"));
        const QString savedId = sessions.group(m_environment).readEntry(appId, QString());
        if (!savedId.isEmpty() && savedId != qApp->sessionId()) {
            auto *manager = static_cast<QSessionManagerPrivate *>(QObjectPrivate::get(application->session_manager));
            auto *previous = manager->platformSessionManager;
            // Qt 6.12 has no public session-ID setter. Replace the native backend
            // before the first xdg_toplevel is associated with it.
            manager->platformSessionManager = application->platformIntegration()->createPlatformSessionManager(savedId, qApp->sessionKey());
            delete previous;
        }
        return true;
    }

    void saveSessionId()
    {
        if (!QGuiApplicationPrivate::instance()->session_manager) {
            return;
        }
        const QString id = qApp->sessionId();
        if (id.isEmpty()) {
            return;
        }
        // Read fresh state because another process may have saved since startup.
        KConfig config(m_config->name(), KConfig::NoGlobals, m_config->locationType());
        // Keep the parent mutable so its subgroup supports writing and sync().
        KConfigGroup sessions(&config, QStringLiteral("XdgSessionManagement"));
        KConfigGroup group = sessions.group(m_environment);
        if (group.readEntry(m_appId, QString()) == id) {
            return;
        }
        group.writeEntry(m_appId, id);
        if (!group.sync()) {
            qWarning() << "Failed to persist native Wayland session ID in" << config.name();
        }
    }

    KSharedConfig::Ptr m_config;
    QString m_appId;
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
