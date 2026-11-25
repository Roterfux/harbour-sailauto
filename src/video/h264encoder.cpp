#include "h264encoder.h"

#include <QMetaObject>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace {

struct Candidate { const char *element; const char *pipeline; };

// {KBPS} = bitrate kbit/s, {GOP} = keyframe interval (frames), {BPS} = bitrate bit/s
const Candidate kCandidates[] = {
    // Sailfish OS hardware encoder (gst-droid)
    {"droidvenc", "droidvenc target-bitrate={BPS}"},
    {"x264enc", "x264enc tune=zerolatency speed-preset=ultrafast bitrate={KBPS} key-int-max={GOP} bframes=0"
                " ! video/x-h264,profile=constrained-baseline"},
    {"openh264enc", "openh264enc bitrate={BPS} gop-size={GOP} complexity=low rate-control=bitrate"},
    {"avenc_h264_omx", "avenc_h264_omx bitrate={BPS}"},
};

int nalType(const QByteArray &d, int pos) { return static_cast<quint8>(d[pos]) & 0x1f; }

// Returns offsets of NAL payload starts (after the 00 00 01 / 00 00 00 01 start code).
QList<QPair<int, int>> splitNals(const QByteArray &d)
{
    QList<QPair<int, int>> nals; // (start of start code, start of payload)
    for (int i = 0; i + 3 <= d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            const int sc = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
            nals.append(qMakePair(sc, i + 3));
            i += 2;
        }
    }
    return nals;
}

} // namespace

H264Encoder::H264Encoder(QObject *parent) : QObject(parent)
{
    if (!gst_is_initialized())
        gst_init(nullptr, nullptr);
    m_busTimer.setInterval(500);
    connect(&m_busTimer, &QTimer::timeout, this, &H264Encoder::pollBus);
}

H264Encoder::~H264Encoder()
{
    stop();
}

bool H264Encoder::start(int width, int height, int fps, int bitrateKbps, QString *error)
{
    stop();
    QString enc;
    const QByteArray custom = qgetenv("SAILAUTO_ENCODER");
    if (!custom.isEmpty()) {
        enc = QString::fromUtf8(custom);
        m_encoderName = enc.section(' ', 0, 0);
    } else {
        for (const Candidate &c : kCandidates) {
            GstElementFactory *f = gst_element_factory_find(c.element);
            if (!f) continue;
            gst_object_unref(f);
            enc = QString::fromLatin1(c.pipeline)
                      .replace(QLatin1String("{KBPS}"), QString::number(bitrateKbps))
                      .replace(QLatin1String("{GOP}"), QString::number(fps * 2))
                      .replace(QLatin1String("{BPS}"), QString::number(bitrateKbps * 1000));
            m_encoderName = QString::fromLatin1(c.element);
            break;
        }
    }
    if (enc.isEmpty()) {
        if (error) *error = QStringLiteral("Kein H.264-Encoder gefunden (droidvenc/x264enc/openh264enc)");
        return false;
    }

    const QString desc = QStringLiteral(
        "appsrc name=src is-live=true format=time do-timestamp=false block=false "
        "caps=video/x-raw,format=BGRx,width=%1,height=%2,framerate=0/1 "
        "! videoconvert ! video/x-raw,format=I420 ! %3 "
        "! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au "
        "! appsink name=sink sync=false emit-signals=true max-buffers=8 drop=false")
        .arg(width).arg(height).arg(enc);

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline || err) {
        if (error) *error = QStringLiteral("GStreamer: %1").arg(err ? QString::fromUtf8(err->message) : QStringLiteral("?"));
        if (err) g_error_free(err);
        if (m_pipeline) gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        return false;
    }
    m_src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
    GstElement *sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    g_signal_connect(sink, "new-sample", G_CALLBACK(&H264Encoder::onNewSample), this);
    gst_object_unref(sink);

    m_pending = 0;
    m_sentConfig = false;
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = QStringLiteral("Pipeline startet nicht (%1)").arg(m_encoderName);
        stop();
        return false;
    }
    m_busTimer.start();
    qInfo("[enc] %s %dx%d @%d fps, %d kbit/s", qPrintable(m_encoderName), width, height, fps, bitrateKbps);
    return true;
}

void H264Encoder::stop()
{
    m_busTimer.stop();
    if (!m_pipeline) return;
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    if (m_src) gst_object_unref(m_src);
    gst_object_unref(m_pipeline);
    m_src = nullptr;
    m_pipeline = nullptr;
    m_pending = 0;
}

void H264Encoder::push(const QImage &frame, quint64 timestampUs)
{
    if (!m_src) return;
    const int bytes = frame.bytesPerLine() * frame.height();
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, static_cast<gsize>(bytes), nullptr);
    gst_buffer_fill(buf, 0, frame.constBits(), static_cast<gsize>(bytes));
    GST_BUFFER_PTS(buf) = timestampUs * GST_USECOND;
    GST_BUFFER_DURATION(buf) = GST_CLOCK_TIME_NONE;
    ++m_pending;
    gst_app_src_push_buffer(GST_APP_SRC(m_src), buf); // takes ownership
}

// Runs on a GStreamer streaming thread.
int H264Encoder::onNewSample(GstElement *sink, void *self)
{
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) return GST_FLOW_OK;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        QByteArray data(reinterpret_cast<const char *>(map.data), static_cast<int>(map.size));
        const quint64 ts = GST_BUFFER_PTS_IS_VALID(buf) ? GST_BUFFER_PTS(buf) / GST_USECOND : 0;
        gst_buffer_unmap(buf, &map);
        QMetaObject::invokeMethod(static_cast<H264Encoder *>(self), "onSample", Qt::QueuedConnection,
                                  Q_ARG(QByteArray, data), Q_ARG(quint64, ts));
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

void H264Encoder::onSample(const QByteArray &data, quint64 timestampUs)
{
    if (!m_pipeline) return;
    m_pending = qMax(0, m_pending - 1);

    // Android phones send SPS/PPS once as a separate "codec config" message
    // before the first frame. Do the same.
    if (!m_sentConfig) {
        QByteArray config, rest;
        const auto nals = splitNals(data);
        for (int i = 0; i < nals.size(); ++i) {
            const int end = i + 1 < nals.size() ? nals[i + 1].first : data.size();
            const QByteArray nal = data.mid(nals[i].first, end - nals[i].first);
            const int t = nalType(data, nals[i].second);
            if (t == 7 || t == 8) config.append(nal);
            else if (t != 9) rest.append(nal); // drop access unit delimiters
        }
        if (config.isEmpty()) return; // wait for a keyframe with SPS/PPS
        m_sentConfig = true;
        emit encoded(config, timestampUs, true);
        if (!rest.isEmpty()) emit encoded(rest, timestampUs, false);
        return;
    }
    emit encoded(data, timestampUs, false);
}

void H264Encoder::pollBus()
{
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    while (GstMessage *msg = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING))) {
        GError *err = nullptr;
        gchar *dbg = nullptr;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) gst_message_parse_error(msg, &err, &dbg);
        else gst_message_parse_warning(msg, &err, &dbg);
        const QString text = QStringLiteral("%1 (%2)").arg(QString::fromUtf8(err ? err->message : "?"), QString::fromUtf8(dbg ? dbg : ""));
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) emit error(text);
        else qWarning("[enc] %s", qPrintable(text));
        if (err) g_error_free(err);
        g_free(dbg);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}
