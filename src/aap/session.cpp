#include "session.h"
#include "protobuf.h"

#include <QDateTime>
#include <QIODevice>
#include <QtEndian>

namespace aap {

namespace {

quint16 be16(const QByteArray &d, int off)
{
    return static_cast<quint16>((static_cast<quint8>(d[off]) << 8) | static_cast<quint8>(d[off + 1]));
}

QByteArray u16(quint16 v)
{
    QByteArray b(2, 0);
    b[0] = static_cast<char>(v >> 8);
    b[1] = static_cast<char>(v & 0xff);
    return b;
}

QByteArray u64(quint64 v)
{
    QByteArray b(8, 0);
    for (int i = 7; i >= 0; --i, v >>= 8)
        b[i] = static_cast<char>(v & 0xff);
    return b;
}

} // namespace

Session::Session(QIODevice *transport, const QString &certPath, const QString &keyPath, QObject *parent)
    : QObject(parent), m_io(transport), m_certPath(certPath), m_keyPath(keyPath)
{
    qRegisterMetaType<aap::VideoConfig>();
    qRegisterMetaType<aap::Session::State>();
    connect(m_io.data(), &QIODevice::readyRead, this, &Session::onReadyRead);

    // OpenAuto only sends VIDEO_FOCUS_INDICATION when asked. If a head unit
    // never answers, start streaming anyway after a short grace period.
    // OpenAuto speaks first. Say so if it stays silent.
    m_silence.setSingleShot(true);
    m_silence.setInterval(5000);
    connect(&m_silence, &QTimer::timeout, this, [this]() {
        emit log(QStringLiteral("5 s keine Daten von der Head Unit (TCP steht, aber OpenAuto sendet nichts). "
                                "OpenAuto neu starten?"));
    });

    m_focusFallback.setSingleShot(true);
    m_focusFallback.setInterval(1500);
    connect(&m_focusFallback, &QTimer::timeout, this, [this]() {
        if (!m_videoFocused && m_state == Streaming) {
            emit log(QStringLiteral("Kein Video-Fokus erhalten, starte trotzdem"));
            m_videoFocused = true;
            emit videoStart(m_video);
        }
    });
}

bool Session::start(QString *error)
{
    if (!m_crypto.init(m_certPath, m_keyPath, error))
        return false;
    emit log(QStringLiteral("TLS: ") + m_crypto.certificateInfo());
    setState(Versioning);
    emit log(QStringLiteral("Warte auf VERSION_REQUEST der Head Unit"));
    m_silence.start();
    if (m_io->bytesAvailable())
        onReadyRead();
    return true;
}

void Session::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void Session::fail(const QString &reason)
{
    if (m_state == Closed) return;
    m_silence.stop();
    emit log(QStringLiteral("Fehler: ") + reason);
    setState(Closed);
    if (m_videoFocused) emit videoStop();
    m_videoFocused = false;
    if (m_io) m_io->close();
    emit closed(reason);
}

// ---------------------------------------------------------------- framing

void Session::onReadyRead()
{
    if (!m_io || m_state == Closed || m_state == Idle) return;
    const QByteArray chunk = m_io->readAll();
    if (m_rxTotal == 0 && !chunk.isEmpty()) {
        m_silence.stop();
        emit log(QStringLiteral("Erste Daten von der Head Unit: %1 Bytes (%2)")
                     .arg(chunk.size()).arg(QString::fromLatin1(chunk.left(8).toHex(' '))));
    }
    m_rxTotal += chunk.size();
    m_rx.append(chunk);

    for (;;) {
        if (m_rx.size() < 4) return;
        const quint8 channel = static_cast<quint8>(m_rx[0]);
        const quint8 flags = static_cast<quint8>(m_rx[1]);
        const quint8 ftype = flags & FrameTypeMask;
        const int headerLen = (ftype == FrameFirst) ? 8 : 4;
        if (m_rx.size() < headerLen) return;
        const int size = be16(m_rx, 2);
        quint32 total = 0;
        if (ftype == FrameFirst)
            total = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(m_rx.constData() + 4));
        if (m_rx.size() < headerLen + size) return;

        QByteArray payload = m_rx.mid(headerLen, size);
        m_rx.remove(0, headerLen + size);

        if (flags & Encrypted) {
            if (!m_crypto.handshakeDone()) {
                fail(QStringLiteral("verschlüsselter Frame vor TLS-Handshake"));
                return;
            }
            bool ok = false;
            payload = m_crypto.decrypt(payload, &ok);
            if (!ok) {
                fail(QStringLiteral("Entschlüsselung fehlgeschlagen"));
                return;
            }
        }

        switch (ftype) {
        case FrameBulk:
            handleMessage(channel, flags, payload);
            break;
        case FrameFirst: {
            Partial &p = m_partials[channel];
            p.data = payload;
            p.total = total;
            p.flags = flags;
            break;
        }
        case FrameMiddle:
        case FrameLast: {
            auto it = m_partials.find(channel);
            if (it == m_partials.end()) {
                emit log(QStringLiteral("Fortsetzungs-Frame ohne Anfang auf Kanal %1").arg(channel));
                break;
            }
            it->data.append(payload);
            if (ftype == FrameLast) {
                Partial p = *it;
                m_partials.erase(it);
                if (p.total && static_cast<quint32>(p.data.size()) != p.total)
                    emit log(QStringLiteral("Größe passt nicht: %1 statt %2").arg(p.data.size()).arg(p.total));
                handleMessage(channel, p.flags, p.data);
            }
            break;
        }
        }
        if (m_state == Closed) return;
    }
}

void Session::sendRaw(quint8 channel, quint8 flags, const QByteArray &msg)
{
    if (!m_io || !m_io->isOpen()) return;
    const bool enc = flags & Encrypted;

    auto writeFrame = [&](quint8 ftype, const QByteArray &chunk) {
        QByteArray body = enc ? m_crypto.encrypt(chunk) : chunk;
        QByteArray f;
        f.reserve(body.size() + 8);
        f.append(static_cast<char>(channel));
        f.append(static_cast<char>((flags & ~FrameTypeMask) | ftype));
        f.append(u16(static_cast<quint16>(body.size())));
        if (ftype == FrameFirst) {
            quint32 t = qToBigEndian<quint32>(static_cast<quint32>(msg.size()));
            f.append(reinterpret_cast<const char *>(&t), 4);
        }
        f.append(body);
        m_io->write(f);
    };

    // Same split rule as aasdk: messages >= 16 KiB go in FIRST/MIDDLE/LAST frames.
    if (msg.size() < MaxFramePayload) {
        writeFrame(FrameBulk, msg);
        return;
    }
    for (int off = 0; off < msg.size(); off += MaxFramePayload) {
        const int n = qMin(MaxFramePayload, msg.size() - off);
        const quint8 ftype = off == 0 ? FrameFirst : (off + n >= msg.size() ? FrameLast : FrameMiddle);
        writeFrame(ftype, msg.mid(off, n));
    }
}

void Session::send(quint8 channel, quint16 id, const QByteArray &payload, bool encrypted, bool controlType)
{
    quint8 flags = 0;
    if (encrypted) flags |= Encrypted;
    if (controlType) flags |= MsgControl;
    sendRaw(channel, flags, u16(id) + payload);
}

// ---------------------------------------------------------------- dispatch

void Session::handleMessage(quint8 channel, quint8 /*flags*/, const QByteArray &msg)
{
    if (msg.size() < 2) return;
    const quint16 id = be16(msg, 0);
    const QByteArray payload = msg.mid(2);

    if (channel == ChControl) {
        handleControl(id, payload);
        return;
    }
    if (id == ctrl::ChannelOpenResponse) {
        pb::Reader r(payload);
        onChannelOpened(channel, r.number(1, 1) == 0);
        return;
    }
    if (channel == m_videoChannel) handleVideo(id, payload);
    else if (channel == m_inputChannel) handleInput(id, payload);
    else if (channel == m_sensorChannel) handleSensor(id, payload);
    else emit log(QStringLiteral("Nachricht 0x%1 auf unbenutztem Kanal %2").arg(id, 4, 16, QChar('0')).arg(channel));
}

void Session::handleControl(quint16 id, const QByteArray &payload)
{
    switch (id) {
    case ctrl::VersionRequest: {
        const quint16 major = payload.size() >= 2 ? be16(payload, 0) : 1;
        const quint16 minor = payload.size() >= 4 ? be16(payload, 2) : 1;
        emit log(QStringLiteral("Head Unit Protokoll %1.%2").arg(major).arg(minor));
        // major, minor, status (0 = MATCH)
        send(ChControl, ctrl::VersionResponse, u16(major) + u16(minor) + u16(0), false);
        setState(Handshaking);
        break;
    }
    case ctrl::SslHandshake: {
        QByteArray out = m_crypto.handshake(payload);
        if (!out.isEmpty())
            send(ChControl, ctrl::SslHandshake, out, false);
        if (m_crypto.handshakeDone())
            emit log(QStringLiteral("TLS-Handshake fertig"));
        break;
    }
    case ctrl::AuthComplete: {
        if (m_state != Handshaking) {
            emit log(QStringLiteral("Doppeltes AUTH_COMPLETE ignoriert"));
            break;
        }
        pb::Reader r(payload);
        if (r.number(1) != 0 || !m_crypto.handshakeDone()) {
            fail(QStringLiteral("Authentifizierung abgelehnt"));
            return;
        }
        emit log(QStringLiteral("Authentifiziert, frage Dienste ab"));
        setState(Discovering);
        pb::Writer w;
        w.string(4, QStringLiteral("Sailfish OS")).string(5, QStringLiteral("Jolla"));
        send(ChControl, ctrl::ServiceDiscoveryRequest, w.data(), true);
        break;
    }
    case ctrl::ServiceDiscoveryResponse:
        handleServiceDiscovery(payload);
        break;
    case ctrl::PingRequest: {
        pb::Reader r(payload);
        pb::Writer w;
        w.sint(1, r.has(1) ? static_cast<int64_t>(r.number(1))
                           : QDateTime::currentMSecsSinceEpoch() * 1000);
        send(ChControl, ctrl::PingResponse, w.data(), m_crypto.handshakeDone());
        break;
    }
    case ctrl::PingResponse:
        break;
    case ctrl::ShutdownRequest:
        emit log(QStringLiteral("Head Unit beendet die Sitzung"));
        send(ChControl, ctrl::ShutdownResponse, QByteArray(), true);
        fail(QStringLiteral("Head Unit hat beendet"));
        break;
    case ctrl::ShutdownResponse:
        fail(QStringLiteral("Sitzung beendet"));
        break;
    case ctrl::AudioFocusResponse:
    case ctrl::NavFocusResponse:
        break;
    default:
        emit log(QStringLiteral("Unbekannte Steuernachricht 0x%1").arg(id, 4, 16, QChar('0')));
    }
}

void Session::handleServiceDiscovery(const QByteArray &payload)
{
    pb::Reader r(payload);
    m_huName = r.string(2);
    emit log(QStringLiteral("Head Unit: %1 / %2 %3").arg(m_huName, r.string(7), r.string(8)));

    for (const pb::Value &cv : r.all(1)) {
        pb::Reader ch(cv.bytes);
        const int id = static_cast<int>(ch.number(1));
        if (ch.has(3)) {
            pb::Reader av(ch.bytes(3));
            if (av.number(1) == 3 /* VIDEO */ && m_videoChannel < 0) {
                m_videoChannel = id;
                QList<pb::Value> cfgs = av.all(4);
                if (!cfgs.isEmpty()) {
                    pb::Reader vc(cfgs.first().bytes);
                    resolutionSize(static_cast<int>(vc.number(1, 1)), m_video.width, m_video.height);
                    m_video.fps = vc.number(2, 1) == 2 ? 60 : 30;
                    m_video.marginWidth = static_cast<int>(vc.number(3));
                    m_video.marginHeight = static_cast<int>(vc.number(4));
                    m_video.dpi = static_cast<int>(vc.number(5, 140));
                }
            }
        } else if (ch.has(4) && m_inputChannel < 0) {
            m_inputChannel = id;
            pb::Reader in(ch.bytes(4));
            if (in.has(2)) {
                pb::Reader tc(in.bytes(2));
                m_video.touchWidth = static_cast<int>(tc.number(1, 800));
                m_video.touchHeight = static_cast<int>(tc.number(2, 480));
            }
        } else if (ch.has(2) && m_sensorChannel < 0) {
            m_sensorChannel = id;
        }
    }

    emit log(QStringLiteral("Video %1x%2@%3 (Rand %4x%5, %6 dpi), Touch %7x%8")
                 .arg(m_video.width).arg(m_video.height).arg(m_video.fps)
                 .arg(m_video.marginWidth).arg(m_video.marginHeight).arg(m_video.dpi)
                 .arg(m_video.touchWidth).arg(m_video.touchHeight));

    if (m_videoChannel < 0) {
        fail(QStringLiteral("Head Unit bietet keinen Videokanal an"));
        return;
    }

    setState(OpeningChannels);
    for (int ch : {m_videoChannel, m_inputChannel, m_sensorChannel}) {
        if (ch < 0) continue;
        pb::Writer w;
        w.sint(1, 0).sint(2, ch);
        send(static_cast<quint8>(ch), ctrl::ChannelOpenRequest, w.data(), true, true);
        ++m_pendingOpens;
    }
}

void Session::onChannelOpened(quint8 channel, bool ok)
{
    --m_pendingOpens;
    emit log(QStringLiteral("Kanal %1 %2").arg(channel).arg(ok ? QStringLiteral("offen") : QStringLiteral("abgelehnt")));
    if (!ok) {
        if (channel == m_videoChannel) fail(QStringLiteral("Videokanal abgelehnt"));
        return;
    }
    if (channel == m_videoChannel) {
        pb::Writer w;
        w.varint(1, 3); // config_index (Android sends the stream type here)
        send(channel, av::SetupRequest, w.data(), true);
    } else if (channel == m_inputChannel) {
        send(channel, input::BindingRequest, QByteArray(), true); // no hardware keys
    } else if (channel == m_sensorChannel) {
        for (int type : {sensor::DrivingStatus, sensor::NightData}) {
            pb::Writer w;
            w.varint(1, static_cast<uint64_t>(type)).sint(2, 0);
            send(channel, sensor::StartRequest, w.data(), true);
        }
    }
}

void Session::handleVideo(quint16 id, const QByteArray &payload)
{
    pb::Reader r(payload);
    switch (id) {
    case av::SetupResponse: {
        if (r.number(1) != 2 /* OK */) {
            fail(QStringLiteral("Head Unit lehnt Video-Setup ab"));
            return;
        }
        m_maxUnacked = qMax(1, static_cast<int>(r.number(2, 1)));
        const int configIndex = r.has(3) ? static_cast<int>(r.all(3).first().number) : 0;
        emit log(QStringLiteral("Video-Setup ok, max. %1 offene Frames").arg(m_maxUnacked));

        pb::Writer start;
        start.sint(1, m_videoSession).varint(2, static_cast<uint64_t>(configIndex));
        send(static_cast<quint8>(m_videoChannel), av::StartIndication, start.data(), true);

        pb::Writer focus;
        focus.varint(2, 1 /* FOCUSED */).varint(3, 1);
        send(static_cast<quint8>(m_videoChannel), av::VideoFocusRequest, focus.data(), true);
        setState(Streaming);
        m_focusFallback.start();
        break;
    }
    case av::VideoFocusIndication: {
        const bool focused = r.number(1) == 1;
        emit log(QStringLiteral("Video-Fokus: %1").arg(focused ? QStringLiteral("ja") : QStringLiteral("nein")));
        if (focused && !m_videoFocused) {
            m_videoFocused = true;
            m_focusFallback.stop();
            m_unacked = 0;
            emit videoStart(m_video);
        } else if (!focused && m_videoFocused) {
            m_videoFocused = false;
            emit videoStop();
        }
        break;
    }
    case av::MediaAck: {
        const int n = qMax(1, static_cast<int>(r.number(2, 1)));
        m_unacked = qMax(0, m_unacked - n);
        emit videoCanSend();
        break;
    }
    default:
        emit log(QStringLiteral("Video-Nachricht 0x%1").arg(id, 4, 16, QChar('0')));
    }
}

void Session::handleInput(quint16 id, const QByteArray &payload)
{
    if (id == input::BindingResponse) {
        emit log(QStringLiteral("Touch-Eingabe aktiv"));
        return;
    }
    if (id != input::EventIndication) return;
    pb::Reader r(payload);
    if (!r.has(3)) return; // buttons / rotary not handled in the MVP
    pb::Reader te(r.bytes(3));
    const int action = static_cast<int>(te.number(3));
    QList<pb::Value> locs = te.all(1);
    if (locs.isEmpty()) return;
    const int idx = qBound(0, static_cast<int>(te.number(2)), locs.size() - 1);
    pb::Reader loc(locs.at(idx).bytes);
    // Touch coordinates come in touch-screen space; scale to video space.
    const int x = static_cast<int>(loc.number(1)) * m_video.width / qMax(1, m_video.touchWidth);
    const int y = static_cast<int>(loc.number(2)) * m_video.height / qMax(1, m_video.touchHeight);
    emit touch(action, x, y);
}

void Session::handleSensor(quint16 id, const QByteArray &payload)
{
    if (id == sensor::StartResponse) return;
    if (id != sensor::EventIndication) return;
    pb::Reader r(payload);
    for (const pb::Value &v : r.all(10))
        emit nightModeChanged(pb::Reader(v.bytes).boolean(1));
    for (const pb::Value &v : r.all(13))
        emit drivingStatusChanged(static_cast<int>(pb::Reader(v.bytes).number(1)));
}

// ---------------------------------------------------------------- video out

bool Session::canSendVideo() const
{
    return m_state == Streaming && m_videoFocused && m_unacked < m_maxUnacked;
}

void Session::sendVideoConfig(const QByteArray &spsPps)
{
    if (m_state != Streaming) return;
    send(static_cast<quint8>(m_videoChannel), av::Media, spsPps, true);
    ++m_unacked; // OpenAuto acks this one too
}

void Session::sendVideoFrame(const QByteArray &accessUnit, quint64 timestampUs)
{
    if (m_state != Streaming) return;
    send(static_cast<quint8>(m_videoChannel), av::MediaWithTimestamp, u64(timestampUs) + accessUnit, true);
    ++m_unacked;
}

void Session::requestShutdown()
{
    if (m_state == Closed || !m_crypto.handshakeDone()) {
        fail(QStringLiteral("Sitzung beendet"));
        return;
    }
    pb::Writer w;
    w.varint(1, 1 /* QUIT */);
    send(ChControl, ctrl::ShutdownRequest, w.data(), true);
    QTimer::singleShot(1000, this, [this]() { fail(QStringLiteral("Sitzung beendet")); });
}

} // namespace aap
