#include "audiorouter.h"

#include <QMetaObject>
#include <QVariant>
#include <QThread>
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>

namespace {

const char kSinkName[] = "sailauto_car";

class CaptureThread : public QThread
{
public:
    explicit CaptureThread(AudioRouter *r) : m_r(r) {}
protected:
    void run() override { m_r->runCapture(); }
private:
    AudioRouter *m_r;
};

struct Ctx { AudioRouter *self; pa_threaded_mainloop *loop; };

void contextState(pa_context *, void *userdata)
{
    pa_threaded_mainloop_signal(static_cast<pa_threaded_mainloop *>(userdata), 0);
}

void signalLoop(pa_threaded_mainloop *loop) { pa_threaded_mainloop_signal(loop, 0); }

} // namespace

AudioRouter::AudioRouter(QObject *parent) : QObject(parent) {}

AudioRouter::~AudioRouter()
{
    stop();
}

void AudioRouter::paLog(const QString &line)
{
    QMetaObject::invokeMethod(this, "log", Qt::QueuedConnection, Q_ARG(QString, line));
}

bool AudioRouter::waitOperation(pa_operation *op)
{
    if (!op) return false;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(m_loop);
    pa_operation_unref(op);
    return true;
}

bool AudioRouter::start(QString *error)
{
    if (m_running) return true;
    m_loop = pa_threaded_mainloop_new();
    m_ctx = pa_context_new(pa_threaded_mainloop_get_api(m_loop), "SailAuto");
    pa_context_set_state_callback(m_ctx, contextState, m_loop);
    if (pa_context_connect(m_ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0
        || pa_threaded_mainloop_start(m_loop) < 0) {
        if (error) *error = QStringLiteral("PulseAudio: %1").arg(QString::fromUtf8(pa_strerror(pa_context_errno(m_ctx))));
        stop();
        return false;
    }

    pa_threaded_mainloop_lock(m_loop);
    for (;;) {
        const pa_context_state_t st = pa_context_get_state(m_ctx);
        if (st == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(st)) {
            if (error) *error = QStringLiteral("PulseAudio nicht erreichbar: %1")
                                    .arg(QString::fromUtf8(pa_strerror(pa_context_errno(m_ctx))));
            pa_threaded_mainloop_unlock(m_loop);
            stop();
            return false;
        }
        pa_threaded_mainloop_wait(m_loop);
    }

    // 0. a leftover "sailauto_car" from a crash: unload it first
    struct ModListCb { static void cb(pa_context *c, const pa_module_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<AudioRouter *, pa_threaded_mainloop *> *>(u);
        if (eol) { signalLoop(s->second); return; }
        if (i && i->name && qstrcmp(i->name, "module-null-sink") == 0 && i->argument
            && QByteArray(i->argument).contains(QByteArray("sink_name=") + kSinkName)) {
            s->first->paLog(QStringLiteral("Ton: alter SailAuto-Ausgang (Modul %1) wird entfernt").arg(i->index));
            pa_operation *op = pa_context_unload_module(c, i->index, nullptr, nullptr);
            if (op) pa_operation_unref(op);
        }
    } };
    QPair<AudioRouter *, pa_threaded_mainloop *> pair(this, m_loop);
    waitOperation(pa_context_get_module_info_list(m_ctx, ModListCb::cb, &pair));

    // 1. remember the current default sink
    struct ServerCb { static void cb(pa_context *, const pa_server_info *i, void *u) {
        auto *s = static_cast<QPair<AudioRouter *, pa_threaded_mainloop *> *>(u);
        if (i && i->default_sink_name) s->first->setProperty("_oldDefault", QString::fromUtf8(i->default_sink_name));
        signalLoop(s->second);
    } };
    waitOperation(pa_context_get_server_info(m_ctx, ServerCb::cb, &pair));
    m_oldDefault = property("_oldDefault").toString();
    if (m_oldDefault == QLatin1String(kSinkName)) m_oldDefault.clear(); // leftover from a crash

    // 2. our virtual output (unload a leftover one first)
    struct ModCb { static void cb(pa_context *, uint32_t idx, void *u) {
        auto *s = static_cast<QPair<AudioRouter *, pa_threaded_mainloop *> *>(u);
        s->first->setProperty("_module", idx);
        signalLoop(s->second);
    } };
    const QByteArray args = QByteArrayLiteral("sink_name=") + kSinkName
                            + " sink_properties=device.description=SailAuto-Auto rate=48000 channels=2 format=s16le";
    setProperty("_module", 0xffffffffu);
    waitOperation(pa_context_load_module(m_ctx, "module-null-sink", args.constData(), ModCb::cb, &pair));
    m_module = property("_module").toUInt();
    if (m_module == PA_INVALID_INDEX) {
        if (error) *error = QStringLiteral("module-null-sink nicht ladbar: %1")
                                .arg(QString::fromUtf8(pa_strerror(pa_context_errno(m_ctx))));
        pa_threaded_mainloop_unlock(m_loop);
        stop();
        return false;
    }

    struct SinkCb { static void cb(pa_context *, const pa_sink_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<AudioRouter *, pa_threaded_mainloop *> *>(u);
        if (!eol && i) s->first->setProperty("_carSink", i->index);
        signalLoop(s->second);
    } };
    waitOperation(pa_context_get_sink_info_by_name(m_ctx, kSinkName, SinkCb::cb, &pair));
    m_carSink = property("_carSink").toUInt();

    // 3. new streams go to the car by default
    waitOperation(pa_context_set_default_sink(m_ctx, kSinkName, [](pa_context *, int, void *u) {
        signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop));

    // 4. watch streams; move the existing ones
    pa_context_set_subscribe_callback(m_ctx, [](pa_context *, pa_subscription_event_type_t t, uint32_t idx, void *u) {
        static_cast<AudioRouter *>(u)->onSubscription(t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK,
                                                      t & PA_SUBSCRIPTION_EVENT_TYPE_MASK, idx);
    }, this);
    pa_operation_unref(pa_context_subscribe(m_ctx, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr));

    struct ListCb { static void cb(pa_context *, const pa_sink_input_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<AudioRouter *, pa_threaded_mainloop *> *>(u);
        if (!eol && i) {
            const char *app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
            const char *role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE);
            s->first->onSinkInput(i->index, i->sink, QString::fromUtf8(i->name ? i->name : ""),
                                  QString::fromUtf8(app ? app : ""), QString::fromUtf8(role ? role : ""), true);
        }
        if (eol) signalLoop(s->second);
    } };
    waitOperation(pa_context_get_sink_input_info_list(m_ctx, ListCb::cb, &pair));
    pa_threaded_mainloop_unlock(m_loop);

    m_running = true;
    emit log(QStringLiteral("Ton: virtueller Ausgang \"SailAuto-Auto\" aktiv (vorher Standard: %1)").arg(m_oldDefault));

    // 5. record its monitor
    m_stopCapture = false;
    m_capture = new CaptureThread(this);
    m_capture->start();
    return true;
}

void AudioRouter::onSinkInput(uint32_t index, uint32_t sink, const QString &name, const QString &app,
                              const QString &role, bool isNew)
{
    // PA thread, mainloop locked.
    if (app == QLatin1String("SailAuto")) return; // our own recorder/test streams
    if (sink == m_carSink) {
        if (!m_seen.contains(index)) {
            m_seen.insert(index);
            paLog(QStringLiteral("Ton: \"%1\" (%2, Rolle %3) spielt im Auto")
                      .arg(app, name, role.isEmpty() ? QStringLiteral("-") : role));
        }
        return;
    }
    m_seen.insert(index);
    if (!m_origSink.contains(index)) m_origSink[index] = sink;
    isNew = !m_moveCount.contains(index);
    const int n = ++m_moveCount[index];
    if (n > 3) {
        if (n == 4)
            paLog(QStringLiteral("Ton: \"%1\" (%2, Rolle %3) wird vom System immer wieder zurückgeholt, gebe auf")
                      .arg(app, name, role));
        return;
    }
    paLog(QStringLiteral("Ton: %1 \"%2\" (%3, Rolle %4) -> Auto")
              .arg(isNew ? QStringLiteral("leite") : QStringLiteral("erneut"), app, name, role.isEmpty() ? QStringLiteral("-") : role));
    moveToCar(index);
}

void AudioRouter::moveToCar(uint32_t index)
{
    pa_operation *op = pa_context_move_sink_input_by_index(m_ctx, index, m_carSink, [](pa_context *c, int ok, void *u) {
        if (!ok) static_cast<AudioRouter *>(u)->paLog(QStringLiteral("Ton: Umleiten abgelehnt: %1")
                                                          .arg(QString::fromUtf8(pa_strerror(pa_context_errno(c)))));
    }, this);
    if (op) pa_operation_unref(op);
}

void AudioRouter::onSubscription(int facility, int type, uint32_t index)
{
    if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT) return;
    if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
        m_origSink.remove(index);
        m_moveCount.remove(index);
        m_seen.remove(index);
        return;
    }
    // NEW or CHANGE (e.g. the policy moved it): look where it is now.
    const bool isNew = type == PA_SUBSCRIPTION_EVENT_NEW;
    struct InfoCb { static void cb(pa_context *, const pa_sink_input_info *i, int eol, void *u) {
        if (eol || !i) return;
        auto *self = static_cast<AudioRouter *>(u);
        const char *app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
        const char *role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE);
        self->onSinkInput(i->index, i->sink, QString::fromUtf8(i->name ? i->name : ""),
                          QString::fromUtf8(app ? app : ""), QString::fromUtf8(role ? role : ""), false);
    } };
    Q_UNUSED(isNew)
    pa_operation *op = pa_context_get_sink_input_info(m_ctx, index, InfoCb::cb, this);
    if (op) pa_operation_unref(op);
}

void AudioRouter::runCapture()
{
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = Rate;
    ss.channels = Channels;
    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = Rate / 50 * Channels * 2; // 20 ms
    int err = 0;
    const QByteArray monitor = QByteArray(kSinkName) + ".monitor";
    pa_simple *s = pa_simple_new(nullptr, "SailAuto", PA_STREAM_RECORD, monitor.constData(), "Auto-Ton",
                                 &ss, nullptr, &attr, &err);
    if (!s) {
        paLog(QStringLiteral("Ton: Aufnahme von %1 nicht möglich: %2").arg(QString::fromLatin1(monitor),
                                                                          QString::fromUtf8(pa_strerror(err))));
        return;
    }
    QByteArray buf(static_cast<int>(attr.fragsize), Qt::Uninitialized);
    while (!m_stopCapture) {
        if (pa_simple_read(s, buf.data(), static_cast<size_t>(buf.size()), &err) < 0) {
            paLog(QStringLiteral("Ton: Aufnahmefehler: %1").arg(QString::fromUtf8(pa_strerror(err))));
            break;
        }
        QMetaObject::invokeMethod(this, "chunk", Qt::QueuedConnection, Q_ARG(QByteArray, buf));
    }
    pa_simple_free(s);
}

void AudioRouter::restoreAll()
{
    // Mainloop locked. Everything still on our sink goes back.
    if (!m_oldDefault.isEmpty()) {
        const QByteArray old = m_oldDefault.toUtf8();
        waitOperation(pa_context_set_default_sink(m_ctx, old.constData(), [](pa_context *, int, void *u) {
            signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop));
    }
    struct BackCb { static void cb(pa_context *c, const pa_sink_input_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<AudioRouter *, pa_threaded_mainloop *> *>(u);
        if (eol) { signalLoop(s->second); return; }
        if (!i || i->sink != s->first->property("_carSink").toUInt()) return;
        const QByteArray old = s->first->property("_oldDefault").toString().toUtf8();
        pa_operation *op = pa_context_move_sink_input_by_name(c, i->index, old.constData(), nullptr, nullptr);
        if (op) pa_operation_unref(op);
    } };
    QPair<AudioRouter *, pa_threaded_mainloop *> pair(this, m_loop);
    if (!m_oldDefault.isEmpty())
        waitOperation(pa_context_get_sink_input_info_list(m_ctx, BackCb::cb, &pair));
    if (m_module != PA_INVALID_INDEX) {
        // Unloading also moves remaining streams to the default sink.
        waitOperation(pa_context_unload_module(m_ctx, m_module, [](pa_context *, int, void *u) {
            signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop));
        m_module = PA_INVALID_INDEX;
    }
}

void AudioRouter::stop()
{
    if (m_capture) {
        m_stopCapture = true;
        m_capture->wait(1000);
        if (m_capture->isRunning()) { m_capture->terminate(); m_capture->wait(500); }
        delete m_capture;
        m_capture = nullptr;
    }
    if (m_loop) {
        pa_threaded_mainloop_lock(m_loop);
        if (m_ctx && pa_context_get_state(m_ctx) == PA_CONTEXT_READY) {
            pa_context_set_subscribe_callback(m_ctx, nullptr, nullptr);
            restoreAll();
        }
        if (m_ctx) {
            pa_context_disconnect(m_ctx);
            pa_context_unref(m_ctx);
            m_ctx = nullptr;
        }
        pa_threaded_mainloop_unlock(m_loop);
        pa_threaded_mainloop_stop(m_loop);
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
    }
    if (m_running) emit log(QStringLiteral("Ton: zurück aufs Telefon (%1)").arg(m_oldDefault));
    m_running = false;
    m_origSink.clear();
    m_moveCount.clear();
    m_seen.clear();
}
