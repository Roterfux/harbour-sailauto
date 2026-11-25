#include "controller.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QTcpSocket>

Controller::Controller(QObject *parent) : QObject(parent)
{
    bool ok = false;
    const int envPort = qEnvironmentVariableIntValue("SAILAUTO_PORT", &ok);
    if (ok && envPort > 0) m_port = envPort;

    m_certDir = QString::fromLocal8Bit(qgetenv("SAILAUTO_CERT_DIR"));
    if (m_certDir.isEmpty())
        m_certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(m_certDir);

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
    connect(&m_screen, &CarScreen::tapped, this, [this](const QString &w) {
        appendLog(QStringLiteral("Auto-Display: ") + w);
    });
    setStatus(QStringLiteral("Bereit"));
}

Controller::~Controller()
{
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
        m_socket = s;
        appendLog(QStringLiteral("Head Unit verbunden: %1").arg(s->peerAddress().toString()));

        m_session = new aap::Session(s, m_certDir + QStringLiteral("/cert.pem"),
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
        connect(s, &QTcpSocket::disconnected, this, [this]() {
            if (m_session) onSessionClosed(QStringLiteral("Verbindung getrennt"));
        });

        QString err;
        if (!m_session->start(&err)) {
            appendLog(err);
            onSessionClosed(err);
        }
        emit statusChanged();
    }
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
    appendLog(QStringLiteral("Sitzung beendet: ") + reason);
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
    setStatus(m_server.isListening() ? QStringLiteral("Warte auf Head Unit") : QStringLiteral("Bereit"));
}

void Controller::appendLog(const QString &line)
{
    const QString l = QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line;
    qInfo("%s", qPrintable(l));
    m_log.append(l);
    while (m_log.size() > 200) m_log.removeFirst();
    emit logChanged();
}

void Controller::setStatus(const QString &s)
{
    m_status = s;
    emit statusChanged();
}
