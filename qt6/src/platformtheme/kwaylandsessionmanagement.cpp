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
class NativeSessionPersistence : public QObject
{
public:
    explicit NativeSessionPersistence(QObject *parent)
        : QObject(parent)
    {
        qApp->installEventFilter(this);
        // Save only on shutdown so startup cannot overwrite a just-closed session.
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            saveSessionId();
        });
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (!m_initialized && event->type() == QEvent::PlatformSurface) {
            auto *window = qobject_cast<QWindow *>(object);
            auto *surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
            if (window && !window->parent() && !window->flags().testFlag(Qt::ForeignWindow) && window->type() != Qt::Popup
                && window->type() != Qt::ToolTip && surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated) {
                // Qt 6.12 Wayland creates the wl_surface in the platform window
                // constructor, but defers the xdg_toplevel until setVisible().
                initialize();
            }
        }
        return false;
    }

private:
    void initialize()
    {
        // Never replace a backend after an earlier eligible window may have
        // acquired its xdg_toplevel, even if initialization cannot finish now.
        m_initialized = true;
        auto *application = QGuiApplicationPrivate::instance();
        if (!application->session_manager) {
            return;
        }
        if (qApp->testAttribute(Qt::AA_DisableSessionManager) || qApp->isSessionRestored()) {
            return;
        }
        const QString appId = QGuiApplication::desktopFileName().isEmpty() ? QCoreApplication::applicationName() : QGuiApplication::desktopFileName();
        if (appId.isEmpty()) {
            return;
        }

        // A passed connection FD can bypass WAYLAND_DISPLAY. Skip persistence
        // rather than assign it to an unrelated/default socket path.
        if (qEnvironmentVariableIsSet("WAYLAND_SOCKET")) {
            return;
        }
        const QString runtimePath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtimePath.isEmpty()) {
            return;
        }
        QString displayPath = qEnvironmentVariable("WAYLAND_DISPLAY");
        if (displayPath.isEmpty()) {
            displayPath = QStringLiteral("wayland-0");
        }
        if (QDir::isRelativePath(displayPath)) {
            displayPath = QDir(runtimePath).absoluteFilePath(displayPath);
        }
        const QString canonicalDisplay = QFileInfo(displayPath).canonicalFilePath();
        if (!canonicalDisplay.isEmpty()) {
            displayPath = canonicalDisplay;
        }
        // Reuse the scope while the machine, desktop and socket path stay the
        // same. A socket-path change selects a new scope; avoid per-login IDs.
        QByteArray machine = QSysInfo::machineUniqueId();
        if (machine.isEmpty()) {
            machine = QSysInfo::machineHostName().toUtf8();
        }
        const QByteArray environment = machine + '\0' + qgetenv("XDG_CURRENT_DESKTOP") + '\0' + QDir::cleanPath(displayPath).toUtf8();
        m_environment = QString::fromLatin1(QCryptographicHash::hash(environment, QCryptographicHash::Sha256).toHex());

        m_config = KSharedConfig::openStateConfig();
        m_appId = appId;
        QString configPath = m_config->name();
        if (QDir::isRelativePath(configPath)) {
            configPath = QDir(QStandardPaths::writableLocation(m_config->locationType())).absoluteFilePath(configPath);
        }
        const QFileInfo configFile(configPath);
        QString canonicalPath = configFile.canonicalFilePath();
        if (canonicalPath.isEmpty()) {
            // The state file need not exist on the application's first run.
            const QString directory = configFile.dir().canonicalPath();
            canonicalPath = directory.isEmpty() ? configFile.absoluteFilePath() : QDir(directory).filePath(configFile.fileName());
        }
        const QByteArray identity = (QDir::cleanPath(canonicalPath) + QLatin1Char('/') + appId + QLatin1Char('/') + m_environment).toUtf8();
        const QString lockName = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
        m_restoreLock = std::make_unique<QLockFile>(runtimePath + QLatin1String("/plasma-qt-session-") + lockName + QLatin1String(".lock"));
        // Keep live restoration locks valid regardless of age. QLockFile still
        // detects a terminated owner and can recover its stale lock file.
        m_restoreLock->setStaleLockTime(0);
        // Only one live process may restore the persisted session. Other
        // processes retain their independent Qt sessions and save on exit.
        // A saved ID can be restored even before Qt receives the new session's ID.
        if (!m_restoreLock->tryLock(0)) {
            return;
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
    }

    void saveSessionId()
    {
        if (!m_restoreLock || !QGuiApplicationPrivate::instance()->session_manager) {
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

    bool m_initialized = false;
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
