#include "controller.h"

#include <QCoreApplication>
#include <csignal>

// SIGTERM/SIGINT: quit the event loop so the controller restores USB and
// audio routing instead of leaving them switched.
static void quitOnSignal(int)
{
    if (QCoreApplication::instance())
        QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
}

#include "phone/phonebook.h"

// argv[1] == "--dump-contacts": run as the contacts helper (setgid copy).
static bool isContactsHelper(int argc, char *argv[])
{
    return argc > 1 && qstrcmp(argv[1], "--dump-contacts") == 0;
}

#ifdef SAILAUTO_DESKTOP
// Headless desktop build for testing against OpenAuto / tools/hu_sim.py.
#include <QGuiApplication>

int main(int argc, char *argv[])
{
    if (isContactsHelper(argc, argv)) return PhoneBook::dumpMain(argc, argv);
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("harbour-sailauto"));
    app.setApplicationName(QStringLiteral("harbour-sailauto"));
    std::signal(SIGTERM, quitOnSignal);
    std::signal(SIGINT, quitOnSignal);
    Controller c;
    if (qEnvironmentVariableIsSet("SAILAUTO_USB_DEVICE")) c.startUsb();
    else if (!c.startListening()) return 1;
    return app.exec();
}

#else
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QScopedPointer>
#include <QTimer>
#include <sailfishapp.h>

// Only one SailAuto may run: /dev/usb_accessory can be open only once.
// A second start (launcher + autostart) hands over to the first and exits.
static const char kBusName[] = "org.roterfux.harbourSailauto";

class AppBus : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.roterfux.harbourSailauto")
public:
    AppBus(Controller *c, QQuickView *v) : m_c(c), m_v(v) {}
public slots:
    void activate(bool byCar)
    {
        m_v->showFullScreen();
        m_v->raise();
        m_v->requestActivate();
        if (byCar) m_c->startedByCar();
    }
private:
    Controller *m_c;
    QQuickView *m_v;
};

int main(int argc, char *argv[])
{
    if (isContactsHelper(argc, argv)) return PhoneBook::dumpMain(argc, argv);
    const bool byCar = [&]() {
        for (int i = 1; i < argc; ++i)
            if (qstrcmp(argv[i], "--autostart") == 0) return true;
        return false;
    }();
    {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (bus.isConnected() && bus.interface()->isServiceRegistered(QLatin1String(kBusName)).value()) {
            QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kBusName), QStringLiteral("/"),
                                                            QLatin1String(kBusName), QStringLiteral("activate"));
            m << byCar;
            bus.call(m, QDBus::Block, 3000);
            qInfo("SailAuto läuft schon, übergebe an die laufende Instanz");
            return 0;
        }
    }
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    app->setOrganizationName(QStringLiteral("org.roterfux"));
    app->setApplicationName(QStringLiteral("harbour-sailauto"));

    std::signal(SIGTERM, quitOnSignal);
    std::signal(SIGINT, quitOnSignal);
    Controller controller;
    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    view->setSource(SailfishApp::pathToMainQml());
    view->show();
    AppBus appBus(&controller, view.data());
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QLatin1String(kBusName)) ||
        !bus.registerObject(QStringLiteral("/"), &appBus, QDBusConnection::ExportAllSlots))
        qWarning("SailAuto: D-Bus-Name nicht registriert: %s", qPrintable(bus.lastError().message()));
    // Launched by the autostart watcher (car asked for Android Auto).
    if (byCar)
        QTimer::singleShot(0, &controller, &Controller::startedByCar);
    return app->exec();
}

#include "main.moc"
#endif
