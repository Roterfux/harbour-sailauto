// Sound test: plays generated tones on the car's audio channels.
//   media:    a looping arpeggio on the media channel (48 kHz stereo),
//             first note left only, second right only (stereo check)
//   guidance: three short beeps on the navigation-voice channel; the car
//             should lower ("duck") its radio meanwhile
// Follows the Android Auto order: audio focus -> START -> PCM (paced in real
// time, within the car's ack window) -> STOP -> release focus.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace aap { class Session; }

class AudioTest : public QObject
{
    Q_OBJECT
public:
    enum Kind { Media, Guidance };
    explicit AudioTest(QObject *parent = nullptr);

    bool start(aap::Session *session, Kind kind, QString *error);
    void stop();
    bool running() const { return m_channel >= 0; }
    void onFocus(int state);

signals:
    void log(const QString &line);
    void finished();

private:
    void tick();
    QByteArray generate(qint64 firstFrame, int frames) const;

    QPointer<aap::Session> m_session;
    Kind m_kind = Media;
    int m_channel = -1;
    int m_rate = 48000, m_channels = 2;
    qint64 m_framesSent = 0;
    qint64 m_maxFrames = 0;
    int m_blocked = 0;
    QElapsedTimer m_clock;
    QTimer m_timer;
};
