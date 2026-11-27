#include "phonebook.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

#ifdef HAVE_QTCONTACTS
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

PhoneBook::PhoneBook(QObject *parent) : QObject(parent) {}

void PhoneBook::load()
{
    QElapsedTimer t;
    t.start();
    QVector<PhoneContact> out;
    const QString fake = QString::fromLocal8Bit(qgetenv("SAILAUTO_FAKE_CONTACTS"));
    if (!fake.isEmpty()) {
        QFile f(fake);
        f.open(QIODevice::ReadOnly);
        for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
            const QJsonObject o = v.toObject();
            PhoneContact c;
            c.name = o.value(QStringLiteral("name")).toString();
            c.favorite = o.value(QStringLiteral("favorite")).toBool();
            c.photo = smallPhoto(o.value(QStringLiteral("photo")).toString());
            for (const QJsonValue &n : o.value(QStringLiteral("numbers")).toArray()) {
                c.numbers << n.toObject().value(QStringLiteral("number")).toString();
                c.labels << n.toObject().value(QStringLiteral("label")).toString();
            }
            if (!c.numbers.isEmpty()) out.append(c);
        }
    } else {
#ifdef HAVE_QTCONTACTS
        QContactManager mgr(QStringLiteral("org.nemomobile.contacts.sqlite"));
        QContactFetchHint hint;
        hint.setDetailTypesHint(QList<QContactDetail::DetailType>()
                                << QContactDisplayLabel::Type << QContactName::Type << QContactPhoneNumber::Type
                                << QContactAvatar::Type << QContactFavorite::Type);
        // Only contacts with a phone number (filter: detail type exists)
        QContactDetailFilter hasNumber;
        hasNumber.setDetailType(QContactPhoneNumber::Type);
        const QList<QContact> all = mgr.contacts(hasNumber, QList<QContactSortOrder>(), hint);
        if (mgr.error() != QContactManager::NoError)
            emit log(QStringLiteral("Telefonbuch: Adressbuch nicht lesbar (Fehler %1)").arg(mgr.error()));
        for (const QContact &k : all) {
            PhoneContact c;
            c.name = k.detail<QContactDisplayLabel>().label();
            if (c.name.isEmpty()) {
                const QContactName n = k.detail<QContactName>();
                c.name = (n.firstName() + QLatin1Char(' ') + n.lastName()).trimmed();
            }
            c.favorite = k.detail<QContactFavorite>().isFavorite();
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
            if (!c.numbers.isEmpty()) out.append(c);
        }
#else
        emit log(QStringLiteral("Telefonbuch: ohne Adressbuch gebaut"));
#endif
    }
    std::sort(out.begin(), out.end(), [](const PhoneContact &a, const PhoneContact &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    m_contacts = out;
    int favs = 0, photos = 0;
    for (const PhoneContact &c : m_contacts) {
        favs += c.favorite;
        photos += !c.photo.isNull();
    }
    emit log(QStringLiteral("Telefonbuch: %1 Kontakte mit Nummer, %2 Favoriten, %3 mit Bild (%4 ms)")
                 .arg(m_contacts.size()).arg(favs).arg(photos).arg(t.elapsed()));
    emit loaded();
}
