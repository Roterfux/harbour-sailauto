// RGB frames in, H.264 Annex-B access units out, via GStreamer.
// On Sailfish OS the hardware encoder (droidvenc from gst-droid) is tried
// first, then software encoders. SAILAUTO_ENCODER overrides the encoder part
// of the pipeline, e.g. SAILAUTO_ENCODER="x264enc tune=zerolatency".
#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QTimer>

typedef struct _GstElement GstElement;

class H264Encoder : public QObject
{
    Q_OBJECT
public:
    explicit H264Encoder(QObject *parent = nullptr);
    ~H264Encoder() override;

    bool start(int width, int height, int fps, int bitrateKbps, QString *error);
    void stop();
    bool running() const { return m_pipeline != nullptr; }
    QString encoderName() const { return m_encoderName; }

    // Frames in flight inside the encoder (pushed, not yet out).
    int pending() const { return m_pending; }
    void push(const QImage &frame, quint64 timestampUs);

signals:
    // One access unit. codecConfig = SPS/PPS split from the first keyframe.
    void encoded(const QByteArray &data, quint64 timestampUs, bool codecConfig);
    void error(const QString &message);

private slots:
    void onSample(const QByteArray &data, quint64 timestampUs);

private:
    static int onNewSample(GstElement *sink, void *self);
    void pollBus();

    GstElement *m_pipeline = nullptr;
    GstElement *m_src = nullptr;
    QString m_encoderName;
    int m_pending = 0;
    bool m_sentConfig = false;
    QTimer m_busTimer;
};
