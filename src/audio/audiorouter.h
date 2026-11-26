// Sends the phone's own sound (music apps, navigation voice, ...) to the car.
//
// While running:
//  - a PulseAudio null sink "sailauto_car" exists and is the default sink,
//  - every playing stream (sink input) is moved onto it, new ones too,
//  - its monitor is recorded (48 kHz, s16le, stereo, 20 ms chunks) and handed
//    out through chunk().
// stop() moves the streams back and restores the old default sink.
// Everything that happens is reported through log(), including streams the
// Sailfish audio policy moves back to the phone.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QObject>
#include <QString>
#include <atomic>

struct pa_threaded_mainloop;
struct pa_context;
class QThread;

class AudioRouter : public QObject
{
    Q_OBJECT
public:
    explicit AudioRouter(QObject *parent = nullptr);
    ~AudioRouter() override;

    bool start(QString *error);
    void stop();
    bool running() const { return m_running; }

    static constexpr int Rate = 48000;
    static constexpr int Channels = 2;

    // --- called from the PulseAudio thread (public for the C callbacks)
    void paLog(const QString &line);
    void onSinkInput(uint32_t index, uint32_t sink, const QString &name, const QString &app, const QString &role, bool isNew);
    void onSubscription(int facility, int type, uint32_t index);
    void runCapture();

signals:
    void log(const QString &line);
    void chunk(const QByteArray &pcm); // delivered in the main thread

private:
    bool waitOperation(struct pa_operation *op);
    void moveToCar(uint32_t index);
    void restoreAll();

    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_ctx = nullptr;
    bool m_running = false;
    QString m_oldDefault;
    uint32_t m_module = 0xffffffffu;
    uint32_t m_carSink = 0xffffffffu;
    QHash<uint32_t, uint32_t> m_origSink;  // sink input -> sink before we moved it
    QHash<uint32_t, int> m_moveCount;      // to stop fighting the policy
    QSet<uint32_t> m_seen;                 // streams already reported
    QThread *m_capture = nullptr;
    std::atomic<bool> m_stopCapture{false};
};
