#include "phonebook.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QProcess>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

#ifdef HAVE_QTCONTACTS
#include <QContactAddress>
#include <QContactAvatar>
#include <QContactDetailFilter>
#include <QContactDisplayLabel>
#include <QContactFavorite>
#include <QContactFetchHint>
#include <QContactManager>
#include <QContactName>
#include <QContactPhoneNumber>
QTCONTACTS_USE_NAMESPACE
#endif

namespace {
QImage smallPhoto(const QString &path)
{
    QImage img(path);
    if (img.isNull()) return img;
    return img.scaled(72, 72, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation).copy(0, 0, 72, 72);
}

#ifdef HAVE_QTCONTACTS
QString numberLabel(const QContactPhoneNumber &n)
{
    const QList<int> sub = n.subTypes();
    if (sub.contains(QContactPhoneNumber::SubTypeMobile)) return QStringLiteral("Mobil");
    if (sub.contains(QContactPhoneNumber::SubTypeFax)) return QStringLiteral("Fax");
    const QList<int> ctx = n.contexts();
    if (ctx.contains(QContactDetail::ContextWork)) return QStringLiteral("Arbeit");
    if (ctx.contains(QContactDetail::ContextHome)) return QStringLiteral("Privat");
    return QStringLiteral("Telefon");
}
#endif
} // namespace

namespace {
const char kHelper[] = "/usr/libexec/harbour-sailauto/contacts-helper";
}

PhoneBook::PhoneBook(QObject *parent) : QObject(parent) {}

QVector<PhoneContact> PhoneBook::fromJson(const QByteArray &json, QVector<RecentCall> *calls)
{
    QVector<PhoneContact> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonArray contacts = doc.isArray() ? doc.array() : doc.object().value(QStringLiteral("contacts")).toArray();
    if (calls) {
        calls->clear();
        for (const QJsonValue &v : doc.object().value(QStringLiteral("calls")).toArray()) {
            const QJsonObject o = v.toObject();
            RecentCall rc;
            rc.number = o.value(QStringLiteral("number")).toString();
            rc.time = static_cast<qint64>(o.value(QStringLiteral("time")).toDouble());
            rc.outgoing = o.value(QStringLiteral("out")).toBool();
            rc.missed = o.value(QStringLiteral("missed")).toBool();
            if (!rc.number.isEmpty()) calls->append(rc);
        }
    }
    for (const QJsonValue &v : contacts) {
        const QJsonObject o = v.toObject();
        PhoneContact c;
        c.name = o.value(QStringLiteral("name")).toString();
        c.favorite = o.value(QStringLiteral("favorite")).toBool();
        const QString photo = o.value(QStringLiteral("photo")).toString();
        if (!photo.isEmpty()) c.photo = smallPhoto(photo);
        const QString png = o.value(QStringLiteral("png")).toString();
        if (!png.isEmpty()) c.photo.loadFromData(QByteArray::fromBase64(png.toLatin1()), "PNG");
        for (const QJsonValue &n : o.value(QStringLiteral("numbers")).toArray()) {
            c.numbers << n.toObject().value(QStringLiteral("number")).toString();
            c.labels << n.toObject().value(QStringLiteral("label")).toString();
        }
        for (const QJsonValue &a : o.value(QStringLiteral("addresses")).toArray())
            c.addresses << a.toString();
        if (!c.numbers.isEmpty() || !c.addresses.isEmpty()) out.append(c);
    }
    return out;
}

QVector<RecentCall> PhoneBook::loadCalls(QString *error)
{
    // Sailfish call history (commhistory): Events, type 3 = call,
    // direction 1 in / 2 out. Same privileged folder rules as contacts.
    QVector<RecentCall> out;
    QString path = QString::fromLocal8Bit(qgetenv("SAILAUTO_COMMHISTORY_DB"));
    if (path.isEmpty()) path = QDir::homePath() + QStringLiteral("/.local/share/commhistory/commhistory.db");
    if (!QFile::exists(path)) {
        if (error) *error = QStringLiteral("Anrufliste nicht gefunden");
        return out;
    }
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("commhistory"));
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            if (error) *error = QStringLiteral("Anrufliste nicht lesbar: %1").arg(db.lastError().text());
        } else {
            QSqlQuery q(db);
            if (!q.exec(QStringLiteral("SELECT remoteUid, startTime, direction, isMissedCall FROM Events "
                                       "WHERE type = 3 ORDER BY startTime DESC LIMIT 60"))) {
                if (error) *error = QStringLiteral("Anrufliste: %1").arg(q.lastError().text());
            }
            while (q.next()) {
                RecentCall rc;
                rc.number = q.value(0).toString();
                rc.time = q.value(1).toLongLong();
                rc.outgoing = q.value(2).toInt() == 2;
                rc.missed = q.value(3).toInt() != 0;
                if (!rc.number.isEmpty()) out.append(rc);
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("commhistory"));
    return out;
}

QVector<PhoneContact> PhoneBook::loadDirect(QString *error)
{
    QVector<PhoneContact> out;
#ifdef HAVE_QTCONTACTS
    QContactManager mgr(QStringLiteral("org.nemomobile.contacts.sqlite"));
    QContactFetchHint hint;
    hint.setDetailTypesHint(QList<QContactDetail::DetailType>()
                            << QContactDisplayLabel::Type << QContactName::Type << QContactPhoneNumber::Type
                            << QContactAvatar::Type << QContactFavorite::Type << QContactAddress::Type);
    // Only contacts with a phone number (filter: detail type exists)
    QContactDetailFilter hasNumber;
    hasNumber.setDetailType(QContactPhoneNumber::Type);
    const QList<QContact> all = mgr.contacts(hasNumber, QList<QContactSortOrder>(), hint);
    if (mgr.error() != QContactManager::NoError && error)
        *error = QStringLiteral("Adressbuch nicht lesbar (Fehler %1)").arg(mgr.error());
    for (const QContact &k : all) {
        PhoneContact c;
        c.name = k.detail<QContactDisplayLabel>().label();
        if (c.name.isEmpty()) {
            const QContactName n = k.detail<QContactName>();
            c.name = (n.firstName() + QLatin1Char(' ') + n.lastName()).trimmed();
        }
        c.favorite = k.detail<QContactFavorite>().isFavorite();
        for (const QContactAddress &a : k.details<QContactAddress>()) {
            QStringList parts;
            for (const QString &x : {a.street(), (a.postcode() + QLatin1Char(' ') + a.locality()).trimmed(), a.country()})
                if (!x.trimmed().isEmpty()) parts << x.trimmed();
            if (!parts.isEmpty()) c.addresses << parts.join(QStringLiteral(", "));
        }
        for (const QContactPhoneNumber &n : k.details<QContactPhoneNumber>()) {
            if (n.number().trimmed().isEmpty() || c.numbers.contains(n.number())) continue;
            c.numbers << n.number();
            c.labels << numberLabel(n);
        }
        for (const QContactAvatar &a : k.details<QContactAvatar>()) {
            const QUrl u = a.imageUrl();
            c.photo = smallPhoto(u.isLocalFile() ? u.toLocalFile() : u.toString());
            if (!c.photo.isNull()) break;
        }
        if (!c.numbers.isEmpty() || !c.addresses.isEmpty()) out.append(c);
    }
#else
    if (error) *error = QStringLiteral("ohne Adressbuch gebaut");
#endif
    return out;
}

int PhoneBook::dumpMain(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QString error;
    const QVector<PhoneContact> list = loadDirect(&error);
    QJsonArray arr;
    for (const PhoneContact &c : list) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), c.name);
        o.insert(QStringLiteral("favorite"), c.favorite);
        QJsonArray nums;
        for (int i = 0; i < c.numbers.size(); ++i) {
            QJsonObject n;
            n.insert(QStringLiteral("number"), c.numbers[i]);
            n.insert(QStringLiteral("label"), c.labels.value(i));
            nums.append(n);
        }
        o.insert(QStringLiteral("numbers"), nums);
        if (!c.addresses.isEmpty()) o.insert(QStringLiteral("addresses"), QJsonArray::fromStringList(c.addresses));
        if (!c.photo.isNull()) {
            // The photo files are in the privileged folder too: send the pixels.
            QByteArray png;
            QBuffer buf(&png);
            buf.open(QIODevice::WriteOnly);
            c.photo.save(&buf, "PNG");
            o.insert(QStringLiteral("png"), QString::fromLatin1(png.toBase64()));
        }
        arr.append(o);
    }
    QFile out;
    out.open(stdout, QIODevice::WriteOnly);
    QString callError;
    QJsonArray calls;
    for (const RecentCall &rc : loadCalls(&callError)) {
        QJsonObject o;
        o.insert(QStringLiteral("number"), rc.number);
        o.insert(QStringLiteral("time"), static_cast<double>(rc.time));
        o.insert(QStringLiteral("out"), rc.outgoing);
        o.insert(QStringLiteral("missed"), rc.missed);
        calls.append(o);
    }
    if (!callError.isEmpty()) error += (error.isEmpty() ? QString() : QStringLiteral("; ")) + callError;
    QJsonObject root;
    root.insert(QStringLiteral("contacts"), arr);
    root.insert(QStringLiteral("calls"), calls);
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!error.isEmpty()) {
        QFile err;
        err.open(stderr, QIODevice::WriteOnly);
        err.write(error.toUtf8() + '\n');
        return 2;
    }
    return 0;
}

void PhoneBook::load()
{
    QElapsedTimer t;
    t.start();
    const QString fake = QString::fromLocal8Bit(qgetenv("SAILAUTO_FAKE_CONTACTS"));
    if (!fake.isEmpty()) {
        QFile f(fake);
        f.open(QIODevice::ReadOnly);
        finish(fromJson(f.readAll(), &m_calls), t.elapsed(), QStringLiteral("Testdatei"));
        return;
    }
    if (QFile::exists(QLatin1String(kHelper))) {
        if (m_helper) return; // still running
        m_helper = new QProcess(this);
        QElapsedTimer *clock = new QElapsedTimer(t);
        connect(m_helper, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
                [this, clock](int code, QProcess::ExitStatus) {
            const QByteArray json = m_helper->readAllStandardOutput();
            const QString err = QString::fromUtf8(m_helper->readAllStandardError()).trimmed();
            if (code != 0 || !err.isEmpty())
                emit log(QStringLiteral("Telefonbuch: Helfer meldet %1 (Code %2)").arg(err.isEmpty() ? QStringLiteral("-") : err).arg(code));
            QVector<RecentCall> calls;
            QVector<PhoneContact> list = fromJson(json, &calls);
            m_calls = calls;
            finish(list, clock->elapsed(), QStringLiteral("Helfer"));
            delete clock;
            m_helper->deleteLater();
            m_helper = nullptr;
        });
        m_helper->start(QLatin1String(kHelper), QStringList() << QStringLiteral("--dump-contacts"));
        return;
    }
    QString error;
    QVector<PhoneContact> list = loadDirect(&error);
    if (!error.isEmpty()) emit log(QStringLiteral("Telefonbuch: ") + error);
    QString callError;
    m_calls = loadCalls(&callError);
    if (!callError.isEmpty() && !qEnvironmentVariableIsSet("SAILAUTO_FAKE_CONTACTS"))
        emit log(QStringLiteral("Telefonbuch: ") + callError);
    finish(list, t.elapsed(), QStringLiteral("direkt"));
}

void PhoneBook::finish(QVector<PhoneContact> out, qint64 ms, const QString &source)
{
    std::sort(out.begin(), out.end(), [](const PhoneContact &a, const PhoneContact &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    m_contacts = out;
    int favs = 0, photos = 0;
    for (const PhoneContact &c : m_contacts) {
        favs += c.favorite;
        photos += !c.photo.isNull();
    }
    emit log(QStringLiteral("Telefonbuch: %1 Kontakte, %2 Favoriten, %3 mit Bild, %4 Anrufe (%5, %6 ms)")
                 .arg(m_contacts.size()).arg(favs).arg(photos).arg(m_calls.size()).arg(source).arg(ms));
    emit loaded();
}
