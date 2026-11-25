// RGB frames in, H.264 Annex-B access units out, via GStreamer.
// On Sailfish OS the hardware encoder (droidvenc from gst-droid) is tried
// first (several input formats), then software encoders. SAILAUTO_ENCODER
// overrides everything between videoconvert and h264parse, e.g.
// SAILAUTO_ENCODER="video/x-raw,format=NV12 ! droidvenc".
#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include "openh264enc.h"
#include "pcmh264.h"

typedef struct _GstElement GstElement;

class H264Encoder : public QObject
{
    Q_OBJECT
public:
    explicit H264Encoder(QObject *parent = nullptr);
    ~H264Encoder() override;

    bool start(int width, int height, int fps, int bitrateKbps, QString *error);
    void stop();
    bool running() const { return m_pipeline != nullptr || m_builtin || m_oh.running(); }
    QString encoderName() const { return m_encoderName; }

    // Frames in flight inside the encoder (pushed, not yet out).
    int pending() const { return m_pending; }
    void push(const QImage &frame, quint64 timestampUs);

signals:
    // Switched to another encoder after the previous one failed.
    void restarted(const QString &name);
    // One access unit. codecConfig = SPS/PPS split from the first keyframe.
    void encoded(const QByteArray &data, quint64 timestampUs, bool codecConfig);
    void error(const QString &message);

private slots:
    void onSample(const QByteArray &data, quint64 timestampUs);

private:
    static int onNewSample(GstElement *sink, void *self);
    bool tryFrom(int index, QString *error);
    bool launch(const QString &enc, const QString &name, QString *error);
    void pollBus();

    GstElement *m_pipeline = nullptr;
    GstElement *m_src = nullptr;
    QString m_encoderName;
    int m_pending = 0;
    bool m_sentConfig = false;
    bool m_gotSample = false;
    int m_candidate = 0;
    int m_width = 0, m_height = 0, m_fps = 30, m_kbps = 2000;
    QStringList m_errors;
    bool m_builtin = false;
    int m_builtinFrames = 0;
    PcmH264Encoder m_pcm;
    bool startBuiltin();
    bool startOpenH264();
    OpenH264Encoder m_oh;
    int m_ohFrames = 0;
    QTimer m_busTimer;
};
