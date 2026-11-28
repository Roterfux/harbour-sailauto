#include "session.h"
#include "protobuf.h"

#include <QDateTime>
#include <QIODevice>
#include <QtEndian>
#include <algorithm>

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

QList<int> varints(const QList<pb::Value> &vals)
{
    // repeated int32, packed or not
    QList<int> out;
    for (const pb::Value &v : vals) {
        if (v.type == pb::Varint) { out << static_cast<int>(static_cast<qint64>(v.number)); continue; }
        int pos = 0;
        while (pos < v.bytes.size()) {
            quint64 x = 0;
            int shift = 0;
            while (pos < v.bytes.size()) {
                const quint8 b = static_cast<quint8>(v.bytes[pos++]);
                x |= static_cast<quint64>(b & 0x7f) << shift;
                shift += 7;
                if (!(b & 0x80)) break;
            }
            out << static_cast<int>(static_cast<qint64>(x));
        }
    }
    return out;
}

QString keyName(int code)
{
    switch (code) {
    case 3: return QStringLiteral("HOME"); case 4: return QStringLiteral("BACK");
    case 5: return QStringLiteral("CALL"); case 6: return QStringLiteral("ENDCALL");
    case 19: return QStringLiteral("UP"); case 20: return QStringLiteral("DOWN");
    case 21: return QStringLiteral("LEFT"); case 22: return QStringLiteral("RIGHT");
    case 23: return QStringLiteral("ENTER"); case 65536: return QStringLiteral("ROTARY");
    case 84: return QStringLiteral("SEARCH/SPRACHE"); case 85: return QStringLiteral("PLAY_PAUSE");
    case 87: return QStringLiteral("NEXT"); case 88: return QStringLiteral("PREVIOUS");
    case 89: return QStringLiteral("REWIND"); case 90: return QStringLiteral("FAST_FORWARD");
    case 126: return QStringLiteral("PLAY"); case 127: return QStringLiteral("PAUSE");
    case 79: return QStringLiteral("HEADSETHOOK"); case 231: return QStringLiteral("VOICE_ASSIST");
    case 65537: return QStringLiteral("MEDIA"); case 65538: return QStringLiteral("NAVIGATION");
    case 65539: return QStringLiteral("RADIO"); case 65540: return QStringLiteral("TEL");
    }
    return QString::number(code);
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
    // Watchdog: report when the setup hangs in one state (key info when
    // testing against an unknown car).
    m_watchdog.setInterval(10000);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        if (m_state == Streaming) {
            // The car pings every few seconds. Silence = car switched off or
            // USB went stale without an error: give up, so we reconnect.
            if (m_rxTotal == m_rxAtCheck && ++m_silentChecks >= 2) {
                emit log(QStringLiteral("Auto sendet seit 20 s nichts mehr (ausgeschaltet?) – Verbindung wird neu aufgebaut"));
                emit carSilent();
                fail(QStringLiteral("Auto antwortet nicht mehr"));
                return;
            }
            if (m_rxTotal != m_rxAtCheck) m_silentChecks = 0;
            m_rxAtCheck = m_rxTotal;
            return;
        }
        if (m_state == Closed || m_state == Idle) return;
        static const char *names[] = {"Idle", "Versioning", "Handshaking", "Discovering", "OpeningChannels", "Streaming", "Closed"};
        emit log(QStringLiteral("HÄNGT: seit 10 s im Zustand %1 (offene Kanalanfragen: %2)")
                     .arg(QLatin1String(names[m_state])).arg(m_pendingOpens));
    });

    // OpenAuto speaks first. Say so if it stays silent.
    m_silence.setSingleShot(true);
    m_silence.setInterval(5000);
    connect(&m_silence, &QTimer::timeout, this, [this]() {
        emit log(QStringLiteral("5 s keine Daten von der Head Unit (Verbindung steht, aber sie sendet nichts)."));
        emit silent();
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
    m_age.start();
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
    m_watchdog.start();
    emit stateChanged(s);
}

void Session::fail(const QString &reason)
{
    if (m_state == Closed) return;
    m_silence.stop();
    m_watchdog.stop();
    emit log(QStringLiteral("Fehler: ") + reason);
    emit log(summary());
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
                     .arg(chunk.size()).arg(QString::fromLatin1(chunk.left(8).toHex())));
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
    if (m_trace) m_trace->message(true, channel, flags, id, payload);
    m_txTotal += payload.size() + 2;
    sendRaw(channel, flags, u16(id) + payload);
}

// ---------------------------------------------------------------- dispatch

void Session::handleMessage(quint8 channel, quint8 flags, const QByteArray &msg)
{
    if (msg.size() < 2) {
        emit log(QStringLiteral("Zu kurze Nachricht auf Kanal %1 (%2 B)").arg(channel).arg(msg.size()));
        return;
    }
    const quint16 id = be16(msg, 0);
    const QByteArray payload = msg.mid(2);
    if (m_trace) m_trace->message(false, channel, flags, id, payload);

    if (channel == ChControl) {
        handleControl(id, payload);
        return;
    }
    if (id == ctrl::ChannelOpenResponse) {
        pb::Reader r(payload);
        onChannelOpened(channel, r.number(1, 1) == 0);
        return;
    }
    switch (m_kinds.value(channel, Trace::Other)) {
    case Trace::Video: handleVideo(id, payload); break;
    case Trace::Input: handleInput(id, payload); break;
    case Trace::Sensor: handleSensor(id, payload); break;
    case Trace::Audio: handleAudio(channel, id, payload); break;
    case Trace::Bluetooth: handleBluetooth(id, payload); break;
    case Trace::Mic:
    case Trace::Playback:
    case Trace::Navigation:
    case Trace::Phone:
        // Opened so the car sees a complete phone; content not used yet.
        emit log(QStringLiteral("Info vom Auto: %1 %2")
                     .arg(m_trace ? m_trace->messageName(channel, id) : QString::number(id, 16),
                          Trace::dump(payload)));
        break;
    default:
        emit log(QStringLiteral("Nachricht 0x%1 auf unbekanntem Kanal %2: %3")
                     .arg(id, 4, 16, QChar('0')).arg(channel).arg(Trace::dump(payload)));
    }
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
        if (!m_crypto.handshakeError().isEmpty())
            emit log(QStringLiteral("TLS-FEHLER: ") + m_crypto.handshakeError());
        if (!out.isEmpty())
            send(ChControl, ctrl::SslHandshake, out, false);
        if (m_crypto.handshakeDone())
            emit log(QStringLiteral("TLS-Handshake fertig: ") + m_crypto.sessionInfo());
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
    case ctrl::AudioFocusResponse: { // AUDIO_FOCUS_NOTIFICATION
        pb::Reader r(payload);
        const int st = static_cast<int>(r.number(1));
        static const char *names[] = {"ungültig", "GAIN", "GAIN_TRANSIENT", "LOSS", "LOSS_TRANSIENT_CAN_DUCK",
                                      "LOSS_TRANSIENT", "GAIN_MEDIA_ONLY", "GAIN_TRANSIENT_GUIDANCE_ONLY"};
        emit log(QStringLiteral("Audio-Fokus vom Auto: %1%2").arg(QLatin1String(st >= 0 && st <= 7 ? names[st] : "?"))
                     .arg(r.boolean(2) ? QStringLiteral(" (unaufgefordert)") : QString()));
        emit audioFocusChanged(st);
        emit audioFocusNotified(st, r.boolean(2));
        break;
    }
    case ctrl::NavFocusResponse:   // NAV_FOCUS_NOTIFICATION
    case 9:  // CHANNEL_CLOSE_NOTIFICATION
    case 17: // VOICE_SESSION_NOTIFICATION
    case 20: case 21: case 22: case 23: case 24: case 25: case 26:
        emit log(QStringLiteral("Auto meldet: %1 %2")
                     .arg(m_trace ? m_trace->messageName(0, id) : QString::number(id), Trace::dump(payload)));
        break;
    case 255:
        emit log(QStringLiteral("AUTO MELDET FEHLER: unerwartete Nachricht (UNEXPECTED_MESSAGE) %1").arg(Trace::dump(payload)));
        break;
    case 0xffff:
        emit log(QStringLiteral("AUTO MELDET FEHLER: Framing-Fehler %1").arg(Trace::dump(payload)));
        break;
    default:
        emit log(QStringLiteral("Unbekannte Steuernachricht 0x%1: %2").arg(id, 4, 16, QChar('0')).arg(Trace::dump(payload)));
    }
}

void Session::handleServiceDiscovery(const QByteArray &payload)
{
    pb::Reader r(payload);
    // Old head units fill the flat fields, newer ones (aasdk >= 1.6) put the
    // same data into headunit_info (17) and display_name (14).
    QString make = r.string(2), huMake = r.string(7), huModel = r.string(8);
    if (r.has(17)) {
        pb::Reader info(r.bytes(17));
        if (make.isEmpty()) make = info.string(1);
        if (huMake.isEmpty()) huMake = info.string(5);
        if (huModel.isEmpty()) huModel = info.string(6);
    }
    m_huName = !r.string(14).isEmpty() ? r.string(14) : (!huModel.isEmpty() ? huModel : make);
    emit log(QStringLiteral("Head Unit: %1 / %2 %3").arg(make, huMake, huModel));

    // OpenAuto/Crankshaft (make "f1x") announce the screen size as touch area
    // but send coordinates already scaled to the video size.
    m_touchInVideoSpace = huMake.compare(QLatin1String("f1x"), Qt::CaseInsensitive) == 0;
    const QByteArray forced = qgetenv("SAILAUTO_TOUCH_SCALE");
    if (forced == "0") m_touchInVideoSpace = true;
    else if (forced == "1") m_touchInVideoSpace = false;
    if (m_touchInVideoSpace) emit log(QStringLiteral("Touch-Koordinaten kommen bereits in Videogröße (OpenAuto)"));

    if (m_trace) m_trace->discovery(payload);
    static const char *kindNames[] = {"Steuerung", "Video", "Audio", "Touch/Tasten", "Sensoren", "Mikrofon",
                                      "Bluetooth", "Musikstatus", "Navigationsstatus", "Telefonstatus", "anderes"};
    for (const pb::Value &cv : r.all(1)) {
        pb::Reader ch(cv.bytes);
        const int id = static_cast<int>(ch.number(1));
        Trace::Kind kind = Trace::Other;
        QString detail;
        if (ch.has(3)) {
            pb::Reader av(ch.bytes(3));
            // old: stream_type VIDEO=3 / new: codec H264_BP=3; video configs in field 4
            if (av.number(1) == 3 || av.has(4)) {
                kind = Trace::Video;
                if (m_videoChannel < 0) {
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
                    detail = QStringLiteral("%1 Konfiguration(en)").arg(cfgs.size());
                } else {
                    detail = QStringLiteral("zweites Display, nicht benutzt");
                    kind = Trace::Other;
                }
            } else {
                kind = Trace::Audio;
                const int type = static_cast<int>(av.number(2));
                m_audioType[id] = type;
                static const char *types[] = {"?", "Ansagen", "System", "Medien", "Telefon"};
                detail = QLatin1String(type >= 0 && type <= 4 ? types[type] : "?");
                bool first = true;
                for (const pb::Value &ac : av.all(3)) {
                    pb::Reader a(ac.bytes);
                    detail += QStringLiteral(" %1Hz/%2bit/%3ch").arg(a.number(1)).arg(a.number(2)).arg(a.number(3));
                    if (first) {
                        m_audio[id].rate = static_cast<int>(a.number(1, 48000));
                        m_audio[id].channels = static_cast<int>(a.number(3, 2));
                        first = false;
                    }
                }
            }
        } else if (ch.has(4)) {
            kind = Trace::Input;
            if (m_inputChannel < 0) {
                m_inputChannel = id;
                pb::Reader in(ch.bytes(4));
                if (in.has(2)) {
                    pb::Reader tc(in.bytes(2));
                    m_video.touchWidth = static_cast<int>(tc.number(1, 800));
                    m_video.touchHeight = static_cast<int>(tc.number(2, 480));
                }
                m_keycodes = varints(in.all(1));
                QStringList names;
                for (int k : m_keycodes) names << keyName(k);
                detail = QStringLiteral("Tasten: %1").arg(names.isEmpty() ? QStringLiteral("keine") : names.join(QLatin1Char(' ')));
            }
        } else if (ch.has(2)) {
            kind = Trace::Sensor;
            if (m_sensorChannel < 0) m_sensorChannel = id;
            pb::Reader se(ch.bytes(2));
            QStringList types;
            for (const pb::Value &sv : se.all(1)) {
                const int t = static_cast<int>(pb::Reader(sv.bytes).number(1));
                types << QString::number(t);
                m_sensorTypes << t;
            }
            detail = QStringLiteral("Typen ") + types.join(QLatin1Char(','));
        } else if (ch.has(5)) {
            kind = Trace::Mic;
        } else if (ch.has(6)) {
            kind = Trace::Bluetooth;
            pb::Reader bt(ch.bytes(6));
            m_carBt = bt.string(1);
            m_btMethods.clear();
            for (const pb::Value &mv : bt.all(2)) {
                if (mv.type == pb::Varint) m_btMethods << static_cast<int>(static_cast<qint64>(mv.number));
                else { // packed
                    int pos = 0;
                    while (pos < mv.bytes.size()) {
                        quint64 v = 0; int shift = 0;
                        while (pos < mv.bytes.size()) {
                            const quint8 b = static_cast<quint8>(mv.bytes[pos++]);
                            v |= static_cast<quint64>(b & 0x7f) << shift; shift += 7;
                            if (!(b & 0x80)) break;
                        }
                        m_btMethods << static_cast<int>(static_cast<qint64>(v));
                    }
                }
            }
            detail = QStringLiteral("Auto %1, Methoden %2").arg(m_carBt).arg(m_btMethods.size());
        } else if (ch.has(8)) {
            kind = Trace::Navigation;
        } else if (ch.has(9)) {
            kind = Trace::Playback;
        } else if (ch.has(10)) {
            kind = Trace::Phone;
        } else if (ch.has(12)) {
            // VendorExtensionService { name = 1, package_white_list = 2 (repeated), data = 3 }:
            // the car maker's own channel (VW: undocumented). Log what it says.
            const pb::Reader ve(ch.bytes(12));
            QStringList pk;
            for (const pb::Value &v : ve.all(2)) pk << QString::fromUtf8(v.bytes);
            const QByteArray data = ve.bytes(3);
            detail = QStringLiteral("Hersteller-Erweiterung \"%1\"%2%3").arg(ve.string(1))
                         .arg(pk.isEmpty() ? QString() : QStringLiteral(", Apps: ") + pk.join(QStringLiteral(", ")))
                         .arg(data.isEmpty() ? QString() : QStringLiteral(", Daten %1 Bytes: %2").arg(data.size())
                                                                   .arg(QString::fromLatin1(data.left(48).toHex())));
        } else {
            int f = 0;
            for (int k = 2; k <= 20; ++k) if (ch.has(k)) { f = k; break; }
            detail = QStringLiteral("unbekannter Dienst (Feld %1)").arg(f);
        }
        m_kinds[id] = kind;
        if (m_trace) m_trace->setKind(id, kind);
        emit log(QStringLiteral("Dienst %1: %2 %3").arg(id).arg(QLatin1String(kindNames[kind])).arg(detail));
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
    QList<int> toOpen;
    for (auto it = m_kinds.constBegin(); it != m_kinds.constEnd(); ++it) {
        switch (it.value()) {
        case Trace::Video:
            if (it.key() == m_videoChannel) toOpen << it.key();
            break;
        case Trace::Input:
            if (it.key() == m_inputChannel) toOpen << it.key();
            break;
        case Trace::Sensor:
            if (it.key() == m_sensorChannel) toOpen << it.key();
            break;
        case Trace::Audio: case Trace::Mic: case Trace::Bluetooth:
        case Trace::Playback: case Trace::Navigation: case Trace::Phone:
            toOpen << it.key();
            break;
        default:
            break;
        }
    }
    std::sort(toOpen.begin(), toOpen.end());
    for (int ch : toOpen) {
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
    if (m_pendingOpens == 0)
        emit log(QStringLiteral("Alle Kanäle beantwortet"));
    const Trace::Kind kind = m_kinds.value(channel, Trace::Other);
    if (kind == Trace::Audio) {
        pb::Writer w;
        w.varint(1, 1); // MEDIA_CODEC_AUDIO_PCM
        send(channel, av::SetupRequest, w.data(), true);
        return;
    }
    if (kind == Trace::Bluetooth) {
        if (m_phoneBt.isEmpty()) {
            emit log(QStringLiteral("Bluetooth: Telefonadresse unbekannt, keine Kopplungsanfrage"));
            return;
        }
        // Prefer numeric comparison (2), else whatever the car lists first.
        int method = m_btMethods.contains(2) ? 2 : (m_btMethods.isEmpty() ? 2 : m_btMethods.first());
        pb::Writer w;
        w.string(1, m_phoneBt).sint(2, method);
        send(channel, 0x8001 /* PAIRING_REQUEST */, w.data(), true);
        emit log(QStringLiteral("Bluetooth: Kopplungsanfrage %1 -> Auto %2 (Methode %3)").arg(m_phoneBt, m_carBt).arg(method));
        return;
    }
    if (channel == m_videoChannel) {
        pb::Writer w;
        w.varint(1, 3); // MEDIA_CODEC_VIDEO_H264_BP
        send(channel, av::SetupRequest, w.data(), true);
    } else if (channel == m_inputChannel) {
        // Ask for exactly the keys the car offers (asking for unsupported
        // ones makes some head units reject the whole binding).
        pb::Writer w;
        for (int k : m_keycodes) w.sint(1, k);
        send(channel, input::BindingRequest, w.data(), true);
    } else if (channel == m_sensorChannel) {
        QList<int> wanted{sensor::DrivingStatus, sensor::NightData};
        // Car GPS for "where is my car" (only if offered: asking for more can upset some cars)
        if (m_sensorTypes.contains(sensor::Location)) wanted << sensor::Location;
        // Gear P = parked (some cars report driving restrictions even when standing)
        if (m_sensorTypes.contains(sensor::Gear)) wanted << sensor::Gear;
        // Wheel speed: keeps the car moving on the map when GPS is gone (tunnel)
        if (m_sensorTypes.contains(sensor::Speed)) wanted << sensor::Speed;
        // Gyroscope: turn rate (map turns at once, tunnels without a route)
        if (m_sensorTypes.contains(sensor::Gyroscope)) wanted << sensor::Gyroscope;
        // GPS satellites: how good the car's fix is
        if (m_sensorTypes.contains(sensor::GpsSatellites)) wanted << sensor::GpsSatellites;
        for (int type : wanted) {
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
        const bool ok = pb::Reader(payload).number(1) == 0;
        emit log(ok ? QStringLiteral("Touch-Eingabe aktiv, %1 Tasten gebunden").arg(m_keycodes.size())
                    : QStringLiteral("Tastenbindung ABGELEHNT"));
        return;
    }
    if (id != input::EventIndication) return;
    pb::Reader r(payload);
    if (r.has(4)) { // ButtonEvents { repeated ButtonEvent {scan_code, is_pressed, meta, long_press} }
        for (const pb::Value &bv : pb::Reader(r.bytes(4)).all(1)) {
            pb::Reader b(bv.bytes);
            const int code = static_cast<int>(b.number(1));
            const bool pressed = b.boolean(2);
            const bool longPress = b.boolean(4);
            // A held key repeats "pressed (long)": log only the first one.
            const bool repeat = pressed && longPress && m_heldLongKey == code;
            m_heldLongKey = pressed && longPress ? code : (pressed ? m_heldLongKey : -1);
            if (!repeat)
                emit log(QStringLiteral("Taste %1 %2%3").arg(keyName(code))
                             .arg(pressed ? QStringLiteral("gedrückt") : QStringLiteral("los"))
                             .arg(longPress ? QStringLiteral(" (lang)") : QString()));
            emit key(code, pressed, longPress);
        }
    }
    if (r.has(6)) { // RelativeInputEvents (rotary knob)
        for (const pb::Value &rv : pb::Reader(r.bytes(6)).all(1)) {
            pb::Reader e(rv.bytes);
            emit log(QStringLiteral("Drehregler %1: %2").arg(keyName(static_cast<int>(e.number(1))))
                         .arg(static_cast<qint64>(static_cast<qint32>(e.number(2)))));
        }
    }
    if (!r.has(3)) return;
    pb::Reader te(r.bytes(3));
    const int action = static_cast<int>(te.number(3));
    QList<pb::Value> locs = te.all(1);
    if (locs.isEmpty()) return;
    const int idx = qBound(0, static_cast<int>(te.number(2)), locs.size() - 1);
    pb::Reader loc(locs.at(idx).bytes);
    // Touch coordinates come in touch-screen space; scale to video space.
    int x = static_cast<int>(loc.number(1));
    int y = static_cast<int>(loc.number(2));
    if (!m_touchInVideoSpace) {
        x = x * m_video.width / qMax(1, m_video.touchWidth);
        y = y * m_video.height / qMax(1, m_video.touchHeight);
    }
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
    for (const pb::Value &v : r.all(8))
        emit gearChanged(static_cast<int>(pb::Reader(v.bytes).number(1)));
    // GyroscopeData { rotation_speed_x_e3 = 1, y_e3 = 2, z_e3 = 3 } (rad/s * 1000, car frame, z up)
    for (const pb::Value &v : r.all(20)) {
        pb::Reader g(v.bytes);
        auto val = [&](int f) { return g.has(f) ? static_cast<int32_t>(static_cast<int64_t>(g.number(f))) / 1e3 : 0.0; };
        emit gyroChanged(val(1), val(2), val(3));
    }
    // GpsSatelliteData { number_in_use = 1, number_in_view = 2, satellites = 3 }
    for (const pb::Value &v : r.all(21)) {
        pb::Reader g(v.bytes);
        emit satellitesChanged(int(g.number(1)), g.has(2) ? int(g.number(2)) : -1);
    }
    // SpeedData { speed_e3 = 1 } (m/s * 1000)
    for (const pb::Value &v : r.all(3)) {
        pb::Reader sp(v.bytes);
        if (sp.has(1)) emit speedChanged(static_cast<int32_t>(static_cast<int64_t>(sp.number(1))) / 1e3);
    }
    // LocationData { timestamp=1, latitude_e7=2, longitude_e7=3, accuracy_e3=4, ... } (int32 as varint)
    for (const pb::Value &v : r.all(1)) {
        pb::Reader l(v.bytes);
        if (!l.has(2) || !l.has(3)) continue;
        const double lat = static_cast<int32_t>(static_cast<int64_t>(l.number(2))) / 1e7;
        const double lon = static_cast<int32_t>(static_cast<int64_t>(l.number(3))) / 1e7;
        const double acc = l.has(4) ? static_cast<int32_t>(static_cast<int64_t>(l.number(4))) / 1e3 : -1;
        if (lat == 0 && lon == 0) continue;
        // speed_e3 = 6 (m/s * 1e3), bearing_e6 = 7 (degrees * 1e6)
        const double speed = l.has(6) ? static_cast<int32_t>(static_cast<int64_t>(l.number(6))) / 1e3 : -1;
        const double bearing = l.has(7) ? static_cast<int32_t>(static_cast<int64_t>(l.number(7))) / 1e6 : -1;
        emit locationChanged(lat, lon, acc, bearing, speed);
    }
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
    ++m_framesSent;
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

void Session::handleAudio(quint8 channel, quint16 id, const QByteArray &payload)
{
    pb::Reader r(payload);
    if (id == av::SetupResponse) { // CONFIG
        const bool ok = r.number(1) == 2;
        AudioState &a = m_audio[channel];
        a.setupOk = ok;
        a.maxUnacked = qMax(1, static_cast<int>(r.number(2, 1)));
        if (ok) QTimer::singleShot(0, this, [this, channel]() { emit audioReady(m_audioType.value(channel)); });
        emit log(QStringLiteral("Audio-Kanal %1 (Typ %2): Setup %3, max. %4 offen")
                     .arg(channel).arg(m_audioType.value(channel)).arg(ok ? QStringLiteral("ok") : QStringLiteral("ABGELEHNT"))
                     .arg(r.number(2)));
        return;
    }
    if (id == av::MediaAck) {
        AudioState &a = m_audio[channel];
        a.unacked = qMax(0, a.unacked - qMax(1, static_cast<int>(r.number(2, 1))));
        return;
    }
    emit log(QStringLiteral("Audio-Kanal %1: Nachricht 0x%2 %3").arg(channel).arg(id, 4, 16, QChar('0')).arg(Trace::dump(payload)));
}

void Session::handleBluetooth(quint16 id, const QByteArray &payload)
{
    pb::Reader r(payload);
    if (id == 0x8002) { // PAIRING_RESPONSE
        emit log(QStringLiteral("Bluetooth: Antwort Status %1, schon gekoppelt: %2")
                     .arg(static_cast<qint64>(r.number(1))).arg(r.boolean(2) ? QStringLiteral("ja") : QStringLiteral("nein")));
        return;
    }
    if (id == 0x8003) {
        emit log(QStringLiteral("Bluetooth: Auto schickt Kopplungsdaten %1 (bitte am Telefon bestätigen, falls gefragt)")
                     .arg(Trace::dump(payload)));
        return;
    }
    emit log(QStringLiteral("Bluetooth: Nachricht 0x%1 %2").arg(id, 4, 16, QChar('0')).arg(Trace::dump(payload)));
}

int Session::audioChannel(int streamType) const
{
    for (auto it = m_audioType.constBegin(); it != m_audioType.constEnd(); ++it)
        if (it.value() == streamType && m_audio.value(it.key()).setupOk) return it.key();
    return -1;
}

void Session::requestAudioFocus(int request)
{
    if (m_state != Streaming) return;
    pb::Writer w;
    w.varint(1, static_cast<uint64_t>(request));
    send(ChControl, ctrl::AudioFocusRequest, w.data(), true);
    static const char *names[] = {"?", "GAIN", "GAIN_TRANSIENT", "GAIN_TRANSIENT_MAY_DUCK", "RELEASE"};
    emit log(QStringLiteral("Audio-Fokus angefragt: %1").arg(QLatin1String(request >= 1 && request <= 4 ? names[request] : "?")));
}

void Session::audioFormat(int channel, int *rate, int *channels) const
{
    const AudioState a = m_audio.value(channel);
    if (rate) *rate = a.rate;
    if (channels) *channels = a.channels;
}

bool Session::startAudio(int channel)
{
    if (m_state != Streaming || !m_audio.value(channel).setupOk) return false;
    AudioState &a = m_audio[channel];
    a.session += 1;
    a.unacked = 0;
    a.started = true;
    pb::Writer w;
    w.sint(1, a.session).varint(2, 0); // session, config index
    send(static_cast<quint8>(channel), av::StartIndication, w.data(), true);
    emit log(QStringLiteral("Audio-Kanal %1: START (Sitzung %2)").arg(channel).arg(a.session));
    return true;
}

void Session::stopAudio(int channel)
{
    if (!m_audio.value(channel).started) return;
    AudioState &a = m_audio[channel];
    a.started = false;
    pb::Writer w;
    w.sint(1, a.session);
    send(static_cast<quint8>(channel), av::StopIndication, w.data(), true);
    emit log(QStringLiteral("Audio-Kanal %1: STOP").arg(channel));
}

bool Session::canSendAudio(int channel) const
{
    const AudioState a = m_audio.value(channel);
    return m_state == Streaming && a.started && a.unacked < a.maxUnacked;
}

void Session::sendAudio(int channel, const QByteArray &pcm, quint64 timestampUs)
{
    if (!canSendAudio(channel)) return;
    send(static_cast<quint8>(channel), av::MediaWithTimestamp, u64(timestampUs) + pcm, true);
    m_audio[channel].unacked += 1;
}

QString Session::summary() const
{
    static const char *names[] = {"Idle", "Versioning", "Handshaking", "Discovering", "OpeningChannels", "Streaming", "Closed"};
    return QStringLiteral("Zusammenfassung: Dauer %1 s, Zustand %2, empfangen %3 B, gesendet %4 B, Videoframes %5, "
                          "offene Kanalanfragen %6")
        .arg(m_age.isValid() ? m_age.elapsed() / 1000.0 : 0.0, 0, 'f', 1)
        .arg(QLatin1String(names[m_state])).arg(m_rxTotal).arg(m_txTotal).arg(m_framesSent).arg(m_pendingOpens);
}

} // namespace aap
