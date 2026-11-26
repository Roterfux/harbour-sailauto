#include "controller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusReply>
#include <QDBusInterface>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QTcpSocket>

#include "aap/usbaccessory.h"

Controller::Controller(QObject *parent) : QObject(parent)
{
    bool ok = false;
    const int envPort = qEnvironmentVariableIntValue("SAILAUTO_PORT", &ok);
    if (ok && envPort > 0) m_port = envPort;

    m_certDir = QString::fromLocal8Bit(qgetenv("SAILAUTO_CERT_DIR"));
    if (m_certDir.isEmpty())
        m_certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(m_certDir);

    m_traceBase = QString::fromLocal8Bit(qgetenv("SAILAUTO_TRACE_DIR"));
    if (m_traceBase.isEmpty())
        m_traceBase = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/SailAuto");

    connect(&m_server, &QTcpServer::newConnection, this, &Controller::onNewConnection);
    connect(&m_frameTimer, &QTimer::timeout, this, &Controller::tick);
    connect(&m_encoder, &H264Encoder::encoded, this, &Controller::onEncoded);
    connect(&m_encoder, &H264Encoder::error, this, [this](const QString &e) {
        appendLog(QStringLiteral("Encoder: ") + e);
    });
    connect(&m_encoder, &H264Encoder::restarted, this, [this](const QString &name) {
        appendLog(QStringLiteral("Encoder gewechselt: ") + name);
        m_screen.setInfo(m_session ? m_session->headUnitName() : QString(), name);
        m_queue.clear();
    });
    connect(&m_audioTest, &AudioTest::log, this, &Controller::appendLog);
    connect(&m_screen, &CarScreen::tapped, this, [this](const QString &w) {
        appendLog(QStringLiteral("Auto-Display: ") + w);
        // Music page play button = media sound test.
        if (w == QLatin1String("Play")) soundTest(QStringLiteral("media"));
        else if (w == QLatin1String("Pause")) stopSoundTest();
    });
    m_usbRetry.setInterval(2000);
    connect(&m_usbRetry, &QTimer::timeout, this, &Controller::tryOpenUsb);
    setStatus(QStringLiteral("Bereit"));
}

Controller::~Controller()
{
    delete m_trace;
    if (m_usbWanted && m_aoaRequested) requestAoa(QStringLiteral("off"));
    m_encoder.stop();
}

bool Controller::customCert() const
{
    return QFile::exists(m_certDir + QStringLiteral("/cert.pem")) && QFile::exists(m_certDir + QStringLiteral("/key.pem"));
}

QStringList Controller::addresses() const
{
    QStringList out;
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            out << a.toString();
    return out;
}

bool Controller::startListening()
{
    if (m_server.isListening()) return true;
    if (!m_server.listen(QHostAddress::AnyIPv4, static_cast<quint16>(m_port))) {
        appendLog(QStringLiteral("Port %1 nicht verfügbar: %2").arg(m_port).arg(m_server.errorString()));
        return false;
    }
    appendLog(QStringLiteral("Warte auf Head Unit auf %1:%2").arg(addresses().join(QStringLiteral(", "))).arg(m_port));
    setStatus(QStringLiteral("Warte auf Head Unit"));
    emit listeningChanged();
    return true;
}

void Controller::stopListening()
{
    disconnectHeadUnit();
    m_server.close();
    setStatus(QStringLiteral("Bereit"));
    emit listeningChanged();
}

void Controller::disconnectHeadUnit()
{
    if (m_session) m_session->requestShutdown();
}

void Controller::soundTest(const QString &kind)
{
    QString err;
    if (!m_audioTest.start(m_session, kind == QLatin1String("guidance") ? AudioTest::Guidance : AudioTest::Media, &err))
        appendLog(QStringLiteral("Tontest nicht möglich: ") + err);
}

void Controller::stopSoundTest()
{
    m_audioTest.stop();
}

void Controller::clearLog()
{
    m_log.clear();
    emit logChanged();
}

void Controller::onNewConnection()
{
    while (QTcpSocket *s = m_server.nextPendingConnection()) {
        if (m_session) {
            appendLog(QStringLiteral("Zweite Verbindung abgelehnt"));
            s->close();
            s->deleteLater();
            continue;
        }
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(s, &QTcpSocket::disconnected, this, [this]() {
            if (m_session) onSessionClosed(QStringLiteral("Verbindung getrennt"));
        });
        attachTransport(s, QStringLiteral("Head Unit verbunden: %1").arg(s->peerAddress().toString()));
    }
}

namespace {
const char kAoaRequest[] = "/run/harbour-sailauto-req/request";
const char kAoaState[] = "/run/harbour-sailauto/state";
const char kAoaLog[] = "/run/harbour-sailauto/last.log";
}

bool Controller::requestAoa(const QString &cmd)
{
    QFile f(QString::fromLatin1(kAoaRequest));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("USB: Root-Dienst nicht erreichbar (%1). Manuell: devel-su sh "
                                 "/usr/share/harbour-sailauto/aoa-gadget.sh %2")
                      .arg(f.errorString(), cmd));
        return false;
    }
    f.write(cmd.toLatin1() + '\n');
    return true;
}

QString Controller::aoaState() const
{
    QFile f(QString::fromLatin1(kAoaState));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromLatin1(f.readAll()).trimmed();
}

bool Controller::startUsb()
{
    m_usbWanted = true;
    emit listeningChanged();
    setStatus(QStringLiteral("USB wird umgeschaltet"));
    m_aoaRequested = requestAoa(QStringLiteral("on"));
    m_aoaWait.start();
    m_aoaReported = false;
    appendLog(QStringLiteral("USB: schalte auf Android-Accessory-Modus"));
    m_usbRetry.start(500);
    return true;
}

void Controller::stopUsb()
{
    const bool was = m_usbWanted;
    m_usbWanted = false;
    m_usbRetry.stop();
    if (m_session && qobject_cast<UsbAccessoryDevice *>(m_socket.data()))
        disconnectHeadUnit();
    if (was && m_aoaRequested) {
        requestAoa(QStringLiteral("off"));
        appendLog(QStringLiteral("USB: zurück auf normalen Modus"));
    }
    m_aoaRequested = false;
    emit listeningChanged();
    setStatus(listening() ? QStringLiteral("Warte auf Head Unit") : QStringLiteral("Bereit"));
}

void Controller::tryOpenUsb()
{
    if (!m_usbWanted || m_session) return;
    if (m_aoaRequested && !m_aoaReported) {
        const QString st = aoaState();
        if (st == QLatin1String("error")) {
            QFile f(QString::fromLatin1(kAoaLog));
            f.open(QIODevice::ReadOnly);
            appendLog(QStringLiteral("USB-Umschaltung fehlgeschlagen: ") + QString::fromUtf8(f.readAll()).trimmed());
            m_aoaReported = true;
            stopUsb();
            return;
        }
        if (st != QLatin1String("on")) {
            if (m_aoaWait.elapsed() < 6000) return; // keep waiting for the root service
            appendLog(QStringLiteral("USB: Root-Dienst meldet nichts (Status \"%1\"). Versuche trotzdem. "
                                     "Manuell: devel-su sh /usr/share/harbour-sailauto/aoa-gadget.sh on").arg(st));
        } else {
            appendLog(QStringLiteral("USB: Accessory-Modus an (18d1:2d00). Kabel ans Auto / an den PC."));
        }
        m_aoaReported = true;
        m_usbRetry.setInterval(2000);
        setStatus(QStringLiteral("Warte auf USB"));
    }
    QString path = QString::fromLocal8Bit(qgetenv("SAILAUTO_USB_DEVICE"));
    if (path.isEmpty()) path = QStringLiteral("/dev/usb_accessory");
    auto *dev = new UsbAccessoryDevice(path, this);
    if (!dev->open(QIODevice::ReadWrite)) {
        static QString lastError;
        if (dev->errorString() != lastError) {
            lastError = dev->errorString();
            appendLog(QStringLiteral("USB: /dev/usb_accessory nicht offen (%1). Ist aoa-gadget.sh on gelaufen?").arg(lastError));
        }
        delete dev;
        m_usbRetry.start();
        return;
    }
    m_usbRetry.stop();
    connect(dev, &UsbAccessoryDevice::disconnected, this, [this]() {
        if (m_session) onSessionClosed(QStringLiteral("USB getrennt"));
    });
    attachTransport(dev, QStringLiteral("USB-Accessory geöffnet, warte auf Head Unit"));
}

QString Controller::phoneBluetoothAddress(QString *why) const
{
    const QByteArray forced = qgetenv("SAILAUTO_BT_ADDRESS");
    if (!forced.isEmpty()) return QString::fromLatin1(forced);
    QStringList reasons;
    const QString cacheFile = m_certDir + QStringLiteral("/bt-address");

    // BlueZ 5: ask the object manager for every adapter (hci0 is not always the name).
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        reasons << QStringLiteral("kein System-D-Bus: ") + bus.lastError().message();
    } else {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                                           QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                           QStringLiteral("GetManagedObjects"));
        QDBusMessage reply = bus.call(call, QDBus::Block, 3000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            reasons << QStringLiteral("BlueZ antwortet nicht (%1)").arg(reply.errorMessage());
        } else {
            QStringList paths;
            const QDBusArgument arg = reply.arguments().value(0).value<QDBusArgument>();
            arg.beginMap();
            while (!arg.atEnd()) {
                QDBusObjectPath path;
                QVariantMap dummy;
                arg.beginMapEntry();
                arg >> path;
                // interfaces -> properties; only the keys matter here
                arg.beginMap();
                bool isAdapter = false;
                while (!arg.atEnd()) {
                    QString iface;
                    QVariantMap props;
                    arg.beginMapEntry();
                    arg >> iface >> props;
                    arg.endMapEntry();
                    if (iface == QLatin1String("org.bluez.Adapter1")) isAdapter = true;
                }
                arg.endMap();
                arg.endMapEntry();
                if (isAdapter) paths << path.path();
            }
            arg.endMap();
            if (paths.isEmpty()) reasons << QStringLiteral("BlueZ hat keinen Adapter (Bluetooth aus?)");
            for (const QString &p : paths) {
                QDBusMessage get = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), p,
                                                                  QStringLiteral("org.freedesktop.DBus.Properties"),
                                                                  QStringLiteral("Get"));
                get << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Address");
                QDBusMessage r = bus.call(get, QDBus::Block, 3000);
                const QString addr = r.type() == QDBusMessage::ReplyMessage
                                         ? r.arguments().value(0).value<QDBusVariant>().variant().toString() : QString();
                if (!addr.isEmpty()) {
                    QFile c(cacheFile);
                    if (c.open(QIODevice::WriteOnly)) c.write(addr.toLatin1());
                    return addr;
                }
                reasons << QStringLiteral("%1: %2").arg(p, r.errorMessage());
            }
        }
    }
    QFile f(QStringLiteral("/sys/class/bluetooth/hci0/address"));
    if (f.open(QIODevice::ReadOnly)) {
        const QString a = QString::fromLatin1(f.readAll()).trimmed().toUpper();
        if (!a.isEmpty()) return a;
    }
    // Bluetooth off: use the address seen last time.
    QFile c(cacheFile);
    if (c.open(QIODevice::ReadOnly)) {
        const QString a = QString::fromLatin1(c.readAll()).trimmed();
        if (!a.isEmpty()) {
            reasons << QStringLiteral("nehme gespeicherte Adresse");
            if (why) *why = reasons.join(QStringLiteral("; "));
            return a;
        }
    }
    if (why) *why = reasons.join(QStringLiteral("; "));
    return QString();
}

void Controller::attachTransport(QIODevice *io, const QString &label)
{
    m_socket = io;
    delete m_trace;
    m_trace = new aap::Trace;
    if (m_trace->open(m_traceBase)) {
        m_lastTraceDir = m_trace->dir();
    } else {
        delete m_trace;
        m_trace = nullptr;
    }
    appendLog(label);
    if (m_trace) appendLog(QStringLiteral("Diagnose-Aufzeichnung: ") + m_lastTraceDir);
    else appendLog(QStringLiteral("Diagnose-Aufzeichnung nicht möglich in ") + m_traceBase);

    m_session = new aap::Session(io, m_certDir + QStringLiteral("/cert.pem"),
                                 m_certDir + QStringLiteral("/key.pem"), this);
    connect(m_session, &aap::Session::log, this, &Controller::appendLog);
    connect(m_session, &aap::Session::stateChanged, this, &Controller::onSessionState);
    connect(m_session, &aap::Session::videoStart, this, &Controller::onVideoStart);
    connect(m_session, &aap::Session::videoStop, this, &Controller::onVideoStop);
    connect(m_session, &aap::Session::videoCanSend, this, &Controller::flush);
    connect(m_session, &aap::Session::touch, &m_screen, &CarScreen::touch);
    connect(m_session, &aap::Session::nightModeChanged, this, [this](bool n) {
        m_screen.setNight(n);
        appendLog(n ? QStringLiteral("Nachtmodus an") : QStringLiteral("Nachtmodus aus"));
    });
    connect(m_session, &aap::Session::closed, this, &Controller::onSessionClosed);
    connect(m_session, &aap::Session::audioFocusChanged, &m_audioTest, &AudioTest::onFocus);
    connect(m_session, &aap::Session::silent, this, [this]() {
        if (auto *usb = qobject_cast<UsbAccessoryDevice *>(m_socket.data())) {
            appendLog(QStringLiteral("USB-Handshake vom Auto (Kernel): ") + usb->handshakeInfo());
            appendLog(QStringLiteral("USB-Modus laut Root-Dienst: \"%1\". Leere Texte = das Auto hat (noch) keinen "
                                     "Accessory-Handshake geschickt (falsche Buchse? Gadget nicht an?).").arg(aoaState()));
            QFile f(QStringLiteral("/run/harbour-sailauto/watch.log"));
            if (f.open(QIODevice::ReadOnly))
                appendLog(QStringLiteral("Root-Wächter: ") + QString::fromUtf8(f.readAll()).trimmed().right(600));
        }
    });
    m_session->setTrace(m_trace);
    QString btWhy;
    const QString bt = phoneBluetoothAddress(&btWhy);
    m_session->setPhoneBluetoothAddress(bt);
    appendLog(QStringLiteral("Bluetooth-Adresse Telefon: %1%2")
                  .arg(bt.isEmpty() ? QStringLiteral("unbekannt") : bt)
                  .arg(btWhy.isEmpty() ? QString() : QStringLiteral(" (") + btWhy + QLatin1Char(')')));
    appendLog(QStringLiteral("Zertifikat: %1").arg(customCert() ? QStringLiteral("eigenes") : QStringLiteral("Entwickler (selbstsigniert)")));

    QString err;
    if (!m_session->start(&err)) {
        appendLog(err);
        onSessionClosed(err);
    }
    emit statusChanged();
}

void Controller::onSessionState(aap::Session::State s)
{
    switch (s) {
    case aap::Session::Versioning: setStatus(QStringLiteral("Verbinde")); break;
    case aap::Session::Handshaking: setStatus(QStringLiteral("TLS-Handshake")); break;
    case aap::Session::Discovering: setStatus(QStringLiteral("Dienste abfragen")); break;
    case aap::Session::OpeningChannels: setStatus(QStringLiteral("Kanäle öffnen")); break;
    case aap::Session::Streaming: setStatus(QStringLiteral("Verbunden")); break;
    default: break;
    }
}

void Controller::onVideoStart(const aap::VideoConfig &cfg)
{
    m_screen.setGeometry(cfg.width, cfg.height, cfg.marginWidth, cfg.marginHeight);
    const int kbps = cfg.height >= 1080 ? 8000 : cfg.height >= 720 ? 4000 : 2000;
    QString err;
    if (!m_encoder.start(cfg.width, cfg.height, cfg.fps, kbps, &err)) {
        appendLog(err);
        disconnectHeadUnit();
        return;
    }
    m_screen.setInfo(m_session ? m_session->headUnitName() : QString(), m_encoder.encoderName());
    m_queue.clear();
    m_clock.start();
    m_statClock.start();
    m_statFrames = 0;
    m_statBytes = 0;
    m_frameTimer.start(1000 / qMax(1, cfg.fps));
    appendLog(QStringLiteral("Video läuft (%1)").arg(m_encoder.encoderName()));
    setStatus(QStringLiteral("Projiziert auf %1").arg(m_session ? m_session->headUnitName() : QStringLiteral("Head Unit")));
}

void Controller::onVideoStop()
{
    m_frameTimer.stop();
    m_encoder.stop();
    m_queue.clear();
    emit statusChanged();
}

void Controller::tick()
{
    if (!m_session || !m_encoder.running()) return;
    flush();
    // Back-pressure: the HU acks every frame (OpenAuto allows 1 in flight).
    // Only render when the pipe is nearly empty so latency stays low.
    if (!m_queue.isEmpty() || m_encoder.pending() >= 3) return;
    m_encoder.push(m_screen.render(), static_cast<quint64>(m_clock.nsecsElapsed() / 1000));
}

void Controller::onEncoded(const QByteArray &data, quint64 ts, bool config)
{
    m_queue.enqueue({data, ts, config});
    flush();
}

void Controller::flush()
{
    while (m_session && !m_queue.isEmpty() && m_session->canSendVideo()) {
        Packet p = m_queue.dequeue();
        if (p.config) m_session->sendVideoConfig(p.data);
        else {
            m_session->sendVideoFrame(p.data, p.ts);
            ++m_statFrames;
        }
        m_statBytes += p.data.size();
    }
    if (m_statClock.isValid() && m_statClock.elapsed() >= 2000) {
        const double s = m_statClock.elapsed() / 1000.0;
        m_fps = m_statFrames / s;
        m_kbps = m_statBytes * 8 / 1000.0 / s;
        m_screen.setStats(m_fps, m_kbps);
        m_statFrames = 0;
        m_statBytes = 0;
        m_statClock.restart();
        emit statsChanged();
    }
}

void Controller::onSessionClosed(const QString &reason)
{
    if (!m_session) return;
    m_audioTest.stop();
    appendLog(QStringLiteral("Sitzung beendet: ") + reason);
    if (m_session->state() != aap::Session::Closed) appendLog(m_session->summary());
    onVideoStop();
    m_session->disconnect(this);
    m_session->deleteLater();
    m_session = nullptr;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->close();
        m_socket->deleteLater();
    }
    m_fps = m_kbps = 0;
    emit statsChanged();
    if (m_trace) {
        m_trace->text(QStringLiteral("--- Ende ---"));
        delete m_trace;
        m_trace = nullptr;
    }
    setStatus(m_server.isListening() ? QStringLiteral("Warte auf Head Unit")
              : m_usbWanted ? QStringLiteral("Warte auf USB") : QStringLiteral("Bereit"));
    if (m_usbWanted) m_usbRetry.start(); // car may come back (ignition, replug)
}

void Controller::appendLog(const QString &line)
{
    const QString l = QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line;
    qInfo("%s", qPrintable(l));
    if (m_trace) m_trace->text(line);
    m_log.append(l);
    while (m_log.size() > 200) m_log.removeFirst();
    emit logChanged();
}

void Controller::setStatus(const QString &s)
{
    m_status = s;
    emit statusChanged();
}
