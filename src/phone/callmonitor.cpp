#include "callmonitor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QUrl>
#include <QVariantMap>

#ifdef HAVE_QTCONTACTS
#include <QContactAvatar>
#include <QContactDetailFilter>
#include <QContactFilter>
#include <QContactDisplayLabel>
#include <QContactManager>
#include <QContactName>
#include <QContactPhoneNumber>
QTCONTACTS_USE_NAMESPACE
#endif

namespace {
const char kService[] = "org.nemomobile.voicecall";
const char kManagerIface[] = "org.nemomobile.voicecall.VoiceCallManager";
const char kCallIface[] = "org.nemomobile.voicecall.VoiceCall";

// voicecall AbstractVoiceCallHandler::VoiceCallStatus
enum { StNull, StActive, StHeld, StDialing, StAlerting, StIncoming, StWaiting, StDisconnected, StRejected, StIgnored };

QVariantMap getAll(const QString &path, const char *iface, QString *error = nullptr)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), path,
                                                    QStringLiteral("org.freedesktop.DBus.Properties"),
                                                    QStringLiteral("GetAll"));
    m << QLatin1String(iface);
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 400);
    QVariantMap out;
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty())
        r.arguments().first().value<QDBusArgument>() >> out;
    else if (error)
        *error = r.errorMessage();
    return out;
}

bool callMethod(const QString &path, const char *iface, const char *method, QString *error)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), path, QLatin1String(iface),
                                                    QLatin1String(method));
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 2000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        *error = r.errorMessage();
        return false;
    }
    // answer()/hangup() return bool
    if (!r.arguments().isEmpty() && r.arguments().first().type() == QVariant::Bool && !r.arguments().first().toBool()) {
        *error = QStringLiteral("abgelehnt");
        return false;
    }
    return true;
}
} // namespace

CallMonitor::CallMonitor(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &CallMonitor::poll);
}

void CallMonitor::start()
{
    poll();
    m_timer.start();
}

void CallMonitor::stop()
{
    m_timer.stop();
}

void CallMonitor::poll()
{
    QString err;
    const QVariantMap mgr = getAll(QStringLiteral("/"), kManagerIface, &err);
    if (mgr.isEmpty()) {
        if (!m_serviceErrorLogged) {
            m_serviceErrorLogged = true;
            emit log(QStringLiteral("Anrufe: voicecall-Dienst nicht erreichbar (%1)").arg(err));
        }
    } else if (m_serviceErrorLogged) {
        m_serviceErrorLogged = false;
    }

    // Pick the call that matters most: ringing > active > outgoing > held.
    CallInfo best;
    int bestRank = 0;
    for (const QString &id : mgr.value(QStringLiteral("voiceCalls")).toStringList()) {
        const QString path = QStringLiteral("/calls/") + id;
        const QVariantMap c = getAll(path, kCallIface);
        const int st = c.value(QStringLiteral("status")).toInt();
        CallInfo ci;
        int rank = 0;
        switch (st) {
        case StIncoming: case StWaiting: ci.state = CallInfo::Incoming; rank = 4; break;
        case StActive: ci.state = CallInfo::Active; rank = 3; break;
        case StDialing: case StAlerting: ci.state = CallInfo::Outgoing; rank = 2; break;
        case StHeld: ci.state = CallInfo::Held; rank = 1; break;
        default: break;
        }
        if (rank <= bestRank) continue;
        ci.id = id;
        ci.number = c.value(QStringLiteral("lineId")).toString();
        ci.startedAt = c.value(QStringLiteral("startedAt")).toDateTime();
        best = ci;
        bestRank = rank;
    }
    if (best.state != CallInfo::None) {
        if (best.number == m_call.number && best.id == m_call.id) {
            best.name = m_call.name;
            best.photo = m_call.photo;
        } else {
            lookupContact(best.number, &best.name, &best.photo);
        }
        if (best.state == CallInfo::Active && !best.startedAt.isValid())
            best.startedAt = m_call.state == CallInfo::Active ? m_call.startedAt : QDateTime::currentDateTime();
    }
    if (!(best == m_call)) {
        m_call = best;
        emit changed();
    }
}

bool CallMonitor::answer()
{
    if (m_call.id.isEmpty()) return false;
    QString err;
    const bool ok = callMethod(QStringLiteral("/calls/") + m_call.id, kCallIface, "answer", &err);
    emit log(ok ? QStringLiteral("Anruf: angenommen") : QStringLiteral("Anruf: Annehmen fehlgeschlagen (%1)").arg(err));
    QTimer::singleShot(200, this, &CallMonitor::poll);
    return ok;
}

bool CallMonitor::hangup()
{
    if (m_call.id.isEmpty()) return false;
    QString err;
    const bool ok = callMethod(QStringLiteral("/calls/") + m_call.id, kCallIface, "hangup", &err);
    emit log(ok ? QStringLiteral("Anruf: aufgelegt/abgelehnt") : QStringLiteral("Anruf: Auflegen fehlgeschlagen (%1)").arg(err));
    QTimer::singleShot(200, this, &CallMonitor::poll);
    return ok;
}

bool CallMonitor::silenceRingtone()
{
    QString err;
    const bool ok = callMethod(QStringLiteral("/"), kManagerIface, "silenceRingtone", &err);
    emit log(ok ? QStringLiteral("Anruf: Klingelton stumm") : QStringLiteral("Anruf: Stumm fehlgeschlagen (%1)").arg(err));
    return ok;
}

void CallMonitor::lookupContact(const QString &number, QString *name, QImage *photo)
{
    if (number.isEmpty()) return;
    auto it = m_contactCache.constFind(number);
    if (it != m_contactCache.constEnd()) {
        *name = it->first;
        *photo = it->second;
        return;
    }
#ifdef HAVE_QTCONTACTS
    static QContactManager *mgr = nullptr;
    if (!mgr) mgr = new QContactManager(QStringLiteral("org.nemomobile.contacts.sqlite"));
    const QList<QContact> found = mgr->contacts(QContactPhoneNumber::match(number));
    if (mgr->error() != QContactManager::NoError && !m_contactErrorLogged) {
        m_contactErrorLogged = true;
        emit log(QStringLiteral("Anrufe: Adressbuch nicht lesbar (Fehler %1) – nur Nummer").arg(mgr->error()));
    }
    if (!found.isEmpty()) {
        const QContact &c = found.first();
        *name = c.detail<QContactDisplayLabel>().label();
        if (name->isEmpty()) {
            const QContactName n = c.detail<QContactName>();
            *name = (n.firstName() + QLatin1Char(' ') + n.lastName()).trimmed();
        }
        for (const QContactAvatar &a : c.details<QContactAvatar>()) {
            const QUrl u = a.imageUrl();
            QImage img(u.isLocalFile() ? u.toLocalFile() : u.toString());
            if (!img.isNull()) {
                *photo = img.scaled(256, 256, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                break;
            }
        }
    }
    emit log(QStringLiteral("Anrufe: Adressbuch %1%2")
                 .arg(found.isEmpty() ? QStringLiteral("kennt die Nummer nicht") : QStringLiteral("hat den Kontakt"))
                 .arg(photo->isNull() ? QString() : QStringLiteral(" (mit Bild)")));
#else
    if (!m_contactErrorLogged) {
        m_contactErrorLogged = true;
        emit log(QStringLiteral("Anrufe: ohne Adressbuch gebaut – nur Nummer"));
    }
#endif
    m_contactCache.insert(number, qMakePair(*name, *photo));
}
