#include "phonestatus.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QVariantMap>

namespace {
const int kTimeout = 400; // ms per D-Bus call; the car frame loop must not stall

QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}

QDBusMessage call(const QString &service, const QString &path, const QString &iface, const QString &method)
{
    return QDBusConnection::systemBus().call(QDBusMessage::createMethodCall(service, path, iface, method),
                                             QDBus::Block, kTimeout);
}

// a(oa{sv}) as returned by oFono GetModems and ConnMan GetServices
QList<QPair<QString, QVariantMap>> pathPropsList(const QDBusMessage &reply)
{
    QList<QPair<QString, QVariantMap>> out;
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return out;
    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    arg.beginArray();
    while (!arg.atEnd()) {
        QDBusObjectPath p;
        QVariantMap m;
        arg.beginStructure();
        arg >> p >> m;
        arg.endStructure();
        out.append(qMakePair(p.path(), m));
    }
    arg.endArray();
    return out;
}

QString techName(const QString &t)
{
    if (t == QLatin1String("nr")) return QStringLiteral("5G");
    if (t == QLatin1String("lte")) return QStringLiteral("4G");
    if (t == QLatin1String("umts") || t == QLatin1String("hspa")) return QStringLiteral("3G");
    if (t == QLatin1String("gsm") || t == QLatin1String("edge") || t == QLatin1String("gprs")) return QStringLiteral("2G");
    return t.toUpper();
}
} // namespace

bool PhoneState::operator==(const PhoneState &o) const
{
    return battery == o.battery && charging == o.charging && signal == o.signal && operatorName == o.operatorName
           && tech == o.tech && roaming == o.roaming && wifi == o.wifi && wifiStrength == o.wifiStrength
           && wifiName == o.wifiName && mobileData == o.mobileData && btOn == o.btOn
           && btConnected == o.btConnected && btDevice == o.btDevice;
}

QString PhoneState::summary() const
{
    // One-pass multi-arg: values (Wi-Fi names...) may contain '%'.
    const QString bat = battery < 0 ? QStringLiteral("?")
                                    : QString::number(battery) + QStringLiteral(" %") + (charging ? QStringLiteral(" (lädt)") : QString());
    QString net = QStringLiteral("keins");
    if (signal >= 0) {
        net = operatorName + QLatin1Char(' ') + tech + QLatin1Char(' ') + QString::number(signal) + QStringLiteral(" %");
        if (roaming) net += QStringLiteral(" Roaming");
        if (mobileData) net += QStringLiteral(" Daten an");
    }
    const QString wl = wifi ? wifiName + QLatin1Char(' ') + QString::number(wifiStrength) + QStringLiteral(" %") : QStringLiteral("aus");
    QString bt = QStringLiteral("aus");
    if (btOn) bt = btConnected ? QStringLiteral("an, ") + QString::number(btConnected) + QStringLiteral(" verbunden (") + btDevice + QLatin1Char(')')
                               : QStringLiteral("an");
    return QStringLiteral("Akku %1, Netz %2, WLAN %3, Bluetooth %4").arg(bat, net, wl, bt);
}

PhoneStatus::PhoneStatus(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(10000);
    connect(&m_timer, &QTimer::timeout, this, &PhoneStatus::poll);
}

void PhoneStatus::start()
{
    m_logged = false;
    poll();
    m_timer.start();
}

void PhoneStatus::stop()
{
    m_timer.stop();
}

void PhoneStatus::poll()
{
    PhoneState s;
    readBattery(s);
    if (QDBusConnection::systemBus().isConnected()) {
        readModem(s);
        readConnman(s);
        readBluetooth(s);
    }
    if (!m_logged) {
        m_logged = true;
        emit log(QStringLiteral("Telefon-Status: ") + s.summary());
    }
    if (s != m_state) {
        m_state = s;
        emit changed();
    }
}

void PhoneStatus::readBattery(PhoneState &s) const
{
    const QDir dir(QStringLiteral("/sys/class/power_supply"));
    for (const QString &e : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString base = dir.filePath(e) + QLatin1Char('/');
        if (readFile(base + QStringLiteral("type")) != QLatin1String("Battery")) continue;
        bool ok = false;
        const int cap = readFile(base + QStringLiteral("capacity")).toInt(&ok);
        if (!ok) continue;
        s.battery = qBound(0, cap, 100);
        const QString st = readFile(base + QStringLiteral("status"));
        s.charging = st == QLatin1String("Charging") || st == QLatin1String("Full");
        return;
    }
    // Sailfish statefs
    bool ok = false;
    const int cap = readFile(QStringLiteral("/run/state/namespaces/Battery/ChargePercentage")).toInt(&ok);
    if (ok) {
        s.battery = qBound(0, cap, 100);
        s.charging = readFile(QStringLiteral("/run/state/namespaces/Battery/IsCharging")) == QLatin1String("1");
    }
}

void PhoneStatus::readModem(PhoneState &s) const
{
    const auto modems = pathPropsList(call(QStringLiteral("org.ofono"), QStringLiteral("/"),
                                           QStringLiteral("org.ofono.Manager"), QStringLiteral("GetModems")));
    for (const auto &m : modems) {
        if (!m.second.value(QStringLiteral("Online")).toBool()) continue;
        const QDBusMessage r = call(QStringLiteral("org.ofono"), m.first,
                                    QStringLiteral("org.ofono.NetworkRegistration"), QStringLiteral("GetProperties"));
        if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty()) continue;
        QVariantMap p;
        r.arguments().first().value<QDBusArgument>() >> p;
        const QString status = p.value(QStringLiteral("Status")).toString();
        if (status != QLatin1String("registered") && status != QLatin1String("roaming")) continue;
        s.signal = qBound(0, p.value(QStringLiteral("Strength")).toInt(), 100);
        s.operatorName = p.value(QStringLiteral("Name")).toString();
        s.tech = techName(p.value(QStringLiteral("Technology")).toString());
        s.roaming = status == QLatin1String("roaming");
        return; // first registered SIM is enough for the bar
    }
}

void PhoneStatus::readConnman(PhoneState &s) const
{
    const auto services = pathPropsList(call(QStringLiteral("net.connman"), QStringLiteral("/"),
                                             QStringLiteral("net.connman.Manager"), QStringLiteral("GetServices")));
    for (const auto &svc : services) {
        const QVariantMap &p = svc.second;
        const QString state = p.value(QStringLiteral("State")).toString();
        if (state != QLatin1String("online") && state != QLatin1String("ready")) continue;
        const QString type = p.value(QStringLiteral("Type")).toString();
        if (type == QLatin1String("wifi") && !s.wifi) {
            s.wifi = true;
            s.wifiStrength = p.value(QStringLiteral("Strength")).toInt();
            s.wifiName = p.value(QStringLiteral("Name")).toString();
        } else if (type == QLatin1String("cellular")) {
            s.mobileData = true;
        }
    }
}

void PhoneStatus::readBluetooth(PhoneState &s) const
{
    const QDBusMessage reply = call(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                    QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                    QStringLiteral("GetManagedObjects"));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return;
    const QDBusArgument arg = reply.arguments().first().value<QDBusArgument>();
    arg.beginMap();
    while (!arg.atEnd()) {
        QDBusObjectPath path;
        arg.beginMapEntry();
        arg >> path;
        arg.beginMap();
        while (!arg.atEnd()) {
            QString iface;
            QVariantMap props;
            arg.beginMapEntry();
            arg >> iface >> props;
            arg.endMapEntry();
            if (iface == QLatin1String("org.bluez.Adapter1") && props.value(QStringLiteral("Powered")).toBool()) {
                s.btOn = true;
            } else if (iface == QLatin1String("org.bluez.Device1") && props.value(QStringLiteral("Connected")).toBool()) {
                if (s.btConnected++ == 0) s.btDevice = props.value(QStringLiteral("Alias")).toString();
            }
        }
        arg.endMap();
        arg.endMapEntry();
    }
    arg.endMap();
}
