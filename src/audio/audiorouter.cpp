#include "audiorouter.h"

#include <QMetaObject>
#include <QVariant>
#include <QFile>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
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

AudioRouter::AudioRouter(QObject *parent) : QObject(parent)
{
    m_readTimer.setInterval(10);
    connect(&m_readTimer, &QTimer::timeout, this, &AudioRouter::readPipe);
}

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
        if (i && i->name && (qstrcmp(i->name, "module-null-sink") == 0 || qstrcmp(i->name, "module-pipe-sink") == 0)
            && i->argument
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
    // A pipe sink: PulseAudio writes the mixed sound into a FIFO that we read.
    // No recording stream is involved, so Sailfish's policy cannot reroute it
    // to the microphone (which happened with a null sink + monitor recording).
    QString dir = QString::fromLocal8Bit(qgetenv("XDG_RUNTIME_DIR"));
    if (dir.isEmpty()) dir = QStringLiteral("/tmp");
    m_fifoPath = dir + QStringLiteral("/sailauto-car.pcm");
    QFile::remove(m_fifoPath);
    const QByteArray base = QByteArrayLiteral("sink_name=") + kSinkName + " file=" + m_fifoPath.toLocal8Bit()
                            + " format=s16le rate=48000 channels=2 sink_properties=device.description=SailAuto-Auto";
    setProperty("_module", 0xffffffffu);
    // Newer PulseAudio paces the pipe in real time itself; older ones need us to.
    waitOperation(pa_context_load_module(m_ctx, "module-pipe-sink", (base + " use_system_clock_for_timing=yes").constData(),
                                         ModCb::cb, &pair));
    m_module = property("_module").toUInt();
    m_selfPaced = m_module != PA_INVALID_INDEX;
    if (m_module == PA_INVALID_INDEX) {
        waitOperation(pa_context_load_module(m_ctx, "module-pipe-sink", base.constData(), ModCb::cb, &pair));
        m_module = property("_module").toUInt();
    }
    if (m_module == PA_INVALID_INDEX) {
        if (error) *error = QStringLiteral("module-pipe-sink nicht ladbar: %1")
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

    // 5. read the pipe
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
    m_fd = ::open(m_fifoPath.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (m_fd < 0) {
        if (error) *error = QStringLiteral("Ton-Pipe %1 nicht lesbar: %2").arg(m_fifoPath, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    m_readClock.start();
    m_bytesOut = 0;
    m_filling = false;
    m_readTimer.start();
    emit log(QStringLiteral("Ton: lese Pipe %1 (%2)").arg(m_fifoPath)
                 .arg(m_selfPaced ? QStringLiteral("PulseAudio taktet") : QStringLiteral("App taktet")));
    return true;
}

void AudioRouter::readPipe()
{
    if (m_fd < 0) return;
    const int frameBytes = Channels * 2;
    const int chunkBytes = Rate / 50 * frameBytes; // 20 ms
    auto msToBytes = [frameBytes](qint64 ms) { return ms * Rate / 1000 * frameBytes; };
    // Where the stream should be by now (wall clock).
    const qint64 due = m_readClock.nsecsElapsed() / 1000 * Rate / 1000000 * frameBytes;
    qint64 budget = 1 << 20;
    if (!m_selfPaced) {
        // We are the clock: never read faster than real time (+40 ms slack).
        budget = due + msToBytes(40) - m_bytesOut;
        if (budget <= 0) return;
    }
    char buf[16384];
    bool gotData = false;
    while (budget > 0) {
        const ssize_t n = ::read(m_fd, buf, static_cast<size_t>(qMin<qint64>(budget, sizeof(buf))));
        if (n <= 0) break;
        gotData = true;
        budget -= n;
        // PulseAudio catching up after a gap we already filled: drop what is
        // too far ahead, else the delay to the car grows with every pause.
        if (m_bytesOut + n > due + msToBytes(200)) continue;
        m_pending.append(buf, static_cast<int>(n));
        m_bytesOut += n;
    }
    // Nothing from PulseAudio (player paused, sink suspended): keep the car's
    // stream running with silence, so it never starves and the 3 s silence
    // rule can hand the speakers back.
    const qint64 lag = due - m_bytesOut;
    if (lag > msToBytes(100)) {
        const qint64 fill = lag - lag % frameBytes;
        m_pending.append(QByteArray(static_cast<int>(fill), '\0'));
        m_bytesOut += fill;
        if (!m_filling) {
            m_filling = true;
            emit log(QStringLiteral("Ton: Pipe liefert nichts, fülle mit Stille"));
        }
    } else if (m_filling && gotData) {
        m_filling = false; // PulseAudio delivers again; log the next gap once more
    }
    while (m_pending.size() >= chunkBytes) {
        emit chunk(m_pending.left(chunkBytes));
        m_pending.remove(0, chunkBytes);
    }
}

QHash<uint32_t, QString> AudioRouter::listSinks()
{
    struct Ctx { QHash<uint32_t, QString> sinks; pa_threaded_mainloop *loop; };
    Ctx ctx{{}, m_loop};
    waitOperation(pa_context_get_sink_info_list(m_ctx, [](pa_context *, const pa_sink_info *i, int eol, void *u) {
        auto *c = static_cast<Ctx *>(u);
        if (eol) { signalLoop(c->loop); return; }
        if (i) c->sinks.insert(i->index, QString::fromUtf8(i->name));
    }, &ctx));
    return ctx.sinks;
}

uint32_t AudioRouter::realSink(const QHash<uint32_t, QString> &sinks) const
{
    // Sailfish: the default sink is the placeholder "sink.null" (silent); the
    // sound really comes out of the droid sinks.
    for (auto it = sinks.constBegin(); it != sinks.constEnd(); ++it)
        if (it.value() == QLatin1String("sink.primary_output")) return it.key();
    for (auto it = sinks.constBegin(); it != sinks.constEnd(); ++it)
        if (it.value() != QLatin1String(kSinkName) && it.value() != QLatin1String("sink.null")
            && !it.value().endsWith(QLatin1String(".monitor"))) return it.key();
    return PA_INVALID_INDEX;
}

int AudioRouter::moveStuckStreams(const QHash<uint32_t, QString> &sinks, uint32_t target)
{
    // Mainloop locked. Everything on our sink or on sink.null goes to the
    // sink it came from (if known and real), else to target.
    if (target == PA_INVALID_INDEX) return 0;
    uint32_t nullSink = PA_INVALID_INDEX;
    for (auto it = sinks.constBegin(); it != sinks.constEnd(); ++it)
        if (it.value() == QLatin1String("sink.null")) nullSink = it.key();
    struct Ctx { QList<QPair<uint32_t, uint32_t>> inputs; pa_threaded_mainloop *loop; }; // input, sink
    Ctx ctx{{}, m_loop};
    waitOperation(pa_context_get_sink_input_info_list(m_ctx, [](pa_context *, const pa_sink_input_info *i, int eol, void *u) {
        auto *c = static_cast<Ctx *>(u);
        if (eol) { signalLoop(c->loop); return; }
        if (i) c->inputs.append(qMakePair(i->index, i->sink));
    }, &ctx));
    int moved = 0;
    for (const auto &in : ctx.inputs) {
        if (in.second != m_carSink && in.second != nullSink) continue;
        uint32_t to = m_origSink.value(in.first, PA_INVALID_INDEX);
        if (!sinks.contains(to) || to == nullSink || to == m_carSink) to = target;
        waitOperation(pa_context_move_sink_input_by_index(m_ctx, in.first, to, [](pa_context *, int, void *u) {
            signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop));
        ++moved;
    }
    return moved;
}

void AudioRouter::restoreAll()
{
    // Mainloop locked. Default back first, so nothing new lands on our sink.
    if (!m_oldDefault.isEmpty()) {
        const QByteArray old = m_oldDefault.toUtf8();
        waitOperation(pa_context_set_default_sink(m_ctx, old.constData(), [](pa_context *, int, void *u) {
            signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop));
    }
    // Streams back to where they were, never onto the silent sink.null
    // (0.8.1 and older did that: phone muted after SailAuto ended).
    const QHash<uint32_t, QString> sinks = listSinks();
    const uint32_t target = realSink(sinks);
    const int moved = moveStuckStreams(sinks, target);
    if (moved)
        paLog(QStringLiteral("Ton: %1 Wiedergabe(n) zurück auf %2").arg(moved).arg(sinks.value(target)));
    if (m_module != PA_INVALID_INDEX) {
        waitOperation(pa_context_unload_module(m_ctx, m_module, [](pa_context *, int, void *u) {
            signalLoop(static_cast<pa_threaded_mainloop *>(u)); }, m_loop));
        m_module = PA_INVALID_INDEX;
    }
    dumpState(QStringLiteral("nach Rückstellung"));
}

int AudioRouter::repairSilentStreams()
{
    if (m_running) return 0;
    pa_threaded_mainloop *loop = pa_threaded_mainloop_new();
    pa_context *ctx = pa_context_new(pa_threaded_mainloop_get_api(loop), "SailAuto");
    pa_context_set_state_callback(ctx, contextState, loop);
    int moved = -1;
    if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0 && pa_threaded_mainloop_start(loop) >= 0) {
        pa_threaded_mainloop_lock(loop);
        for (;;) {
            const pa_context_state_t st = pa_context_get_state(ctx);
            if (st == PA_CONTEXT_READY || !PA_CONTEXT_IS_GOOD(st)) break;
            pa_threaded_mainloop_wait(loop);
        }
        if (pa_context_get_state(ctx) == PA_CONTEXT_READY) {
            m_loop = loop;
            m_ctx = ctx;
            const QHash<uint32_t, QString> sinks = listSinks();
            const uint32_t target = realSink(sinks);
            // A leftover SailAuto sink (crash) counts as stuck too.
            m_carSink = PA_INVALID_INDEX;
            for (auto it = sinks.constBegin(); it != sinks.constEnd(); ++it)
                if (it.value() == QLatin1String(kSinkName)) m_carSink = it.key();
            moved = moveStuckStreams(sinks, target);
            if (moved > 0)
                paLog(QStringLiteral("Ton repariert: %1 stumme Wiedergabe(n) auf %2 verschoben").arg(moved).arg(sinks.value(target)));
            m_loop = nullptr;
            m_ctx = nullptr;
            m_carSink = PA_INVALID_INDEX;
        }
        pa_context_disconnect(ctx);
        pa_threaded_mainloop_unlock(loop);
        pa_threaded_mainloop_stop(loop);
    }
    pa_context_unref(ctx);
    pa_threaded_mainloop_free(loop);
    if (moved < 0) paLog(QStringLiteral("Ton reparieren: PulseAudio nicht erreichbar"));
    return moved;
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
    m_readTimer.stop();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_pending.clear();
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
    if (!m_fifoPath.isEmpty()) QFile::remove(m_fifoPath);
    if (m_running) emit log(QStringLiteral("Ton: zurück aufs Telefon (%1)").arg(m_oldDefault));
    m_running = false;
    m_origSink.clear();
    m_moveCount.clear();
    m_seen.clear();
}
