#include "trace.h"

#include <QDateTime>
#include <QDir>
#include <QtEndian>

namespace aap {

namespace {

bool readVarint(const QByteArray &d, int &pos, quint64 &out)
{
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= d.size()) return false;
        const quint8 b = static_cast<quint8>(d[pos++]);
        out |= static_cast<quint64>(b & 0x7f) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// True if the bytes look like a well-formed protobuf message.
bool looksLikeMessage(const QByteArray &d)
{
    if (d.isEmpty()) return false;
    int pos = 0;
    while (pos < d.size()) {
        quint64 key;
        if (!readVarint(d, pos, key) || (key >> 3) == 0) return false;
        quint64 v;
        switch (key & 7) {
        case 0: if (!readVarint(d, pos, v)) return false; break;
        case 1: pos += 8; break;
        case 5: pos += 4; break;
        case 2: if (!readVarint(d, pos, v) || pos + static_cast<qint64>(v) > d.size()) return false;
                pos += static_cast<int>(v); break;
        default: return false;
        }
        if (pos > d.size()) return false;
    }
    return true;
}

bool printable(const QByteArray &d)
{
    if (d.isEmpty()) return true;
    for (char c : d)
        if ((static_cast<quint8>(c) < 0x20 && c != '\n' && c != '\t') || c == 0x7f) return false;
    return true;
}

const char *controlName(quint16 id)
{
    switch (id) {
    case 1: return "VERSION_REQUEST"; case 2: return "VERSION_RESPONSE";
    case 3: return "SSL_HANDSHAKE"; case 4: return "AUTH_COMPLETE";
    case 5: return "SERVICE_DISCOVERY_REQUEST"; case 6: return "SERVICE_DISCOVERY_RESPONSE";
    case 7: return "CHANNEL_OPEN_REQUEST"; case 8: return "CHANNEL_OPEN_RESPONSE";
    case 9: return "CHANNEL_CLOSE_NOTIFICATION";
    case 11: return "PING_REQUEST"; case 12: return "PING_RESPONSE";
    case 13: return "NAV_FOCUS_REQUEST"; case 14: return "NAV_FOCUS_NOTIFICATION";
    case 15: return "BYEBYE_REQUEST"; case 16: return "BYEBYE_RESPONSE";
    case 17: return "VOICE_SESSION_NOTIFICATION";
    case 18: return "AUDIO_FOCUS_REQUEST"; case 19: return "AUDIO_FOCUS_NOTIFICATION";
    case 20: return "CAR_CONNECTED_DEVICES_REQUEST"; case 21: return "CAR_CONNECTED_DEVICES_RESPONSE";
    case 22: return "USER_SWITCH_REQUEST"; case 23: return "BATTERY_STATUS_NOTIFICATION";
    case 24: return "CALL_AVAILABILITY_STATUS"; case 25: return "USER_SWITCH_RESPONSE";
    case 26: return "SERVICE_DISCOVERY_UPDATE";
    case 255: return "UNEXPECTED_MESSAGE"; case 65535: return "FRAMING_ERROR";
    }
    return nullptr;
}

const char *mediaName(quint16 id)
{
    switch (id) {
    case 0x0000: return "MEDIA_DATA"; case 0x0001: return "CODEC_CONFIG";
    case 0x8000: return "SETUP"; case 0x8001: return "START"; case 0x8002: return "STOP";
    case 0x8003: return "CONFIG"; case 0x8004: return "ACK";
    case 0x8005: return "MICROPHONE_REQUEST"; case 0x8006: return "MICROPHONE_RESPONSE";
    case 0x8007: return "VIDEO_FOCUS_REQUEST"; case 0x8008: return "VIDEO_FOCUS_NOTIFICATION";
    case 0x8009: return "UPDATE_UI_CONFIG_REQUEST"; case 0x800a: return "UPDATE_UI_CONFIG_REPLY";
    case 0x800b: return "AUDIO_UNDERFLOW";
    }
    return nullptr;
}

const char *kindName(Trace::Kind k)
{
    switch (k) {
    case Trace::Control: return "control"; case Trace::Video: return "video";
    case Trace::Audio: return "audio"; case Trace::Input: return "input";
    case Trace::Sensor: return "sensor"; case Trace::Mic: return "mic";
    case Trace::Bluetooth: return "bluetooth"; case Trace::Playback: return "playback";
    case Trace::Navigation: return "navstatus"; case Trace::Phone: return "phonestatus";
    case Trace::Other: break;
    }
    return "?";
}

} // namespace

bool Trace::open(const QString &baseDir)
{
    m_dir = baseDir + QLatin1Char('/') + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    if (!QDir().mkpath(m_dir)) return false;
    m_log.setFileName(m_dir + QStringLiteral("/log.txt"));
    m_bin.setFileName(m_dir + QStringLiteral("/trace.bin"));
    if (!m_log.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    m_bin.open(QIODevice::WriteOnly);
    m_bin.write("AAPTRACE1\n");
    m_clock.start();
    return true;
}

void Trace::text(const QString &line)
{
    if (!m_log.isOpen()) return;
    m_log.write(QStringLiteral("%1 %2\n").arg(m_clock.elapsed() / 1000.0, 9, 'f', 3).arg(line).toUtf8());
    m_log.flush();
}

QString Trace::messageName(quint8 channel, quint16 id) const
{
    const Kind k = kind(channel);
    const char *n = nullptr;
    if (k == Control) n = controlName(id);
    else if (id == 7) n = "CHANNEL_OPEN_REQUEST";
    else if (id == 8) n = "CHANNEL_OPEN_RESPONSE";
    else if (k == Video || k == Audio || k == Mic) n = mediaName(id);
    else if (k == Input) n = id == 0x8001 ? "INPUT_REPORT" : id == 0x8002 ? "KEY_BINDING_REQUEST" : id == 0x8003 ? "KEY_BINDING_RESPONSE" : nullptr;
    else if (k == Sensor) n = id == 0x8001 ? "SENSOR_REQUEST" : id == 0x8002 ? "SENSOR_RESPONSE" : id == 0x8003 ? "SENSOR_BATCH" : nullptr;
    else if (k == Bluetooth) n = id == 0x8001 ? "PAIRING_REQUEST" : id == 0x8002 ? "PAIRING_RESPONSE" : id == 0x8003 ? "AUTH_DATA" : id == 0x8004 ? "AUTH_RESULT" : nullptr;
    else if (k == Playback) n = id == 0x8001 ? "PLAYBACK_STATUS" : id == 0x8002 ? "PLAYBACK_INPUT" : id == 0x8003 ? "PLAYBACK_METADATA" : nullptr;
    else if (k == Navigation) n = id == 0x8001 ? "CLUSTER_START" : id == 0x8002 ? "CLUSTER_STOP" : id == 0x8003 ? "NAV_STATUS" : nullptr;
    const QString base = QStringLiteral("%1/%2").arg(QLatin1String(kindName(k))).arg(channel);
    return n ? QStringLiteral("%1 %2").arg(base, QLatin1String(n))
             : QStringLiteral("%1 0x%2").arg(base).arg(id, 4, 16, QLatin1Char('0'));
}

QString Trace::dump(const QByteArray &d, int depth)
{
    if (depth > 6) return QStringLiteral("…");
    QStringList parts;
    int pos = 0;
    while (pos < d.size()) {
        quint64 key;
        if (!readVarint(d, pos, key)) break;
        const int field = static_cast<int>(key >> 3);
        quint64 v = 0;
        switch (key & 7) {
        case 0:
            readVarint(d, pos, v);
            parts << QStringLiteral("%1:%2").arg(field).arg(v);
            break;
        case 1:
            parts << QStringLiteral("%1:fix64").arg(field);
            pos += 8;
            break;
        case 5:
            parts << QStringLiteral("%1:fix32").arg(field);
            pos += 4;
            break;
        case 2: {
            if (!readVarint(d, pos, v) || pos + static_cast<qint64>(v) > d.size()) {
                parts << QStringLiteral("%1:<kaputt>").arg(field);
                pos = d.size();
                break;
            }
            const QByteArray sub = d.mid(pos, static_cast<int>(v));
            pos += static_cast<int>(v);
            if (printable(sub) && !sub.isEmpty() && !looksLikeMessage(sub))
                parts << QStringLiteral("%1:\"%2\"").arg(field).arg(QString::fromUtf8(sub.left(200)));
            else if (looksLikeMessage(sub))
                parts << QStringLiteral("%1:{%2}").arg(field).arg(dump(sub, depth + 1));
            else
                parts << QStringLiteral("%1:<%2 B %3>").arg(field).arg(sub.size())
                             .arg(QString::fromLatin1(sub.left(16).toHex()));
            break;
        }
        default:
            parts << QStringLiteral("<unlesbar ab %1>").arg(pos);
            pos = d.size();
        }
    }
    return parts.join(QLatin1Char(' '));
}

void Trace::message(bool outgoing, quint8 channel, quint8 flags, quint16 id, const QByteArray &payload)
{
    if (!m_log.isOpen()) return;
    const qint64 us = m_clock.nsecsElapsed() / 1000;
    const Kind k = kind(channel);
    const bool media = (k == Video || k == Audio || k == Mic) && (id == 0x0000 || id == 0x8004);
    const bool ping = k == Control && (id == 11 || id == 12);

    // Binary: header always, payload in full except big outgoing media.
    {
        const QByteArray body = (outgoing && media) ? payload.left(64) : payload;
        QByteArray rec;
        auto put = [&rec](quint64 v, int bytes) {
            for (int i = bytes - 1; i >= 0; --i) rec.append(static_cast<char>((v >> (8 * i)) & 0xff));
        };
        put(static_cast<quint64>(us), 8);
        rec.append(outgoing ? 'O' : 'I');
        rec.append(static_cast<char>(channel));
        rec.append(static_cast<char>(flags));
        put(id, 2);
        put(static_cast<quint64>(payload.size()), 4);
        put(static_cast<quint64>(body.size()), 4);
        rec.append(body);
        m_bin.write(rec);
    }

    // Text: thin out the steady stream of frames, acks and pings.
    const quint32 key = (outgoing ? 0x1000000u : 0u) | (static_cast<quint32>(channel) << 16) | id;
    const int n = ++m_seen[key];
    if ((media || ping) && n > 3 && n % 300 != 0) return;
    QString line = QStringLiteral("%1 %2 (%3 B)").arg(outgoing ? QStringLiteral("->") : QStringLiteral("<-"),
                                                     messageName(channel, id)).arg(payload.size());
    if (media || id == 0x0001 || (k == Control && id == 3)) {
        if (n > 3) line += QStringLiteral(" [#%1]").arg(n);
    } else if (!payload.isEmpty()) {
        line += QStringLiteral(" ") + (looksLikeMessage(payload) ? dump(payload)
                                                                 : QStringLiteral("hex:") + QString::fromLatin1(payload.left(32).toHex()));
    }
    text(line);
}

void Trace::discovery(const QByteArray &payload)
{
    if (m_dir.isEmpty()) return;
    QFile b(m_dir + QStringLiteral("/discovery.bin"));
    if (b.open(QIODevice::WriteOnly)) b.write(payload);
    QFile t(m_dir + QStringLiteral("/discovery.txt"));
    if (t.open(QIODevice::WriteOnly | QIODevice::Text)) {
        // One channel per line makes it easy to read.
        int pos = 0;
        QStringList lines;
        while (pos < payload.size()) {
            const int start = pos;
            quint64 key, v;
            if (!readVarint(payload, pos, key)) break;
            const int type = key & 7;
            if (type == 0) readVarint(payload, pos, v);
            else if (type == 2) { readVarint(payload, pos, v); pos += static_cast<int>(v); }
            else if (type == 1) pos += 8;
            else if (type == 5) pos += 4;
            else break;
            lines << dump(payload.mid(start, pos - start));
        }
        t.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
    }
}

} // namespace aap
