#include "puremaps.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QFile>
#include <QPainter>
#include <QSvgRenderer>
#include <QVariantMap>

namespace {
const char kService[] = "io.github.rinigus.PureMaps";
const char kPath[] = "/io/github/rinigus/PureMaps/navigator";
const char kIface[] = "io.github.rinigus.PureMaps.navigator";
}

bool NavState::operator==(const NavState &o) const
{
    return app == o.app && running == o.running && hasRoute == o.hasRoute && alongRoute == o.alongRoute
           && icon == o.icon && manDist == o.manDist && manTime == o.manTime && street == o.street
           && narrative == o.narrative && destDist == o.destDist && destTime == o.destTime
           && destEta == o.destEta && progress == o.progress && roundaboutExit == o.roundaboutExit;
}

PureMapsClient::PureMapsClient(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &PureMapsClient::poll);
}

void PureMapsClient::start()
{
    poll();
    m_timer.start();
}

void PureMapsClient::stop()
{
    m_timer.stop();
}

void PureMapsClient::poll()
{
    NavState s;
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    s.app = bus && bus->isServiceRegistered(QLatin1String(kService)).value();
    if (s.app) {
        QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                        QStringLiteral("org.freedesktop.DBus.Properties"),
                                                        QStringLiteral("GetAll"));
        m << QLatin1String(kIface);
        const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 500);
        if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
            QVariantMap p;
            r.arguments().first().value<QDBusArgument>() >> p;
            s.running = p.value(QStringLiteral("running")).toBool();
            s.hasRoute = p.value(QStringLiteral("hasRoute")).toBool();
            s.alongRoute = p.value(QStringLiteral("alongRoute")).toBool();
            s.icon = p.value(QStringLiteral("icon")).toString();
            s.manDist = p.value(QStringLiteral("manDist")).toString();
            s.manTime = p.value(QStringLiteral("manTime")).toString();
            s.street = p.value(QStringLiteral("street")).toString();
            s.narrative = p.value(QStringLiteral("narrative")).toString();
            s.destDist = p.value(QStringLiteral("destDist")).toString();
            s.destTime = p.value(QStringLiteral("destTime")).toString();
            s.destEta = p.value(QStringLiteral("destEta")).toString();
            s.progress = p.value(QStringLiteral("progress")).toInt();
            s.roundaboutExit = p.value(QStringLiteral("roundaboutExit")).toInt();
        }
    }
    if (s.app != m_state.app)
        emit log(s.app ? QStringLiteral("Navigation: Pure Maps läuft") : QStringLiteral("Navigation: Pure Maps beendet"));
    if (s.running != m_state.running)
        emit log(s.running ? QStringLiteral("Navigation: Route aktiv (%1, %2)").arg(s.destDist, s.destTime)
                           : QStringLiteral("Navigation: keine aktive Route"));
    if (s != m_state) {
        m_state = s;
        emit changed();
    }
}

QImage PureMapsClient::icon(const QString &name, int size)
{
    if (name.isEmpty()) return QImage();
    const QString key = name + QLatin1Char('@') + QString::number(size);
    auto it = m_icons.constFind(key);
    if (it != m_icons.constEnd()) return *it;
    QImage img;
    const QString base = QStringLiteral("/usr/share/harbour-pure-maps/qml/icons/navigation/");
    for (const QString &f : {base + name + QStringLiteral("-white.svg"), base + name + QStringLiteral(".svg")}) {
        if (!QFile::exists(f)) continue;
        QSvgRenderer svg(f);
        if (!svg.isValid()) continue;
        img = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        svg.render(&p);
        break;
    }
    m_icons.insert(key, img);
    return img;
}

void PureMapsClient::stopNavigation()
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                    QLatin1String(kIface), QStringLiteral("Stop"));
    QDBusConnection::sessionBus().call(m, QDBus::Block, 1000);
    emit log(QStringLiteral("Navigation: vom Auto aus gestoppt"));
}

bool PureMapsClient::search(const QString &text)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QStringLiteral("/io/github/rinigus/PureMaps"),
                                                    QStringLiteral("io.github.rinigus.PureMaps"), QStringLiteral("Search"));
    m << text;
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        emit log(QStringLiteral("Navigation: Suche an Pure Maps fehlgeschlagen (%1)").arg(r.errorName()));
        return false;
    }
    emit log(QStringLiteral("Navigation: Ziel an Pure Maps geschickt"));
    return true;
}

bool PureMapsClient::startNavigation()
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QLatin1String(kPath),
                                                    QLatin1String(kIface), QStringLiteral("Start"));
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    const bool ok = r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty() && r.arguments().first().toBool();
    emit log(ok ? QStringLiteral("Navigation: vom Auto aus gestartet")
                : QStringLiteral("Navigation: Start nicht möglich (%1)").arg(r.errorMessage().isEmpty() ? QStringLiteral("keine Route") : r.errorMessage()));
    QTimer::singleShot(300, this, &PureMapsClient::poll);
    return ok;
}

bool PureMapsClient::showPoi(const QString &title, double lat, double lon)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QStringLiteral("/io/github/rinigus/PureMaps"),
                                                    QStringLiteral("io.github.rinigus.PureMaps"), QStringLiteral("ShowPoi"));
    m << title << lat << lon;
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 3000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        emit log(QStringLiteral("Navigation: Ziel an Pure Maps fehlgeschlagen (%1)").arg(r.errorName()));
        return false;
    }
    emit log(QStringLiteral("Navigation: gewähltes Ziel an Pure Maps geschickt"));
    return true;
}
