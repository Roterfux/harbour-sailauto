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

#ifdef SAILAUTO_DESKTOP
// Headless desktop build for testing against OpenAuto / tools/hu_sim.py.
#include <QGuiApplication>

int main(int argc, char *argv[])
{
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
#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QScopedPointer>
#include <sailfishapp.h>

int main(int argc, char *argv[])
{
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
    return app->exec();
}
#endif
