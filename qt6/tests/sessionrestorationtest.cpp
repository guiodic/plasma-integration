/*
    SPDX-License-Identifier: LGPL-2.0-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QTimer>
#include <QWindow>

#include <memory>
#include <vector>

static QString quoted(const QString &value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(array.mid(1, array.size() - 2));
}

class GeometryReceiver : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.plasma.integration.SessionRestoreTest")

public:
    QString output;
    QString expected;
    bool move = false;
    bool moveAcknowledged = false;
    bool recorded = false;
    int hold = 0;

public Q_SLOTS:
    void diagnostic(const QString &message)
    {
        moveAcknowledged = true;
        qInfo().noquote() << message;
    }

    void report(const QString &json)
    {
        if (recorded) {
            return;
        }
        recorded = true;
        QJsonObject result = QJsonDocument::fromJson(json.toUtf8()).object();
        result.insert(QStringLiteral("sessionId"), qApp->sessionId());
        result.insert(QStringLiteral("platform"), QGuiApplication::platformName());
        bool ok = !qApp->sessionId().isEmpty() && QGuiApplication::platformName() == QLatin1String("wayland");
        if (move) {
            ok = ok && result.value(QStringLiteral("x")).toInt() == 180 && result.value(QStringLiteral("y")).toInt() == 140;
        } else {
            QFile previous(expected);
            ok = ok && previous.open(QIODevice::ReadOnly);
            const QJsonObject saved = QJsonDocument::fromJson(previous.readAll()).object();
            for (const auto &key : {QStringLiteral("sessionId"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")}) {
                ok = ok && saved.contains(key) && result.value(key) == saved.value(key);
            }
        }
        result.insert(QStringLiteral("passed"), ok);
        QSaveFile file(output);
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(result).toJson()) < 0 || !file.commit()) {
            ok = false;
        }
        QTimer::singleShot(ok ? hold : 0, qApp, [ok] {
            qApp->exit(ok ? 0 : 1);
        });
    }
};

int main(int argc, char **argv)
{
    // Set the config identity before Qt loads the platform theme.
    for (int i = 1; i + 1 < argc; ++i) {
        if (QByteArray(argv[i]) == "--app-id") {
            QCoreApplication::setApplicationName(QString::fromLocal8Bit(argv[i + 1]));
        }
    }
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{QStringLiteral("app-id"), QStringLiteral("Unique application identity shared by both processes"), QStringLiteral("id")},
                       {QStringLiteral("output"), QStringLiteral("Write observed KWin geometry and Qt session ID"), QStringLiteral("file")},
                       {QStringLiteral("expected"), QStringLiteral("Compare against the first process's result"), QStringLiteral("file")},
                       {QStringLiteral("hold"), QStringLiteral("Keep the window alive after recording its geometry"), QStringLiteral("milliseconds")},
                       {QStringLiteral("move"), QStringLiteral("Move the first process's window using KWin")}});
    parser.process(app);
    if (!parser.isSet(QStringLiteral("app-id")) || !parser.isSet(QStringLiteral("output"))
        || (!parser.isSet(QStringLiteral("move")) && !parser.isSet(QStringLiteral("expected")))) {
        parser.showHelp(2);
    }
    QGuiApplication::setDesktopFileName(parser.value(QStringLiteral("app-id")));

    GeometryReceiver receiver;
    receiver.output = parser.value(QStringLiteral("output"));
    receiver.expected = parser.value(QStringLiteral("expected"));
    receiver.move = parser.isSet(QStringLiteral("move"));
    receiver.hold = parser.value(QStringLiteral("hold")).toInt();
    const QString service = QStringLiteral("org.kde.plasma.integration.SessionRestoreTest.p%1").arg(QCoreApplication::applicationPid());
    if (!QDBusConnection::sessionBus().registerService(service)
        || !QDBusConnection::sessionBus().registerObject(QStringLiteral("/Test"), &receiver, QDBusConnection::ExportAllSlots)) {
        return 2;
    }

    QLabel window(QStringLiteral("Native Qt session restoration"));
    window.setWindowTitle(parser.value(QStringLiteral("app-id")) + QStringLiteral("-%1").arg(QCoreApplication::applicationPid()));
    window.setWindowRole(QStringLiteral("main"));
    window.resize(320, 200);
    window.show();

    std::vector<std::unique_ptr<QTemporaryFile>> scripts;
    auto runScript = [&](const QString &source) {
        auto file = std::make_unique<QTemporaryFile>();
        if (!file->open() || file->write(source.toUtf8()) < 0 || !file->flush()) {
            app.exit(2);
            return;
        }
        QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"), QStringLiteral("org.kde.kwin.Scripting"));
        const QDBusReply<int> id = scripting.call(QStringLiteral("loadScript"), file->fileName(), file->fileName());
        if (!id.isValid() || id.value() < 0) {
            qWarning() << "Cannot load KWin test script:" << id.error();
            app.exit(2);
            return;
        }
        QDBusInterface script(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting/Script%1").arg(id.value()), QStringLiteral("org.kde.kwin.Script"));
        const QDBusMessage reply = script.call(QStringLiteral("run"));
        if (reply.type() == QDBusMessage::ErrorMessage) {
            qWarning() << "Cannot run KWin test script:" << reply.errorMessage();
            app.exit(2);
        }
        scripts.push_back(std::move(file));
    };

    QTimer exposure;
    QTimer inspection;
    QObject::connect(&inspection, &QTimer::timeout, [&] {
        if (receiver.recorded) {
            inspection.stop();
            return;
        }
        const QString lookup = QStringLiteral("const w = workspace.windowList().find(w => w.caption === %1);\n").arg(quoted(window.windowTitle()));
        if (receiver.move && !receiver.moveAcknowledged) {
            runScript(lookup
                      + QStringLiteral("if (w) { const g = {x:180,y:140,width:w.frameGeometry.width,height:w.frameGeometry.height}; "
                                       "w.frameGeometry = g; callDBus(%1, '/Test', 'org.kde.plasma.integration.SessionRestoreTest', "
                                       "'diagnostic', JSON.stringify({requested:g,actual:w.frameGeometry})); }")
                            .arg(quoted(service)));
        } else {
            runScript(lookup
                      + QStringLiteral("if (w) { const g = w.frameGeometry; "
                                       "callDBus(%1, '/Test', 'org.kde.plasma.integration.SessionRestoreTest', 'report', "
                                       "JSON.stringify({x:g.x,y:g.y,width:g.width,height:g.height})); }")
                            .arg(quoted(service)));
        }
    });
    QElapsedTimer deadline;
    deadline.start();
    QObject::connect(&exposure, &QTimer::timeout, [&] {
        if (!window.windowHandle()->isExposed()) {
            if (deadline.elapsed() > 60000) {
                app.exit(1);
            }
            return;
        }
        exposure.stop();
        inspection.start(2000);
    });
    exposure.start(200);
    QTimer::singleShot(120000, &app, [&] {
        app.exit(1);
    });
    return app.exec();
}

#include "sessionrestorationtest.moc"
