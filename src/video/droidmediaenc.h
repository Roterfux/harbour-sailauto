// Hardware H.264 encoder through libdroidmedia (the phone's Android codec,
// loaded with libhybris at runtime: no build dependency). gst-droid's
// droidvenc only takes camera buffers; droidmedia itself also takes plain
// YUV frames when the encoder is created without "meta data" input.
//
// Safety: the Android struct layout changed in droidmedia 0.20260902 (two
// fields more in DroidMediaCodecMetaData), so the installed version decides
// the layout. A marker file guards against a crash inside the Android
// library: if the app died while trying, the next start skips it. No output
// within a few seconds -> failed() and the caller falls back to software.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QVector>

class DroidMediaEncoder : public QObject
{
    Q_OBJECT
public:
    explicit DroidMediaEncoder(QObject *parent = nullptr);
    ~DroidMediaEncoder() override;

    // libhybris + libdroidmedia there and no crash last time; *why if not
    static bool available(QString *why);
    bool start(int width, int height, int fps, int bitrateKbps, QString *error);
    void stop();
    bool running() const { return m_codec != nullptr; }
    QString description() const { return m_desc; }
    int pending() const { return m_inFlight; }
    void encode(const QImage &frame, quint64 timestampUs);
    // First output came: the hardware path works (crash marker removed)
    void confirmWorking();

signals:
    void output(const QByteArray &data, quint64 timestampUs, bool codecConfig);
    void failed(const QString &why);
    void dataFromCodec(const QByteArray &data, quint64 timestampUs, bool codecConfig); // internal
    void errorFromCodec(int err);                                                     // internal

public:
    // called from the codec's threads
    void onData(const void *data, long size, qint64 ts, bool config);
    void onError(int err);
    void onInputDone();

private:
    void toYuv(const QImage &img, quint8 *dst) const;

    void *m_codec = nullptr;
    QString m_desc;
    int m_width = 0, m_height = 0, m_stride = 0, m_sliceHeight = 0;
    quint32 m_colorFormat = 0;
    bool m_semiPlanar = true;
    QAtomicInt m_inFlight;
    bool m_confirmed = false;
};
