#include "audiorouter.h"

#include <QMetaObject>
#include <QVariant>
#include <pulse/pulseaudio.h>
#include <pulse/error.h>

namespace {

const char kSinkName[] = "sailauto_car";

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
        if (!eol && i) {
            s->first->setProperty("_carSink", i->index);
            s->first->setProperty("_carMonitor", i->monitor_source);
        }
        signalLoop(s->second);
    } };
    waitOperation(pa_context_get_sink_info_by_name(m_ctx, kSinkName, SinkCb::cb, &pair));
    m_carSink = property("_carSink").toUInt();
    m_carMonitor = property("_carMonitor").toUInt();

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

    // Diagnosis: what the sound system looks like right now.
    dumpState(QStringLiteral("nach Umleitung"));
    m_running = true;
    emit log(QStringLiteral("Ton: virtueller Ausgang \"SailAuto-Auto\" aktiv (vorher Standard: %1)").arg(m_oldDefault));

    // 5. record its monitor -- and make sure it really is the monitor.
    // On Sailfish the audio policy may route recordings to the microphone,
    // which would feed the car's speakers back into the car (howling).
    if (!startCapture(error)) {
        stop();
        return false;
    }
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

bool AudioRouter::startCapture(QString *error)
{
    pa_threaded_mainloop_lock(m_loop);
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = Rate;
    ss.channels = Channels;
    pa_proplist *pl = pa_proplist_new();
    pa_proplist_sets(pl, PA_PROP_APPLICATION_NAME, "SailAuto");
    pa_proplist_sets(pl, PA_PROP_MEDIA_NAME, "Auto-Ton (Monitor)");
    m_stream = pa_stream_new_with_proplist(m_ctx, "Auto-Ton", &ss, nullptr, pl);
    pa_proplist_free(pl);
    pa_stream_set_state_callback(m_stream, [](pa_stream *, void *u) {
        signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop);
    pa_stream_set_read_callback(m_stream, [](pa_stream *st, size_t, void *u) {
        static_cast<AudioRouter *>(u)->onReadable(st); }, this);
    // If the policy moves us anyway: stop at once, never forward.
    pa_stream_set_moved_callback(m_stream, [](pa_stream *st, void *u) {
        auto *self = static_cast<AudioRouter *>(u);
        self->checkSource(pa_stream_get_device_index(st), QStringLiteral("verschoben"));
    }, this);

    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength = static_cast<uint32_t>(-1);
    attr.prebuf = static_cast<uint32_t>(-1);
    attr.minreq = static_cast<uint32_t>(-1);
    attr.fragsize = Rate / 50 * Channels * 2; // 20 ms
    const QByteArray monitor = QByteArray(kSinkName) + ".monitor";
    const int flags = PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY;
    if (pa_stream_connect_record(m_stream, monitor.constData(), &attr, static_cast<pa_stream_flags_t>(flags)) < 0) {
        if (error) *error = QStringLiteral("Aufnahme nicht startbar: %1").arg(QString::fromUtf8(pa_strerror(pa_context_errno(m_ctx))));
        pa_threaded_mainloop_unlock(m_loop);
        return false;
    }
    for (;;) {
        const pa_stream_state_t st = pa_stream_get_state(m_stream);
        if (st == PA_STREAM_READY) break;
        if (!PA_STREAM_IS_GOOD(st)) {
            if (error) *error = QStringLiteral("Aufnahme abgelehnt: %1").arg(QString::fromUtf8(pa_strerror(pa_context_errno(m_ctx))));
            pa_threaded_mainloop_unlock(m_loop);
            return false;
        }
        pa_threaded_mainloop_wait(m_loop);
    }
    const uint32_t src = pa_stream_get_device_index(m_stream);
    const QString name = QString::fromUtf8(pa_stream_get_device_name(m_stream));
    pa_threaded_mainloop_unlock(m_loop);
    if (src != m_carMonitor) {
        if (error) *error = QStringLiteral("FEHLER: Aufnahme kommt nicht vom SailAuto-Ausgang, sondern von \"%1\" "
                                           "(Quelle #%2, erwartet #%3) - vermutlich Mikrofon. Abgebrochen, sonst "
                                           "Rückkopplung.").arg(name).arg(src).arg(m_carMonitor);
        return false;
    }
    m_captureOk = true;
    emit log(QStringLiteral("Ton: Aufnahme geprüft: kommt von \"%1\" (Quelle #%2)").arg(name).arg(src));
    return true;
}

void AudioRouter::checkSource(uint32_t source, const QString &why)
{
    // PA thread.
    if (source == m_carMonitor) return;
    m_captureOk = false;
    paLog(QStringLiteral("FEHLER: Aufnahme wurde %1 auf Quelle #%2 (nicht SailAuto-Ausgang #%3). Ton ans Auto "
                         "gesperrt, sonst Rückkopplung.").arg(why).arg(source).arg(m_carMonitor));
}

void AudioRouter::onReadable(pa_stream *st)
{
    // PA thread.
    const void *data = nullptr;
    size_t n = 0;
    while (pa_stream_readable_size(st) > 0) {
        if (pa_stream_peek(st, &data, &n) < 0) return;
        if (n == 0) break;
        if (data && m_captureOk) m_pending.append(static_cast<const char *>(data), static_cast<int>(n));
        else if (!data && m_captureOk) m_pending.append(QByteArray(static_cast<int>(n), 0)); // hole = silence
        pa_stream_drop(st);
    }
    const int chunkBytes = Rate / 50 * Channels * 2;
    while (m_pending.size() >= chunkBytes) {
        const QByteArray c = m_pending.left(chunkBytes);
        m_pending.remove(0, chunkBytes);
        QMetaObject::invokeMethod(this, "chunk", Qt::QueuedConnection, Q_ARG(QByteArray, c));
    }
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

void AudioRouter::dumpState(const QString &when)
{
    // Mainloop must be locked by the caller.
    struct SinkList { static void cb(pa_context *, const pa_sink_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<QStringList *, pa_threaded_mainloop *> *>(u);
        if (eol) { signalLoop(s->second); return; }
        static const char *st[] = {"läuft", "leer", "pausiert"};
        const char *role = pa_proplist_gets(i->proplist, "device.class");
        s->first->append(QStringLiteral("  Ausgang #%1 %2 (%3) %4 %5Hz/%6ch %7")
                             .arg(i->index).arg(QString::fromUtf8(i->name), QString::fromUtf8(i->description))
                             .arg(QString::fromUtf8(i->state >= 0 && i->state <= 2 ? st[i->state] : "?"))
                             .arg(i->sample_spec.rate).arg(i->sample_spec.channels)
                             .arg(QString::fromUtf8(role ? role : "")));
    } };
    struct InputList { static void cb(pa_context *, const pa_sink_input_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<QStringList *, pa_threaded_mainloop *> *>(u);
        if (eol) { signalLoop(s->second); return; }
        const char *app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
        const char *bin = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
        const char *role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE);
        s->first->append(QStringLiteral("  Wiedergabe #%1 \"%2\" (%3) Rolle=%4 -> Ausgang #%5%6")
                             .arg(i->index).arg(QString::fromUtf8(app ? app : "?"), QString::fromUtf8(bin ? bin : "?"),
                                                QString::fromUtf8(role ? role : "-"))
                             .arg(i->sink).arg(i->corked ? QStringLiteral(" (pausiert)") : QString()));
    } };
    struct OutputList { static void cb(pa_context *, const pa_source_output_info *i, int eol, void *u) {
        auto *s = static_cast<QPair<QStringList *, pa_threaded_mainloop *> *>(u);
        if (eol) { signalLoop(s->second); return; }
        const char *app = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
        s->first->append(QStringLiteral("  Aufnahme #%1 \"%2\" <- Quelle #%3").arg(i->index)
                             .arg(QString::fromUtf8(app ? app : "?")).arg(i->source));
    } };
    QStringList lines;
    QPair<QStringList *, pa_threaded_mainloop *> pair(&lines, m_loop);
    waitOperation(pa_context_get_sink_info_list(m_ctx, SinkList::cb, &pair));
    waitOperation(pa_context_get_sink_input_info_list(m_ctx, InputList::cb, &pair));
    waitOperation(pa_context_get_source_output_info_list(m_ctx, OutputList::cb, &pair));
    if (lines.isEmpty()) lines << QStringLiteral("  (nichts)");
    paLog(QStringLiteral("Ton-Diagnose %1 (Auto-Ausgang #%2):\n%3").arg(when).arg(m_carSink).arg(lines.join(QLatin1Char('\n'))));
}

void AudioRouter::dumpNow(const QString &when)
{
    if (!m_loop || !m_running) return;
    pa_threaded_mainloop_lock(m_loop);
    dumpState(when);
    pa_threaded_mainloop_unlock(m_loop);
}

void AudioRouter::stop()
{
    m_captureOk = false;
    if (m_loop) {
        pa_threaded_mainloop_lock(m_loop);
        if (m_stream) {
            pa_stream_set_read_callback(m_stream, nullptr, nullptr);
            pa_stream_set_moved_callback(m_stream, nullptr, nullptr);
            pa_stream_set_state_callback(m_stream, nullptr, nullptr);
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }
        m_pending.clear();
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
