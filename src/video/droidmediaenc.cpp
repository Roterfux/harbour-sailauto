#include "droidmediaenc.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/types.h>

namespace {
// ---- droidmedia C API (subset), see sailfishos/droidmedia droidmediacodec.h ----
struct DmData { void *data; ssize_t size; };
struct DmBufferCallbacks { void (*unref)(void *); void *data; };
struct DmCodecData { DmData data; int64_t ts; int64_t decoding_ts; bool sync; bool codec_config; };
struct DmCallbacks { void (*signal_eos)(void *); void (*error)(void *, int); int (*size_changed)(void *, int32_t, int32_t); };
struct DmDataCallbacks { void (*data_available)(void *, DmCodecData *); };

// DroidMediaCodecMetaData before 0.20260902 ...
struct MetaOld { const char *type; int32_t width, height, fps, channels, sample_rate, hal_format; int32_t flags; };
// ... and from 0.20260902 on (stride, slice_height added)
struct MetaNew { const char *type; int32_t width, height, fps, channels, sample_rate, hal_format; int32_t flags;
                 int32_t stride, slice_height; };
struct EncTail { int32_t color_format, bitrate, meta_data, stride, slice_height, max_input_size, bitrate_mode;
                 int32_t h264_profile, h264_level, h264_prepend_header; };
template <class M> struct EncMeta { M parent; EncTail enc; };

struct Api {
    bool loaded = false;
    QString error;
    void (*init)() = nullptr; // _droid_media_init
    void *(*createEncoder)(void *meta) = nullptr;
    unsigned (*colorFormats)(void *meta, int encoder, uint32_t *formats, unsigned max) = nullptr;
    void (*setCallbacks)(void *codec, DmCallbacks *cb, void *data) = nullptr;
    void (*setDataCallbacks)(void *codec, DmDataCallbacks *cb, void *data) = nullptr;
    bool (*start)(void *codec) = nullptr;
    void (*stop)(void *codec) = nullptr;
    void (*destroy)(void *codec) = nullptr;
    void (*queue)(void *codec, DmCodecData *data, DmBufferCallbacks *cb) = nullptr;
    bool newLayout = false;
    QString version;
};

Api &api()
{
    static Api a;
    static bool tried = false;
    if (tried) return a;
    tried = true;
    const QByteArray hybrisLib = qEnvironmentVariableIsSet("SAILAUTO_HYBRIS_LIB") ? qgetenv("SAILAUTO_HYBRIS_LIB") // tests
                                                                                   : QByteArray("libhybris-common.so.1");
    void *hybris = dlopen(hybrisLib.constData(), RTLD_LAZY);
    if (!hybris) { a.error = QStringLiteral("libhybris fehlt"); return a; }
    auto adlopen = reinterpret_cast<void *(*)(const char *, int)>(dlsym(hybris, "android_dlopen"));
    auto adlsym = reinterpret_cast<void *(*)(void *, const char *)>(dlsym(hybris, "android_dlsym"));
    if (!adlopen || !adlsym) { a.error = QStringLiteral("android_dlopen fehlt"); return a; }
    void *lib = adlopen("libdroidmedia.so", RTLD_NOW);
    if (!lib) { a.error = QStringLiteral("libdroidmedia.so nicht ladbar"); return a; }
    auto sym = [&](const char *n) { return adlsym(lib, n); };
    a.init = reinterpret_cast<void (*)()>(sym("_droid_media_init"));
    a.createEncoder = reinterpret_cast<void *(*)(void *)>(sym("droid_media_codec_create_encoder"));
    a.colorFormats = reinterpret_cast<unsigned (*)(void *, int, uint32_t *, unsigned)>(sym("droid_media_codec_get_supported_color_formats"));
    a.setCallbacks = reinterpret_cast<void (*)(void *, DmCallbacks *, void *)>(sym("droid_media_codec_set_callbacks"));
    a.setDataCallbacks = reinterpret_cast<void (*)(void *, DmDataCallbacks *, void *)>(sym("droid_media_codec_set_data_callbacks"));
    a.start = reinterpret_cast<bool (*)(void *)>(sym("droid_media_codec_start"));
    a.stop = reinterpret_cast<void (*)(void *)>(sym("droid_media_codec_stop"));
    a.destroy = reinterpret_cast<void (*)(void *)>(sym("droid_media_codec_destroy"));
    a.queue = reinterpret_cast<void (*)(void *, DmCodecData *, DmBufferCallbacks *)>(sym("droid_media_codec_queue"));
    if (!a.init || !a.createEncoder || !a.setCallbacks || !a.setDataCallbacks || !a.start || !a.stop || !a.destroy || !a.queue) {
        a.error = QStringLiteral("Funktionen fehlen in libdroidmedia");
        return a;
    }
    // Layout by the installed version (0.YYYYMMDD.n)
    QProcess rpm;
    rpm.start(QStringLiteral("rpm"), QStringList() << QStringLiteral("-q") << QStringLiteral("--qf") << QStringLiteral("%{VERSION}")
                                                   << QStringLiteral("droidmedia"));
    if (rpm.waitForFinished(3000)) a.version = QString::fromLatin1(rpm.readAllStandardOutput()).trimmed();
    const qint64 date = a.version.section(QLatin1Char('.'), 1, 1).toLongLong();
    a.newLayout = date >= 20260902;
    a.init();
    a.loaded = true;
    return a;
}

QString crashMarker()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/hwenc-trying");
}

// OMX colour formats
const uint32_t kYUV420Planar = 19, kYUV420SemiPlanar = 21, kQcomNV12_32m = 0x7FA30C04, kQcomNV12 = 0x7FA30C00;

void freeBuffer(void *p) { std::free(p); }

void cbError(void *d, int err) { static_cast<DroidMediaEncoder *>(d)->onError(err); }
void cbEos(void *) {}
int cbSize(void *, int32_t, int32_t) { return 0; }
void cbData(void *d, DmCodecData *e)
{
    static_cast<DroidMediaEncoder *>(d)->onData(e->data.data, long(e->data.size), e->ts, e->codec_config);
}
} // namespace

DroidMediaEncoder::DroidMediaEncoder(QObject *parent) : QObject(parent)
{
    // Codec threads emit these; queued to the Qt thread (works with Qt 5.6)
    connect(this, &DroidMediaEncoder::dataFromCodec, this, [this](const QByteArray &data, quint64 ts, bool config) {
        if (!config && m_inFlight > 0) m_inFlight.deref();
        emit output(data, ts, config);
    }, Qt::QueuedConnection);
    connect(this, &DroidMediaEncoder::errorFromCodec, this, [this](int err) {
        emit failed(QStringLiteral("Fehler 0x%1 im Hardware-Encoder").arg(-err, 0, 16));
    }, Qt::QueuedConnection);
}

DroidMediaEncoder::~DroidMediaEncoder() { stop(); }

bool DroidMediaEncoder::available(QString *why)
{
    if (QFile::exists(crashMarker())) {
        if (why) *why = QStringLiteral("ging beim letzten Versuch nicht (Datei %1 löschen für einen neuen Versuch)").arg(crashMarker());
        return false;
    }
    if (!qEnvironmentVariableIsSet("SAILAUTO_HYBRIS_LIB")
        && !QFile::exists(QStringLiteral("/usr/libexec/droid-hybris/system/lib64/libdroidmedia.so"))
        && !QFile::exists(QStringLiteral("/usr/libexec/droid-hybris/system/lib/libdroidmedia.so"))) {
        if (why) *why = QStringLiteral("kein libdroidmedia am Gerät");
        return false;
    }
    return true;
}

bool DroidMediaEncoder::start(int width, int height, int fps, int bitrateKbps, QString *error)
{
    stop();
    // Crash guard: set before touching Android code, removed after the first output
    QDir().mkpath(QFileInfo(crashMarker()).path());
    { QFile f(crashMarker()); f.open(QIODevice::WriteOnly); }
    Api &a = api();
    if (!a.loaded) {
        QFile::remove(crashMarker());
        if (error) *error = a.error;
        return false;
    }
    m_width = width;
    m_height = height;
    // Pick the colour format the encoder takes
    MetaNew probeNew{};
    MetaOld probeOld{};
    probeNew.type = probeOld.type = "video/avc";
    probeNew.width = probeOld.width = width;
    probeNew.height = probeOld.height = height;
    probeNew.fps = probeOld.fps = fps;
    uint32_t formats[64] = {};
    unsigned n = a.colorFormats ? a.colorFormats(a.newLayout ? static_cast<void *>(&probeNew) : static_cast<void *>(&probeOld), 1, formats, 64) : 0;
    QStringList fl;
    for (unsigned i = 0; i < n && i < 64; ++i) fl << QStringLiteral("0x%1").arg(formats[i], 0, 16);
    auto has = [&](uint32_t f) { for (unsigned i = 0; i < n && i < 64; ++i) if (formats[i] == f) return true; return false; };
    m_stride = width;
    m_sliceHeight = height;
    if (has(kYUV420SemiPlanar) || n == 0) { m_colorFormat = kYUV420SemiPlanar; m_semiPlanar = true; }
    else if (has(kYUV420Planar)) { m_colorFormat = kYUV420Planar; m_semiPlanar = false; }
    else if (has(kQcomNV12_32m) || has(kQcomNV12)) {
        m_colorFormat = has(kQcomNV12_32m) ? kQcomNV12_32m : kQcomNV12;
        m_semiPlanar = true;
        m_stride = (width + 127) & ~127;       // Qualcomm NV12: 128-byte rows, 32-line planes
        m_sliceHeight = (height + 31) & ~31;
    } else {
        QFile::remove(crashMarker());
        if (error) *error = QStringLiteral("kein passendes Farbformat (%1)").arg(fl.join(QLatin1Char(',')));
        return false;
    }
    EncTail t{};
    t.color_format = int32_t(m_colorFormat);
    t.bitrate = bitrateKbps * 1000;
    t.meta_data = 0;                         // plain YUV frames, not camera buffers
    t.stride = m_stride;
    t.slice_height = m_sliceHeight;
    t.max_input_size = m_stride * m_sliceHeight * 3 / 2;
    t.bitrate_mode = 1;                      // VBR: few bits for still pictures
    t.h264_profile = 1;                      // OMX_VIDEO_AVCProfileBaseline
    t.h264_level = 0;                        // let Android pick
    t.h264_prepend_header = 1;               // SPS/PPS with every keyframe
    // Generous zeroed buffer: a newer library reading more fields sees zeros
    QByteArray metaBuf(512, '\0');
    if (a.newLayout) {
        EncMeta<MetaNew> m{};
        m.parent = probeNew;
        m.parent.stride = m_stride;
        m.parent.slice_height = m_sliceHeight;
        m.enc = t;
        std::memcpy(metaBuf.data(), &m, sizeof m);
    } else {
        EncMeta<MetaOld> m{};
        m.parent = probeOld;
        m.enc = t;
        std::memcpy(metaBuf.data(), &m, sizeof m);
    }
    m_codec = a.createEncoder(metaBuf.data());
    if (!m_codec) {
        QFile::remove(crashMarker());
        if (error) *error = QStringLiteral("Encoder ließ sich nicht anlegen (Farbformate %1)").arg(fl.join(QLatin1Char(',')));
        return false;
    }
    static DmCallbacks cb{cbEos, cbError, cbSize};
    static DmDataCallbacks dcb{cbData};
    a.setCallbacks(m_codec, &cb, this);
    a.setDataCallbacks(m_codec, &dcb, this);
    if (!a.start(m_codec)) {
        a.destroy(m_codec);
        m_codec = nullptr;
        QFile::remove(crashMarker());
        if (error) *error = QStringLiteral("Encoder startet nicht");
        return false;
    }
    m_inFlight = 0;
    m_confirmed = false;
    m_desc = QStringLiteral("Hardware (droidmedia %1, Format 0x%2%3)")
                 .arg(a.version.isEmpty() ? QStringLiteral("?") : a.version)
                 .arg(m_colorFormat, 0, 16)
                 .arg(m_stride != width ? QStringLiteral(", Zeile %1").arg(m_stride) : QString());
    qInfo("[enc] droidmedia %dx%d stride %d/%d format 0x%x, layout %s, formats %s", width, height, m_stride, m_sliceHeight,
          m_colorFormat, a.newLayout ? "new" : "old", qPrintable(fl.join(QLatin1Char(','))));
    return true;
}

void DroidMediaEncoder::confirmWorking()
{
    if (m_confirmed) return;
    m_confirmed = true;
    QFile::remove(crashMarker());
}

void DroidMediaEncoder::stop()
{
    if (!m_codec) return;
    Api &a = api();
    a.stop(m_codec);
    a.destroy(m_codec);
    m_codec = nullptr;
    m_inFlight = 0;
}

void DroidMediaEncoder::toYuv(const QImage &src, quint8 *dst) const
{
    const QImage img = src.format() == QImage::Format_RGB32 || src.format() == QImage::Format_ARGB32
                           ? src : src.convertToFormat(QImage::Format_RGB32);
    const int w = qMin(m_width, img.width()) & ~1, h = qMin(m_height, img.height()) & ~1;
    quint8 *yPlane = dst;
    quint8 *cPlane = dst + m_stride * m_sliceHeight;               // UV interleaved (NV12) or U
    quint8 *vPlane = cPlane + (m_stride / 2) * (m_sliceHeight / 2); // V (planar only)
    for (int y = 0; y < h; y += 2) {
        const QRgb *r0 = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        const QRgb *r1 = reinterpret_cast<const QRgb *>(img.constScanLine(y + 1));
        quint8 *y0 = yPlane + y * m_stride, *y1 = y0 + m_stride;
        quint8 *uv = m_semiPlanar ? cPlane + (y / 2) * m_stride : nullptr;
        quint8 *u = m_semiPlanar ? nullptr : cPlane + (y / 2) * (m_stride / 2);
        quint8 *v = m_semiPlanar ? nullptr : vPlane + (y / 2) * (m_stride / 2);
        for (int x = 0; x < w; x += 2) {
            const QRgb a = r0[x], b = r0[x + 1], c = r1[x], d = r1[x + 1];
            const int Ra = (a >> 16) & 0xff, Ga = (a >> 8) & 0xff, Ba = a & 0xff;
            const int Rb = (b >> 16) & 0xff, Gb = (b >> 8) & 0xff, Bb = b & 0xff;
            const int Rc = (c >> 16) & 0xff, Gc = (c >> 8) & 0xff, Bc = c & 0xff;
            const int Rd = (d >> 16) & 0xff, Gd = (d >> 8) & 0xff, Bd = d & 0xff;
            y0[x] = quint8(((66 * Ra + 129 * Ga + 25 * Ba + 128) >> 8) + 16);
            y0[x + 1] = quint8(((66 * Rb + 129 * Gb + 25 * Bb + 128) >> 8) + 16);
            y1[x] = quint8(((66 * Rc + 129 * Gc + 25 * Bc + 128) >> 8) + 16);
            y1[x + 1] = quint8(((66 * Rd + 129 * Gd + 25 * Bd + 128) >> 8) + 16);
            const int R = Ra + Rb + Rc + Rd, G = Ga + Gb + Gc + Gd, B = Ba + Bb + Bc + Bd;
            const quint8 U = quint8(((-38 * R - 74 * G + 112 * B + 512) >> 10) + 128);
            const quint8 V = quint8(((112 * R - 94 * G - 18 * B + 512) >> 10) + 128);
            if (uv) { uv[x] = U; uv[x + 1] = V; }
            else { u[x / 2] = U; v[x / 2] = V; }
        }
    }
}

void DroidMediaEncoder::encode(const QImage &frame, quint64 timestampUs)
{
    if (!m_codec) return;
    const size_t size = size_t(m_stride) * size_t(m_sliceHeight) * 3 / 2;
    quint8 *buf = static_cast<quint8 *>(std::calloc(1, size)); // freed by the codec via unref
    if (!buf) return;
    toYuv(frame, buf);
    DmCodecData d{};
    d.data.data = buf;
    d.data.size = ssize_t(size);
    d.ts = qint64(timestampUs);
    d.sync = false;
    DmBufferCallbacks cb{freeBuffer, buf};
    m_inFlight.ref();
    api().queue(m_codec, &d, &cb);
}

void DroidMediaEncoder::onData(const void *data, long size, qint64 ts, bool config)
{
    // codec thread: copy, hand over to the Qt thread
    emit dataFromCodec(QByteArray(static_cast<const char *>(data), int(size)), quint64(ts), config);
}

void DroidMediaEncoder::onError(int err)
{
    emit errorFromCodec(err);
}

void DroidMediaEncoder::onInputDone() {}
