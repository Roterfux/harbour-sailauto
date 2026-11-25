// Session recorder for debugging without the car.
//
// Per session a folder ~/Documents/SailAuto/<yyyyMMdd-HHmmss>/ with
//   log.txt        readable log: every message (named + decoded), errors
//   trace.bin      every message raw (outgoing video shortened), for
//                  tools/aap_trace.py
//   discovery.bin  the head unit's ServiceDiscoveryResponse, exactly as
//                  received; tools/hu_sim.py --discovery replays it
//   discovery.txt  the same, decoded
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QString>

namespace aap {

class Trace
{
public:
    // Kinds of channel, for naming messages.
    enum Kind { Control, Video, Audio, Input, Sensor, Mic, Bluetooth, Playback, Navigation, Phone, Other };

    bool open(const QString &baseDir);
    bool isOpen() const { return m_log.isOpen(); }
    QString dir() const { return m_dir; }

    void setKind(int channel, Kind k) { m_kinds[channel] = k; }
    Kind kind(int channel) const { return channel == 0 ? Control : m_kinds.value(channel, Other); }

    void text(const QString &line);
    void message(bool outgoing, quint8 channel, quint8 flags, quint16 id, const QByteArray &payload);
    void discovery(const QByteArray &payload);

    QString messageName(quint8 channel, quint16 id) const;
    static QString dump(const QByteArray &pb, int depth = 0);

private:
    QString m_dir;
    QFile m_log, m_bin;
    QElapsedTimer m_clock;
    QHash<int, Kind> m_kinds;
    QHash<quint32, int> m_seen; // (dir,channel,id) -> count, to thin out media spam
};

} // namespace aap
