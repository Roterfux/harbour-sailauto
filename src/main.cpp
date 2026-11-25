#include "controller.h"

#ifdef SAILAUTO_DESKTOP
// Headless desktop build for testing against OpenAuto / tools/hu_sim.py.
#include <QGuiApplication>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("harbour-sailauto"));
    app.setApplicationName(QStringLiteral("harbour-sailauto"));
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

    Controller controller;
    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->rootContext()->setContextProperty(QStringLiteral("controller"), &controller);
    view->setSource(SailfishApp::pathToMainQml());
    view->show();
    return app->exec();
}
#endif
