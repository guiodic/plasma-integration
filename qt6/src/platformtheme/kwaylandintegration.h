/*  This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2015 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#ifndef KWAYLANDINTEGRATION_H
#define KWAYLANDINTEGRATION_H

#include "kdeplatformtheme.h"
#include <QHash>
#include <QString>
#include <QObject>
#include <QtWaylandClient/QWaylandClientExtensionTemplate>

class QWindow;
class ServerSideDecorationPaletteManager;
class XdgSessionManagerV1;
class XdgSessionV1;

class KWaylandIntegration : public QObject
{
    Q_OBJECT
public:
    explicit KWaylandIntegration(KdePlatformTheme *platformTheme);
    ~KWaylandIntegration() override;

    void setPalette(QWindow *window, const QString &paletteName);
    void saveSessionId(const QString &id);

    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    static bool isRelevantTopLevel(QWindow *w);
    static struct wl_surface *surfaceFromWindow(QWindow *w);
    void shellSurfaceCreated(QWindow *w);
    void shellSurfaceDestroyed(QWindow *w);

    void installColorScheme(QWindow *w);
    void initializeSessionManagement();
    void startSessionManagement();
    void registerToplevelSession(QWindow *w);
    QString toplevelSessionName(QWindow *w) const;
    QScopedPointer<ServerSideDecorationPaletteManager> m_paletteManager;
    QScopedPointer<XdgSessionManagerV1> m_sessionManager;
    QScopedPointer<XdgSessionV1> m_session;
    QString m_sessionId;
    QHash<QWindow *, QString> m_toplevelSessionNames;

    KdePlatformTheme *m_platformTheme;
};

#endif
