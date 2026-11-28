#include "h264encoder.h"

#include <QMetaObject>
#include <QSet>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

namespace {

struct Candidate { const char *element; const char *input; const char *pipeline; };

// Tried in order; the first one that links and starts wins. If an encoder
// fails before producing its first frame, the next one is tried.
// {KBPS} = bitrate kbit/s, {GOP} = keyframe interval (frames), {BPS} = bitrate bit/s
// input = caps between videoconvert and the encoder ("" = let them negotiate).
const Candidate kCandidates[] = {
    // Sailfish OS hardware encoder (gst-droid). Its accepted raw formats differ
    // per device, so try free negotiation first, then common formats.
    {"droidvenc", "", "droidvenc target-bitrate={BPS}"},
    {"droidvenc", "video/x-raw,format=NV12", "droidvenc target-bitrate={BPS}"},
    {"droidvenc", "video/x-raw,format=YV12", "droidvenc target-bitrate={BPS}"},
    {"droidvenc", "video/x-raw,format=I420", "droidvenc target-bitrate={BPS}"},
    {"x264enc", "video/x-raw,format=I420",
     "x264enc tune=zerolatency speed-preset=ultrafast bitrate={KBPS} key-int-max={GOP} bframes=0"
     " ! video/x-h264,profile=constrained-baseline"},
    {"openh264enc", "video/x-raw,format=I420", "openh264enc bitrate={BPS} gop-size={GOP} complexity=low rate-control=bitrate"},
    {"avenc_h264_omx", "", "avenc_h264_omx bitrate={BPS}"},
};
const int kCandidateCount = sizeof(kCandidates) / sizeof(kCandidates[0]);

// Candidates that failed once stay skipped while the app runs, so a video
// restart (car menu and back) does not waste a second on droidvenc again.
QSet<int> s_failed;

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
    m_dmWatch.setSingleShot(true);
    connect(&m_dmWatch, &QTimer::timeout, this, [this]() {
        if (m_dm.running() && !m_gotSample) dropToSoftware(QStringLiteral("kein Bild nach 4 s"));
    });
    connect(&m_dm, &DroidMediaEncoder::failed, this, [this](const QString &why) { dropToSoftware(why); });
    connect(&m_dm, &DroidMediaEncoder::output, this, [this](const QByteArray &data, quint64 ts, bool config) {
        if (!m_dm.running()) return;
        m_dm.confirmWorking();          // works: no crash marker any more
        m_dmWatch.stop();
        if (config) {                    // SPS/PPS from Android as its own buffer
            if (!m_sentConfig) {
                m_sentConfig = true;
                m_gotSample = true;
                emit encoded(data, ts, true);
            }
            return;
        }
        emitAccessUnit(data, ts);
    });
}

H264Encoder::~H264Encoder()
{
    stop();
}

bool H264Encoder::start(int width, int height, int fps, int bitrateKbps, QString *error)
{
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_kbps = bitrateKbps;
    m_errors.clear();
    return tryFrom(0, error);
}

namespace { bool s_hwAllowed = true; }

void H264Encoder::setHardwareAllowed(bool on) { s_hwAllowed = on; }

bool H264Encoder::startDroidMedia()
{
    QString why;
    if (!DroidMediaEncoder::available(&why)) {
        qInfo("[enc] droidmedia: %s", qPrintable(why));
        m_errors << QStringLiteral("Hardware: ") + why;
        return false;
    }
    if (!m_dm.start(m_width, m_height, m_fps, m_kbps, &why)) {
        qWarning("[enc] droidmedia: %s", qPrintable(why));
        m_errors << QStringLiteral("Hardware: ") + why;
        return false;
    }
    m_pending = 0;
    m_sentConfig = false;
    m_gotSample = false;
    m_encoderName = m_dm.description();
    m_dmWatch.start(4000);
    return true;
}

void H264Encoder::dropToSoftware(const QString &why)
{
    m_dmWatch.stop();
    if (!m_dm.running()) return;
    m_dm.stop();
    emit error(QStringLiteral("Hardware-Encoder: %1, nehme Software").arg(why));
    if (startOpenH264() || startBuiltin()) emit restarted(m_encoderName);
}

bool H264Encoder::tryFrom(int index, QString *error)
{
    stop();
    const QByteArray custom = qgetenv("SAILAUTO_ENCODER");
    if (index == 0 && (custom == "droidmedia" || (custom.isEmpty() && s_hwAllowed)) && startDroidMedia())
        return true;
    if (custom == "builtin")
        return index == 0 && startBuiltin();
    if (custom == "openh264")
        return index == 0 && startOpenH264();
    if (!custom.isEmpty()) {
        // Full override of "videoconvert ! <this>", e.g. "video/x-raw,format=NV12 ! droidvenc"
        if (index > 0) return false;
        m_candidate = 0;
        if (launch(QString::fromUtf8(custom), QString::fromUtf8(custom).section(QLatin1Char(' '), 0, 0), error))
            return true;
        return false;
    }

    for (int i = index; i < kCandidateCount; ++i) {
        if (s_failed.contains(i)) continue;
        const Candidate &c = kCandidates[i];
        GstElementFactory *f = gst_element_factory_find(c.element);
        if (!f) continue;
        gst_object_unref(f);
        QString enc = QString::fromLatin1(c.pipeline)
                          .replace(QLatin1String("{KBPS}"), QString::number(m_kbps))
                          .replace(QLatin1String("{GOP}"), QString::number(m_fps * 2))
                          .replace(QLatin1String("{BPS}"), QString::number(m_kbps * 1000));
        if (*c.input) enc = QString::fromLatin1(c.input) + QStringLiteral(" ! ") + enc;
        QString name = QString::fromLatin1(c.element);
        if (*c.input) name += QStringLiteral(" (") + QString::fromLatin1(c.input).section(QLatin1Char('='), -1) + QLatin1Char(')');
        QString err;
        m_candidate = i;
        if (launch(enc, name, &err))
            return true;
        m_errors << QStringLiteral("%1: %2").arg(name, err);
        s_failed.insert(i);
        qWarning("[enc] %s failed: %s", qPrintable(name), qPrintable(err));
    }
    // Compiled-in openh264 (software, compresses well).
    if (startOpenH264())
        return true;
    // Last resort, always works: built-in lossless encoder.
    if (!m_errors.isEmpty())
        qWarning("[enc] GStreamer encoders failed: %s", qPrintable(m_errors.join(QStringLiteral(" | "))));
    if (startBuiltin())
        return true;
    if (error) *error = QStringLiteral("Kein Encoder lief: ") + m_errors.join(QStringLiteral(" | "));
    return false;
}

bool H264Encoder::startOpenH264()
{
    stop();
    QString err;
    if (!m_oh.start(m_width, m_height, m_fps, m_kbps, &err)) {
        m_errors << QStringLiteral("openh264 (intern): ") + err;
        return false;
    }
    m_ohFrames = 0;
    m_pending = 0;
    m_sentConfig = false;
    m_gotSample = true;
    m_encoderName = QStringLiteral("openh264 (intern)");
    qInfo("[enc] built-in openh264 %dx%d @%d fps, %d kbit/s", m_width, m_height, m_fps, m_kbps);
    return true;
}

bool H264Encoder::startBuiltin()
{
    stop();
    m_pcm.reset(m_width, m_height);
    m_builtin = true;
    m_builtinFrames = 0;
    m_pending = 0;
    m_sentConfig = false;
    m_gotSample = true;
    m_encoderName = QStringLiteral("eingebaut (PCM)");
    qInfo("[enc] built-in PCM/skip encoder %dx%d", m_width, m_height);
    return true;
}

bool H264Encoder::launch(const QString &enc, const QString &name, QString *error)
{
    const QString desc = QStringLiteral(
        "appsrc name=src is-live=true format=time do-timestamp=false block=false "
        "caps=video/x-raw,format=BGRx,width=%1,height=%2,framerate=%3/1 "
        "! videoconvert ! %4 "
        "! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au "
        "! appsink name=sink sync=false emit-signals=true max-buffers=8 drop=false")
        .arg(m_width).arg(m_height).arg(m_fps).arg(enc);

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline || err) {
        if (error) *error = err ? QString::fromUtf8(err->message) : QStringLiteral("?");
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
    m_gotSample = false;
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = QStringLiteral("Pipeline startet nicht");
        stop();
        return false;
    }
    m_encoderName = name;
    m_busTimer.start();
    qInfo("[enc] %s %dx%d @%d fps, %d kbit/s", qPrintable(name), m_width, m_height, m_fps, m_kbps);
    return true;
}

void H264Encoder::stop()
{
    m_dmWatch.stop();
    m_dm.stop();
    m_builtin = false;
    m_oh.stop();
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
    if (m_dm.running()) {
        m_dm.encode(frame, timestampUs);
        return;
    }
    if (m_oh.running()) {
        const QByteArray au = m_oh.encode(frame, timestampUs, m_ohFrames++ == 0);
        if (!au.isEmpty()) {
            ++m_pending;          // onSample() counts it down again
            onSample(au, timestampUs);
        }
        return;
    }
    if (m_builtin) {
        if (!m_sentConfig) {
            m_sentConfig = true;
            emit encoded(m_pcm.codecConfig(), timestampUs, true);
        }
        // Keyframe every ~10 s so a decoder that lost track recovers.
        const bool key = ++m_builtinFrames % (m_fps * 10) == 1;
        const QByteArray au = m_pcm.encode(frame, key);
        if (!au.isEmpty()) emit encoded(au, timestampUs, false);
        return;
    }
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
    if (!m_pipeline && !m_oh.running()) return;
    m_pending = qMax(0, m_pending - 1);
    emitAccessUnit(data, timestampUs);
}

void H264Encoder::emitAccessUnit(const QByteArray &data, quint64 timestampUs)
{
    m_gotSample = true;

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
        const bool isError = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR;
        if (err) g_error_free(err);
        g_free(dbg);
        gst_message_unref(msg);
        if (!isError) {
            qWarning("[enc] %s", qPrintable(text));
            continue;
        }
        if (!m_gotSample) {
            // Failed while negotiating: try the next encoder/format.
            m_errors << QStringLiteral("%1: %2").arg(m_encoderName, text);
            s_failed.insert(m_candidate);
            emit error(QStringLiteral("%1 geht nicht, versuche nächsten Encoder").arg(m_encoderName));
            gst_object_unref(bus);
            QString e;
            if (tryFrom(m_candidate + 1, &e)) emit restarted(m_encoderName);
            else emit error(e);
            return;
        }
        emit error(text);
    }
    gst_object_unref(bus);
}
