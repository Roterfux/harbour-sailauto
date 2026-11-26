#include "audiotest.h"
#include "../aap/session.h"

#include <QtEndian>
#include <QtMath>
#include <cmath>

namespace {
// C major arpeggio, a quarter second each (Hz).
const double kNotes[] = {523.25, 659.25, 783.99, 1046.50, 783.99, 659.25};
const int kNoteCount = 6;
const double kNoteLen = 0.25;
}

AudioTest::AudioTest(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, &AudioTest::tick);
}

bool AudioTest::start(aap::Session *session, Kind kind, QString *error)
{
    stop();
    if (!session) {
        if (error) *error = QStringLiteral("keine Verbindung zum Auto");
        return false;
    }
    const int streamType = kind == Media ? 3 : 1;
    const int ch = session->audioChannel(streamType);
    if (ch < 0) {
        if (error) *error = QStringLiteral("Auto hat keinen eingerichteten %1-Kanal")
                                .arg(kind == Media ? QStringLiteral("Medien") : QStringLiteral("Ansage"));
        return false;
    }
    m_session = session;
    m_kind = kind;
    session->audioFormat(ch, &m_rate, &m_channels);
    m_rate = qBound(8000, m_rate, 48000);
    m_channels = qBound(1, m_channels, 2);
    // media: 60 s max; guidance: 3 beeps = 0.9 s
    m_maxFrames = kind == Media ? static_cast<qint64>(m_rate) * 60 : static_cast<qint64>(m_rate * 0.9);

    session->requestAudioFocus(kind == Media ? 1 /*GAIN*/ : 3 /*GAIN_TRANSIENT_MAY_DUCK*/);
    if (!session->startAudio(ch)) {
        if (error) *error = QStringLiteral("START auf Kanal %1 nicht möglich").arg(ch);
        return false;
    }
    m_channel = ch;
    m_framesSent = 0;
    m_blocked = 0;
    m_clock.start();
    m_timer.start();
    emit log(QStringLiteral("Tontest %1 auf Kanal %2: %3 Hz, %4 Kanal/Kanäle")
                 .arg(kind == Media ? QStringLiteral("Musik") : QStringLiteral("Ansage"))
                 .arg(ch).arg(m_rate).arg(m_channels));
    return true;
}

void AudioTest::stop()
{
    if (m_channel < 0) return;
    m_timer.stop();
    if (m_session) {
        m_session->stopAudio(m_channel);
        m_session->requestAudioFocus(4 /*RELEASE*/);
    }
    emit log(QStringLiteral("Tontest gestoppt nach %1 s Ton").arg(double(m_framesSent) / m_rate, 0, 'f', 1));
    m_channel = -1;
    emit finished();
}

void AudioTest::onFocus(int state)
{
    // LOSS (3) or LOSS_TRANSIENT (5): the car wants the speakers back.
    if (running() && (state == 3 || state == 5)) {
        emit log(QStringLiteral("Auto nimmt Audio-Fokus weg, Tontest endet"));
        stop();
    }
}

QByteArray AudioTest::generate(qint64 firstFrame, int frames) const
{
    QByteArray out(frames * m_channels * 2, Qt::Uninitialized);
    qint16 *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < frames; ++i) {
        const double t = double(firstFrame + i) / m_rate;
        double v = 0, left = 1, right = 1;
        if (m_kind == Media) {
            const int n = static_cast<int>(t / kNoteLen);
            const double inNote = t - n * kNoteLen;
            const double env = qMin(1.0, inNote / 0.01) * qMin(1.0, (kNoteLen - inNote) / 0.04);
            v = 0.25 * env * qSin(2 * M_PI * kNotes[n % kNoteCount] * t);
            // Stereo check every 12 notes: one note left only, next right only.
            if (n % 12 == 0) right = 0;
            else if (n % 12 == 1) left = 0;
        } else {
            const double inBeep = std::fmod(t, 0.3);
            if (inBeep < 0.15) {
                const double env = qMin(1.0, inBeep / 0.005) * qMin(1.0, (0.15 - inBeep) / 0.02);
                v = 0.3 * env * qSin(2 * M_PI * 880.0 * t);
            }
        }
        // little-endian signed 16 bit
        if (m_channels == 2) {
            *dst++ = qToLittleEndian(static_cast<qint16>(v * left * 32767));
            *dst++ = qToLittleEndian(static_cast<qint16>(v * right * 32767));
        } else {
            *dst++ = qToLittleEndian(static_cast<qint16>(v * 32767));
        }
    }
    return out;
}

void AudioTest::tick()
{
    if (!m_session || m_session->state() != aap::Session::Streaming) {
        stop();
        return;
    }
    // Stay ~150 ms ahead of real time, in 20 ms packets.
    const qint64 target = static_cast<qint64>((m_clock.nsecsElapsed() / 1e9 + 0.15) * m_rate);
    const int chunk = m_rate / 50;
    while (m_framesSent < target && m_framesSent < m_maxFrames) {
        if (!m_session->canSendAudio(m_channel)) {
            if (++m_blocked == 100) emit log(QStringLiteral("Tontest: Auto bestätigt keine Audiopakete (ACK fehlt)"));
            return;
        }
        const int n = static_cast<int>(qMin<qint64>(chunk, m_maxFrames - m_framesSent));
        const quint64 ts = static_cast<quint64>(m_framesSent * 1000000 / m_rate);
        m_session->sendAudio(m_channel, generate(m_framesSent, n), ts);
        m_framesSent += n;
    }
    // Let the last packet play out before STOP.
    if (m_framesSent >= m_maxFrames && m_clock.elapsed() > m_maxFrames * 1000 / m_rate + 300)
        stop();
}
