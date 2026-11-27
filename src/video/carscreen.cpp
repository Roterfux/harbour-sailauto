#include "carscreen.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>


// Text never gets cut with "…": shrink the font (down to ~55 %), and wrap
// onto a second line when the rect is tall enough.
static void drawFit(QPainter &p, const QRect &r, int flags, const QString &t)
{
    p.save();
    QFont f = p.font();
    int px = f.pixelSize() > 0 ? f.pixelSize() : 16;
    const int minPx = qMax(9, px * 55 / 100);
    for (; px > minPx; --px) {
        f.setPixelSize(px);
        if (QFontMetrics(f).width(t) <= r.width()) break;
    }
    f.setPixelSize(px);
    p.setFont(f);
    const QFontMetrics fm(f);
    if (fm.width(t) > r.width() && r.height() >= fm.height() * 2) flags |= Qt::TextWordWrap;
    p.drawText(r, flags, t);
    p.restore();
}

namespace {
struct Palette { QColor bg, panel, panelPressed, text, dim, accent; };
Palette palette(bool night)
{
    if (night)
        return {QColor(8, 10, 16), QColor(28, 32, 44), QColor(52, 60, 84), QColor(220, 224, 235), QColor(120, 128, 150), QColor(0, 170, 200)};
    return {QColor(18, 22, 32), QColor(40, 48, 68), QColor(70, 84, 120), QColor(245, 247, 252), QColor(150, 160, 185), QColor(0, 200, 230)};
}
}

CarScreen::CarScreen(QObject *parent) : QObject(parent)
{
    setGeometry(800, 480, 0, 0);
}

void CarScreen::setGeometry(int width, int height, int marginWidth, int marginHeight)
{
    m_image = QImage(width, height, QImage::Format_RGB32);
    // The HU crops the margins away; keep all content inside the safe area.
    m_safe = QRect(marginWidth / 2, marginHeight / 2, width - marginWidth, height - marginHeight);
    layout();
}

void CarScreen::setNight(bool night) { m_night = night; }

void CarScreen::setInfo(const QString &headUnit, const QString &encoder)
{
    m_headUnit = headUnit;
    m_encoder = encoder;
}

void CarScreen::setStats(double fps, double kbps)
{
    m_fps = fps;
    m_kbps = kbps;
}

void CarScreen::layout()
{
    m_tiles.clear();
    const int bar = m_safe.height() / 8;
    const QRect area = m_safe.adjusted(m_safe.width() / 40, bar + m_safe.height() / 30,
                                       -m_safe.width() / 40, -m_safe.height() / 30);
    const int gap = area.width() / 40;
    const int n = 5;
    const int w = (area.width() - (n - 1) * gap) / n;
    const int h = area.height();
    const struct { Page p; const char *t; const char *g; } defs[] = {
        {Navigation, "Navigation", "⮝"}, {Music, "Musik", "♫"}, {Apps, "Apps", ""},
        {Phone, "Telefon", "☎"}, {Info, "Info", "i"},
    };
    for (int i = 0; i < n; ++i)
        m_tiles.append({QRect(area.left() + i * (w + gap), area.top(), w, h), defs[i].p,
                        QString::fromUtf8(defs[i].t), QString::fromUtf8(defs[i].g)});
    m_backButton = QRect(m_safe.left() + m_safe.width() / 40, m_safe.top() + bar / 8, bar * 3, bar * 3 / 4);
    // Music page: transport buttons in a row at the bottom.
    const int pb = m_safe.height() / 5;
    const int cy = m_safe.bottom() - m_safe.height() / 30 - pb;
    m_playButton = QRect(m_safe.center().x() - pb / 2, cy, pb, pb);
    m_prevButton = m_playButton.translated(-pb * 2, 0);
    m_nextButton = m_playButton.translated(pb * 2, 0);
    layoutApps();
}

void CarScreen::showNotification(const QString &app, const QString &title, const QString &body)
{
    if (m_banners.size() >= 5) m_banners.removeFirst(); // flood: keep the newest
    m_banners.append({app, title, body});
    if (m_banners.size() == 1) m_bannerClock.start();
}

void CarScreen::drawBanner(QPainter &p)
{
    if (m_banners.isEmpty()) return;
    if (m_bannerClock.elapsed() > 8000) {
        m_banners.removeFirst();
        m_bannerClock.start();
        if (m_banners.isEmpty()) {
            m_bannerRect = QRect();
            return;
        }
    }
    const Palette c = palette(m_night);
    const Banner &b = m_banners.first();
    const int bar = m_safe.height() / 8;
    m_bannerRect = QRect(m_safe.left() + m_safe.width() / 20, m_safe.top() + bar / 6,
                         m_safe.width() * 9 / 10, bar * 2 + bar / 2);
    p.setPen(QPen(c.accent, 2));
    p.setBrush(c.panelPressed);
    p.drawRoundedRect(m_bannerRect, 16, 16);
    const QRect in = m_bannerRect.adjusted(bar / 3, bar / 6, -bar / 3, -bar / 6);
    QFont f = p.font();
    f.setPixelSize(bar * 3 / 10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.accent);
    const QString head = b.app + (m_banners.size() > 1 ? QStringLiteral("  (+%1)").arg(m_banners.size() - 1) : QString());
    drawFit(p, QRect(in.left(), in.top(), in.width(), bar / 2), Qt::AlignLeft | Qt::AlignVCenter,
               head);
    f.setPixelSize(bar * 9 / 20);
    p.setFont(f);
    p.setPen(c.text);
    const QRect titleR(in.left(), in.top() + bar / 2, in.width(), bar * 2 / 3);
    drawFit(p, titleR, Qt::AlignLeft | Qt::AlignVCenter, b.title);
    f.setBold(false);
    f.setPixelSize(bar * 2 / 5);
    p.setFont(f);
    p.setPen(m_parked ? c.text : c.dim);
    const QString body = m_parked ? b.body : (b.body.isEmpty() ? QString() : QStringLiteral("Text nur im Stand"));
    const QRect bodyR(in.left(), titleR.bottom(), in.width(), in.bottom() - titleR.bottom());
    drawFit(p, bodyR, Qt::AlignLeft | Qt::AlignVCenter,
               QString(body).replace(QLatin1Char('\n'), QLatin1Char(' ')));
}

void CarScreen::setNav(const NavState &s, const QImage &icon)
{
    // Show the navigation page when a route starts.
    if (s.running && !m_nav.running && m_page != Navigation) m_page = Navigation;
    if (s.running) m_navSearchSent = false;
    m_nav = s;
    m_navIcon = icon;
}

void CarScreen::drawManeuver(QPainter &p, const QRect &r, bool small)
{
    const Palette c = palette(m_night);
    if (!m_navIcon.isNull()) {
        p.drawImage(r, m_navIcon);
        return;
    }
    // Own arrow from the icon name: left/right/slight/sharp/uturn/roundabout/arrive.
    const QString n = m_nav.icon;
    p.setPen(QPen(c.text, qMax(small ? 2 : 4, r.width() / 10), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    const QPoint bottom(r.center().x(), r.bottom() - r.height() / 10);
    const QPoint mid = r.center();
    if (n.startsWith(QLatin1String("arrive")) || n == QLatin1String("flag")) {
        p.drawLine(QPoint(mid.x() - r.width() / 6, r.bottom() - r.height() / 10), QPoint(mid.x() - r.width() / 6, r.top() + r.height() / 8));
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        QPolygon flag;
        flag << QPoint(mid.x() - r.width() / 6, r.top() + r.height() / 8) << QPoint(mid.x() + r.width() / 4, r.top() + r.height() / 4)
             << QPoint(mid.x() - r.width() / 6, r.top() + r.height() * 3 / 8);
        p.drawPolygon(flag);
        return;
    }
    if (n.startsWith(QLatin1String("roundabout")) || n.startsWith(QLatin1String("rotary"))) {
        const int rad = r.width() / 5;
        p.drawEllipse(mid, rad, rad);
        p.drawLine(bottom, QPoint(mid.x(), mid.y() + rad));
        if (m_nav.roundaboutExit > 0 && !small) {
            QFont f = p.font();
            f.setPixelSize(rad);
            f.setBold(true);
            p.setFont(f);
            p.drawText(QRect(mid.x() - rad, mid.y() - rad, 2 * rad, 2 * rad), Qt::AlignCenter, QString::number(m_nav.roundaboutExit));
        }
        return;
    }
    int dx = 0; // end point direction
    if (n.contains(QLatin1String("uturn"))) {
        QPainterPath path(bottom);
        path.lineTo(mid.x() + r.width() / 6, mid.y() - r.height() / 8);
        path.arcTo(QRectF(mid.x() - r.width() / 6, mid.y() - r.height() / 3, r.width() / 3, r.height() / 3), 0, 180);
        path.lineTo(mid.x() - r.width() / 6, mid.y() + r.height() / 8);
        p.drawPath(path);
        return;
    }
    if (n.contains(QLatin1String("left"))) dx = n.contains(QLatin1String("slight")) ? -1 : n.contains(QLatin1String("sharp")) ? -3 : -2;
    else if (n.contains(QLatin1String("right"))) dx = n.contains(QLatin1String("slight")) ? 1 : n.contains(QLatin1String("sharp")) ? 3 : 2;
    const QPoint turn(mid.x(), mid.y() + r.height() / 10);
    QPoint end;
    switch (qAbs(dx)) {
    case 0: end = QPoint(mid.x(), r.top() + r.height() / 8); break;
    case 1: end = QPoint(mid.x() + (dx > 0 ? 1 : -1) * r.width() / 4, r.top() + r.height() / 6); break;
    case 2: end = QPoint(mid.x() + (dx > 0 ? 1 : -1) * r.width() * 3 / 8, turn.y()); break;
    default: end = QPoint(mid.x() + (dx > 0 ? 1 : -1) * r.width() / 3, r.bottom() - r.height() / 4); break;
    }
    p.drawLine(bottom, turn);
    p.drawLine(turn, end);
    // Arrow head
    const QLineF dir(end, turn);
    QLineF a = dir; a.setLength(r.width() / 6); a.setAngle(dir.angle() + 35);
    QLineF b = dir; b.setLength(r.width() / 6); b.setAngle(dir.angle() - 35);
    p.drawLine(a);
    p.drawLine(b);
}

void CarScreen::drawNavPage(QPainter &p, const QRect &body)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    m_navStartButton = QRect();
    m_navButtons.clear();
    QFont f = p.font();
    if (m_map && !m_navChoosing) {
        drawNavOnMap(p, body);
        return;
    }
    if (!m_nav.running) {
        drawNavChoose(p, body);
        return;
    }
    // Left: maneuver icon + distance. Right: street, instruction. Bottom: destination.
    const int iconSize = qMin(body.height() * 5 / 10, body.width() / 3);
    const QRect left(body.left(), body.top(), iconSize + 40, body.height() - bar * 3 / 2);
    const QRect iconR(left.center().x() - iconSize / 2, left.top() + 10, iconSize, iconSize);
    drawManeuver(p, iconR, false);
    f.setPixelSize(bar * 4 / 5);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    p.drawText(QRect(left.left(), iconR.bottom() + 5, left.width(), bar), Qt::AlignCenter, m_nav.manDist);

    const QRect right(left.right() + 20, body.top(), body.right() - left.right() - 20, left.height());
    f.setPixelSize(bar * 3 / 4);
    p.setFont(f);
    const QString street = m_nav.street.isEmpty() ? m_nav.narrative : m_nav.street;
    p.drawText(QRect(right.left(), right.top() + 10, right.width(), right.height() / 2 - 10),
               Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, street);
    if (!m_nav.street.isEmpty() && !m_nav.narrative.isEmpty()) {
        f.setPixelSize(bar * 2 / 5);
        f.setBold(false);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(QRect(right.left(), right.center().y(), right.width(), right.height() / 2),
                   Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_nav.narrative);
    }
    if (!m_nav.alongRoute) {
        f.setPixelSize(bar * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(240, 170, 40));
        p.drawText(QRect(right.left(), right.bottom() - bar / 2, right.width(), bar / 2), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Abseits der Route"));
    }
    // Destination strip with progress
    const QRect strip(body.left(), body.bottom() - bar, body.width(), bar);
    p.setPen(Qt::NoPen);
    p.setBrush(c.panel);
    p.drawRoundedRect(strip, 12, 12);
    p.setBrush(c.accent);
    const QRect prog(strip.left() + 10, strip.bottom() - 8, (strip.width() - 20) * qBound(0, m_nav.progress, 100) / 100, 4);
    p.drawRect(prog);
    f.setPixelSize(bar * 9 / 20);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    p.drawText(strip.adjusted(20, 0, -20, -6), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("Ziel: %1  ·  %2").arg(m_nav.destDist, m_nav.destTime));
    p.drawText(strip.adjusted(20, 0, -20, -6), Qt::AlignVCenter | Qt::AlignRight,
               QStringLiteral("Ankunft %1").arg(m_nav.destEta));
}

void CarScreen::setCall(const CallInfo &c, bool silenced)
{
    m_call = c;
    m_callSilenced = silenced;
    if (c.state == CallInfo::None) m_callButtons.clear();
}

void CarScreen::drawCall(QPainter &p)
{
    m_callButtons.clear();
    if (m_call.state == CallInfo::None) return;
    const Palette c = palette(m_night);
    const QColor green(40, 170, 80), red(210, 50, 50);
    const int bar = m_safe.height() / 8;
    const QString who = m_call.name.isEmpty() ? (m_call.number.isEmpty() ? QStringLiteral("Unbekannt") : m_call.number)
                                              : m_call.name;
    QFont f = p.font();
    auto button = [&](const QRect &r, const QString &action, const QString &label, const QColor &col) {
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(r, 14, 14);
        QFont bf = f;
        bf.setBold(true);
        bf.setPixelSize(qMin(r.height() * 2 / 5, bar * 9 / 20));
        p.setFont(bf);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, label);
        m_callButtons.append({r, action, label, col});
    };

    if (callFullScreen()) {
        p.fillRect(m_safe, c.bg);
        const QRect area = m_safe.adjusted(m_safe.width() / 20, bar / 2, -m_safe.width() / 20, -bar / 3);
        const int ph = area.height() * 9 / 20;
        const QRect photoR(area.left(), area.top(), ph, ph);
        if (!m_call.photo.isNull()) {
            QPainterPath clip;
            clip.addEllipse(photoR);
            p.save();
            p.setClipPath(clip);
            p.drawImage(photoR, m_call.photo);
            p.restore();
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(c.accent);
            p.drawEllipse(photoR);
            f.setPixelSize(ph / 2);
            f.setBold(true);
            p.setFont(f);
            p.setPen(c.bg);
            const QString initials = m_call.name.isEmpty() ? QStringLiteral("?")
                : QString(m_call.name.split(QLatin1Char(' '), QString::SkipEmptyParts).value(0).left(1)
                          + m_call.name.split(QLatin1Char(' '), QString::SkipEmptyParts).value(1).left(1)).toUpper();
            p.drawText(photoR, Qt::AlignCenter, initials);
        }
        const QRect textR(photoR.right() + bar / 2, area.top(), area.right() - photoR.right() - bar / 2, ph);
        f.setPixelSize(bar * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.accent);
        p.drawText(QRect(textR.left(), textR.top(), textR.width(), ph / 4), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("Eingehender Anruf"));
        f.setPixelSize(bar * 4 / 5);
        p.setFont(f);
        p.setPen(c.text);
        drawFit(p, QRect(textR.left(), textR.top() + ph / 4, textR.width(), ph / 2), Qt::AlignLeft | Qt::AlignVCenter,
                   who);
        if (!m_call.name.isEmpty() && !m_call.number.isEmpty()) {
            f.setPixelSize(bar / 2);
            f.setBold(false);
            p.setFont(f);
            p.setPen(c.dim);
            p.drawText(QRect(textR.left(), textR.top() + ph * 3 / 4, textR.width(), ph / 4), Qt::AlignLeft | Qt::AlignVCenter,
                       m_call.number);
        }
        // Buttons: decline | silence | silence + music | answer
        const int gap = bar / 4;
        const int bh = qMin(area.bottom() - photoR.bottom() - bar / 2, bar * 2);
        const int bw = (area.width() - 3 * gap) / 4;
        const int by = area.bottom() - bh;
        button(QRect(area.left(), by, bw, bh), QStringLiteral("decline"), QStringLiteral("Ablehnen"), red);
        button(QRect(area.left() + (bw + gap), by, bw, bh), QStringLiteral("silence"), QStringLiteral("Stumm"), c.panelPressed);
        button(QRect(area.left() + 2 * (bw + gap), by, bw, bh), QStringLiteral("silence_music"),
               QStringLiteral("Stumm\n+ Musik"), c.panelPressed);
        button(QRect(area.left() + 3 * (bw + gap), by, bw, bh), QStringLiteral("answer"), QStringLiteral("Annehmen"), green);
        return;
    }

    // Strip at the bottom: silenced ringing, active, outgoing or held call.
    const int sh = bar + bar / 3;
    const QRect strip(m_safe.left() + m_safe.width() / 40, m_safe.bottom() - sh - bar / 6,
                      m_safe.width() * 38 / 40, sh);
    p.setPen(QPen(m_call.state == CallInfo::Incoming ? c.accent : green, 2));
    p.setBrush(c.panelPressed);
    p.drawRoundedRect(strip, 14, 14);
    QString status;
    switch (m_call.state) {
    case CallInfo::Incoming: status = QStringLiteral("Anruf (stumm)"); break;
    case CallInfo::Outgoing: status = QStringLiteral("Wählt"); break;
    case CallInfo::Held: status = QStringLiteral("Gehalten"); break;
    default: {
        const qint64 secs = m_call.startedAt.isValid() ? m_call.startedAt.secsTo(QDateTime::currentDateTime()) : 0;
        status = QStringLiteral("Gespräch %1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
    }
    }
    const int pad = bar / 4;
    const int bw = bar * 3;
    QRect buttons(strip.right() - pad - (m_call.state == CallInfo::Incoming ? 2 * bw + pad : bw), strip.top() + pad / 2,
                  0, strip.height() - pad);
    buttons.setRight(strip.right() - pad);
    if (m_call.state == CallInfo::Incoming) {
        button(QRect(buttons.left(), buttons.top(), bw, buttons.height()), QStringLiteral("decline"), QStringLiteral("Ablehnen"), red);
        button(QRect(buttons.left() + bw + pad, buttons.top(), bw, buttons.height()), QStringLiteral("answer"),
               QStringLiteral("Annehmen"), green);
    } else {
        button(buttons, QStringLiteral("hangup"), QStringLiteral("Auflegen"), red);
    }
    const QRect textR(strip.left() + pad, strip.top(), buttons.left() - strip.left() - 2 * pad, strip.height());
    f.setPixelSize(bar * 2 / 5);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    drawFit(p, textR, Qt::AlignLeft | Qt::AlignVCenter,
               status + QStringLiteral(": ") + who);
}

void CarScreen::setContacts(const QVector<PhoneContact> &contacts)
{
    m_contacts = contacts;
    m_phoneDetail = -1;
    m_phoneScroll = 0;
    // No favorites: start on the full list.
    bool fav = false;
    for (const PhoneContact &c : m_contacts) fav |= c.favorite;
    m_phoneTab = fav ? 0 : 1;
}

int CarScreen::contactForNumber(const QString &number) const
{
    auto key = [](const QString &n) {
        QString d;
        for (const QChar ch : n)
            if (ch.isDigit()) d += ch;
        return d.right(8);
    };
    const QString k = key(number);
    if (k.size() < 6) return -1;
    for (int i = 0; i < m_contacts.size(); ++i)
        for (const QString &n : m_contacts[i].numbers)
            if (key(n) == k) return i;
    return -1;
}

QVector<int> CarScreen::phoneVisible() const
{
    QVector<int> out;
    for (int i = 0; i < m_contacts.size(); ++i)
        if (!m_contacts[i].numbers.isEmpty() && (m_phoneTab == 1 || m_contacts[i].favorite)) out.append(i);
    return out;
}

void CarScreen::drawPhonePage(QPainter &p, const QRect &body)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    QFont f = p.font();
    m_phoneRows.clear();
    m_dialButtons.clear();
    auto avatar = [&](const QRect &r, const PhoneContact &k) {
        if (!k.photo.isNull()) {
            QPainterPath clip;
            clip.addEllipse(r);
            p.save();
            p.setClipPath(clip);
            p.drawImage(r, k.photo);
            p.restore();
            return;
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c.panelPressed);
        p.drawEllipse(r);
        QFont af = f;
        af.setPixelSize(r.height() * 2 / 5);
        af.setBold(true);
        p.setFont(af);
        p.setPen(c.text);
        const QStringList parts = k.name.split(QLatin1Char(' '), QString::SkipEmptyParts);
        p.drawText(r, Qt::AlignCenter, (parts.value(0).left(1) + parts.value(1).left(1)).toUpper());
    };

    if (m_contacts.isEmpty()) {
        f.setPixelSize(bar / 2);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(body, Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Kein Kontakt mit Telefonnummer.\n(Adressbuch leer oder nicht lesbar – siehe Protokoll)"));
        return;
    }

    if (m_phoneDetail >= 0 && m_phoneDetail < m_contacts.size()) {
        const PhoneContact &k = m_contacts[m_phoneDetail];
        const int ph = qMin(body.height() / 2, body.width() / 4);
        const QRect photoR(body.left(), body.top() + 10, ph, ph);
        avatar(photoR, k);
        f.setPixelSize(bar * 3 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(QRect(body.left(), photoR.bottom() + 10, ph + 40, bar * 2), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, k.name);
        // One big button per number.
        const QRect nums(photoR.right() + bar / 2, body.top() + 10, body.right() - photoR.right() - bar / 2, body.height() - 20);
        const int bh = qMin(bar * 3 / 2, (nums.height() - 10 * 3) / qMax(1, qMin(4, k.numbers.size())));
        for (int i = 0; i < k.numbers.size() && i < 4; ++i) {
            const QRect r(nums.left(), nums.top() + i * (bh + 10), nums.width(), bh);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(40, 170, 80));
            p.drawRoundedRect(r, 14, 14);
            f.setPixelSize(bar * 2 / 5);
            f.setBold(true);
            p.setFont(f);
            p.setPen(Qt::white);
            const QRect inner = r.adjusted(bar / 3, 0, -bar / 3, 0);
            p.drawText(inner, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Anrufen  ·  ") + k.labels.value(i));
            f.setBold(false);
            p.setFont(f);
            p.drawText(inner, Qt::AlignRight | Qt::AlignVCenter, k.numbers[i]);
            m_dialButtons.append({r, k.numbers[i]});
        }
        return;
    }

    // Tabs left, list middle, page buttons right.
    const int tabW = body.width() / 5;
    const int tabH = bar * 3 / 2;
    const char *tabNames[] = {"Favoriten", "Alle", "Zuletzt"};
    for (int i = 0; i < 3; ++i) {
        m_phoneTabs[i] = QRect(body.left(), body.top() + i * (tabH + 10), tabW, tabH);
        p.setPen(Qt::NoPen);
        p.setBrush(i == m_phoneTab ? c.accent : c.panel);
        p.drawRoundedRect(m_phoneTabs[i], 12, 12);
        f.setPixelSize(bar * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(i == m_phoneTab ? c.bg : c.text);
        p.drawText(m_phoneTabs[i], Qt::AlignCenter, QString::fromUtf8(tabNames[i]));
    }
    const int btnW = bar + 10;
    m_phoneList = QRect(body.left() + tabW + 15, body.top(), body.width() - tabW - 15 - btnW - 10, body.height());
    m_phoneUp = QRect(m_phoneList.right() + 10, body.top(), btnW, body.height() / 2 - 5);
    m_phoneDown = QRect(m_phoneList.right() + 10, body.center().y() + 5, btnW, body.height() / 2 - 5);

    if (m_phoneTab == 2) {
        // Recent calls: arrow (in/out, red = missed), name or number, time.
        const int rh = phoneRowHeight();
        const int maxScroll = qMax(0, m_recent.size() * rh - m_phoneList.height());
        m_phoneScroll = qBound(0, m_phoneScroll, maxScroll);
        if (m_recent.isEmpty()) {
            f.setPixelSize(bar * 2 / 5);
            f.setBold(false);
            p.setFont(f);
            p.setPen(c.dim);
            p.drawText(m_phoneList, Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("Keine Anrufe in der Liste."));
        }
        p.save();
        p.setClipRect(m_phoneList);
        const QDate today = QDate::currentDate();
        for (int k = m_phoneScroll / rh; k < m_recent.size(); ++k) {
            const QRect r(m_phoneList.left(), m_phoneList.top() + k * rh - m_phoneScroll, m_phoneList.width(), rh - 6);
            if (r.top() > m_phoneList.bottom()) break;
            const RecentCall &rc = m_recent[k];
            const int ci = contactForNumber(rc.number);
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(r, 12, 12);
            // direction arrow
            const QColor col = rc.missed ? QColor(230, 70, 60) : rc.outgoing ? c.accent : QColor(60, 190, 100);
            const int as = r.height() / 2;
            const QRect ar(r.left() + 14, r.center().y() - as / 2, as, as);
            p.setPen(QPen(col, qMax(3, as / 8), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPoint a = rc.outgoing ? ar.bottomLeft() : ar.topRight();
            const QPoint b = rc.outgoing ? ar.topRight() : ar.bottomLeft();
            p.drawLine(a, b);
            p.drawLine(b, b + (rc.outgoing ? QPoint(-as / 2, 0) : QPoint(as / 2, 0)));
            p.drawLine(b, b + (rc.outgoing ? QPoint(0, as / 2) : QPoint(0, -as / 2)));
            const QRect tr(ar.right() + 18, r.top(), r.width() - as - 40, r.height());
            const QDateTime t = QDateTime::fromMSecsSinceEpoch(rc.time * 1000).toLocalTime();
            const QString when = t.date() == today ? t.toString(QStringLiteral("HH:mm"))
                                 : t.date() == today.addDays(-1) ? QStringLiteral("gestern ") + t.toString(QStringLiteral("HH:mm"))
                                                                 : t.toString(QStringLiteral("dd.MM. HH:mm"));
            f.setPixelSize(qMin(bar * 2 / 5, r.height() * 2 / 5));
            f.setBold(true);
            p.setFont(f);
            p.setPen(rc.missed ? QColor(240, 110, 100) : c.text);
            const QString who = ci >= 0 ? m_contacts[ci].name : rc.number;
            const int whenW = QFontMetrics(f).width(when) + 20;
            drawFit(p, tr.adjusted(0, 0, -whenW, 0), Qt::AlignLeft | Qt::AlignVCenter,
                       who);
            f.setBold(false);
            p.setFont(f);
            p.setPen(c.dim);
            p.drawText(tr, Qt::AlignRight | Qt::AlignVCenter, when);
            m_phoneRows.append({r.intersected(m_phoneList), -(k + 1)}); // negative = recent call
        }
        p.restore();
        for (const QRect &ar : {m_phoneUp, m_phoneDown}) {
            const bool up = ar == m_phoneUp;
            const bool enabled = up ? m_phoneScroll > 0 : m_phoneScroll < maxScroll;
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(ar, 12, 12);
            p.setBrush(enabled ? c.text : c.dim);
            const QPoint o = ar.center();
            const int k = ar.width() / 4;
            QPolygon tri;
            if (up) tri << QPoint(o.x() - k, o.y() + k / 2) << QPoint(o.x() + k, o.y() + k / 2) << QPoint(o.x(), o.y() - k / 2);
            else tri << QPoint(o.x() - k, o.y() - k / 2) << QPoint(o.x() + k, o.y() - k / 2) << QPoint(o.x(), o.y() + k / 2);
            p.drawPolygon(tri);
        }
        return;
    }
    const QVector<int> vis = phoneVisible();
    if (m_phoneTab == 0 && !vis.isEmpty()) {
        // Favorites: big tiles (3 x 2), one tap = call (one number) or numbers.
        const int perPage = 6;
        const int pages = (vis.size() + perPage - 1) / perPage;
        m_favPage = qBound(0, m_favPage, pages - 1);
        const int gap = 10;
        const int tw = (m_phoneList.width() - 2 * gap) / 3;
        const int th = (m_phoneList.height() - gap) / 2;
        for (int k = 0; k < perPage && m_favPage * perPage + k < vis.size(); ++k) {
            const int ci = vis[m_favPage * perPage + k];
            const PhoneContact &ct = m_contacts[ci];
            const QRect r(m_phoneList.left() + (k % 3) * (tw + gap), m_phoneList.top() + (k / 3) * (th + gap), tw, th);
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(r, 14, 14);
            const int as = qMin(tw - 20, th * 13 / 20);
            avatar(QRect(r.center().x() - as / 2, r.top() + 8, as, as), ct);
            f.setPixelSize(qMin(bar * 2 / 5, th / 6));
            f.setBold(true);
            p.setFont(f);
            p.setPen(c.text);
            const QRect nr(r.left() + 6, r.top() + 8 + as, r.width() - 12, r.bottom() - (r.top() + 8 + as));
            // Whole name, never cut: two lines, smaller font if needed.
            int px = qMin(bar * 2 / 5, th / 6);
            QRect need;
            for (; px > 10; --px) {
                f.setPixelSize(px);
                need = QFontMetrics(f).boundingRect(nr, Qt::AlignCenter | Qt::TextWordWrap, ct.name);
                if (need.width() <= nr.width() && need.height() <= nr.height()) break;
            }
            p.setFont(f);
            p.drawText(nr, Qt::AlignCenter | Qt::TextWordWrap, ct.name);
            m_phoneRows.append({r, ci});
        }
        for (const QRect &ar : {m_phoneUp, m_phoneDown}) {
            const bool up = ar == m_phoneUp;
            const bool enabled = up ? m_favPage > 0 : m_favPage + 1 < pages;
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(ar, 12, 12);
            p.setBrush(enabled ? c.text : c.dim);
            const QPoint o = ar.center();
            const int k = ar.width() / 4;
            QPolygon tri;
            if (up) tri << QPoint(o.x() - k, o.y() + k / 2) << QPoint(o.x() + k, o.y() + k / 2) << QPoint(o.x(), o.y() - k / 2);
            else tri << QPoint(o.x() - k, o.y() - k / 2) << QPoint(o.x() + k, o.y() - k / 2) << QPoint(o.x(), o.y() + k / 2);
            p.drawPolygon(tri);
        }
        return;
    }
    const int rh = phoneRowHeight();
    const int maxScroll = qMax(0, vis.size() * rh - m_phoneList.height());
    m_phoneScroll = qBound(0, m_phoneScroll, maxScroll);
    if (vis.isEmpty()) {
        f.setPixelSize(bar * 2 / 5);
        f.setBold(false);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(m_phoneList, Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Keine Favoriten.\nIn „Personen“ am Telefon einen Kontakt als Favorit markieren."));
    }
    p.save();
    p.setClipRect(m_phoneList);
    const int first = m_phoneScroll / rh;
    for (int k = first; k < vis.size(); ++k) {
        const QRect r(m_phoneList.left(), m_phoneList.top() + k * rh - m_phoneScroll, m_phoneList.width(), rh - 6);
        if (r.top() > m_phoneList.bottom()) break;
        const PhoneContact &ct = m_contacts[vis[k]];
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(r, 12, 12);
        const int as = r.height() - 12;
        avatar(QRect(r.left() + 8, r.top() + 6, as, as), ct);
        const QRect tr(r.left() + as + 24, r.top(), r.width() - as - 32, r.height());
        f.setPixelSize(qMin(bar * 2 / 5, r.height() * 2 / 5));
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        drawFit(p, tr.adjusted(0, 0, 0, -r.height() / 3), Qt::AlignLeft | Qt::AlignBottom,
                   ct.name);
        f.setBold(false);
        f.setPixelSize(qMin(bar * 3 / 10, r.height() * 3 / 10));
        p.setFont(f);
        p.setPen(c.dim);
        const QString sub = ct.labels.value(0) + QStringLiteral(" ") + ct.numbers.value(0)
                            + (ct.numbers.size() > 1 ? QStringLiteral("  (+%1)").arg(ct.numbers.size() - 1) : QString());
        drawFit(p, tr.adjusted(0, r.height() * 2 / 3 - 4, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                   sub);
        m_phoneRows.append({r.intersected(m_phoneList), vis[k]});
    }
    p.restore();
    // Page buttons
    for (const QRect &ar : {m_phoneUp, m_phoneDown}) {
        const bool up = ar == m_phoneUp;
        const bool enabled = up ? m_phoneScroll > 0 : m_phoneScroll < maxScroll;
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(ar, 12, 12);
        p.setBrush(enabled ? c.text : c.dim);
        const QPoint o = ar.center();
        const int k = ar.width() / 4;
        QPolygon tri;
        if (up) tri << QPoint(o.x() - k, o.y() + k / 2) << QPoint(o.x() + k, o.y() + k / 2) << QPoint(o.x(), o.y() - k / 2);
        else tri << QPoint(o.x() - k, o.y() - k / 2) << QPoint(o.x() + k, o.y() - k / 2) << QPoint(o.x(), o.y() + k / 2);
        p.drawPolygon(tri);
    }
}

void CarScreen::phoneTap(const QPoint &pt)
{
    if (m_phoneDetail >= 0) {
        for (const DialButton &b : m_dialButtons)
            if (b.rect.contains(pt)) {
                emit dial(b.number, m_contacts.value(m_phoneDetail).name, m_contacts.value(m_phoneDetail).photo);
                return;
            }
        return;
    }
    for (int i = 0; i < 3; ++i)
        if (m_phoneTabs[i].contains(pt)) {
            m_phoneTab = i;
            m_phoneScroll = 0;
            m_favPage = 0;
            return;
        }
    const bool favGrid = m_phoneTab == 0;
    const int page = m_phoneList.height() - phoneRowHeight();
    if (m_phoneUp.contains(pt)) { if (favGrid) --m_favPage; else m_phoneScroll -= page; return; }
    if (m_phoneDown.contains(pt)) { if (favGrid) ++m_favPage; else m_phoneScroll += page; return; }
    for (const Row &r : m_phoneRows)
        if (r.rect.contains(pt) && r.contact < 0) {
            const RecentCall &rc = m_recent.value(-r.contact - 1);
            const int ci = contactForNumber(rc.number);
            emit dial(rc.number, ci >= 0 ? m_contacts[ci].name : QString(), ci >= 0 ? m_contacts[ci].photo : QImage());
            return;
        }
    for (const Row &r : m_phoneRows)
        if (r.rect.contains(pt)) {
            const PhoneContact &ct = m_contacts[r.contact];
            if (favGrid && ct.numbers.size() == 1)
                emit dial(ct.numbers.first(), ct.name, ct.photo); // favorite with one number: call right away
            else
                m_phoneDetail = r.contact;
            return;
        }
}

void CarScreen::drawNavOnMap(QPainter &p, const QRect &body)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    // Map uses the whole area below the top bar.
    const QRect area(m_safe.left(), m_safe.top() + bar, m_safe.width(), m_safe.height() - bar);
    m_map->paint(p, area);
    QFont f = p.font();
    QColor card = c.bg;
    card.setAlpha(215);
    auto button = [&](const QRect &r, const QString &kind, const QString &label, const QColor &bg, const QColor &fg) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r, 12, 12);
        f.setPixelSize(bar * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(r, Qt::AlignCenter, label);
        m_navButtons.append({r, kind, QString()});
    };
    if (m_nav.running) {
        // Turn card top left: icon, distance, street
        const int is = bar * 2;
        const QRect cardR(area.left() + 10, area.top() + 10, qMin(area.width() / 2, is + bar * 6), is + bar / 2);
        p.setPen(Qt::NoPen);
        p.setBrush(card);
        p.drawRoundedRect(cardR, 14, 14);
        drawManeuver(p, QRect(cardR.left() + 8, cardR.top() + bar / 4, is, is), false);
        const QRect tr(cardR.left() + is + 18, cardR.top() + 6, cardR.width() - is - 26, cardR.height() - 12);
        f.setPixelSize(bar * 3 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(QRect(tr.left(), tr.top(), tr.width(), tr.height() / 2), Qt::AlignLeft | Qt::AlignVCenter, m_nav.manDist);
        f.setPixelSize(bar * 2 / 5);
        p.setFont(f);
        const QString street = m_nav.street.isEmpty() ? m_nav.narrative : m_nav.street;
        drawFit(p, QRect(tr.left(), tr.center().y(), tr.width(), tr.height() / 2), Qt::AlignLeft | Qt::AlignVCenter,
                   street);
        // Destination strip
        const QRect strip(area.left() + 10, area.bottom() - bar - 6, area.width() * 2 / 5, bar); // leaves the car free
        p.setBrush(card);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(strip, 12, 12);
        p.setBrush(c.accent);
        p.drawRect(QRect(strip.left() + 10, strip.bottom() - 7, (strip.width() - 20) * qBound(0, m_nav.progress, 100) / 100, 4));
        f.setPixelSize(bar * 2 / 5);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(strip.adjusted(14, 0, -14, -6), Qt::AlignVCenter | Qt::AlignLeft,
                   QStringLiteral("%1 · %2 · %3").arg(m_nav.destDist, m_nav.destTime, m_nav.destEta));
        if (!m_nav.alongRoute) {
            p.setPen(QColor(240, 170, 40));
            p.drawText(QRect(cardR.left(), cardR.bottom() + 4, cardR.width(), bar / 2), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("Abseits der Route"));
        }
        return;
    }
    // No route: "Ziel" (choose destination) and, if Pure Maps has one, start.
    button(QRect(area.right() - bar * 3 - 10, area.top() + 10, bar * 3, bar), QStringLiteral("choose"),
           QStringLiteral("Ziel"), c.accent, c.bg);
    if (m_nav.app && m_nav.hasRoute)
        button(QRect(area.left() + 10, area.top() + 10, bar * 6, bar), QStringLiteral("start"),
               QStringLiteral("Navigation starten"), QColor(40, 170, 80), Qt::white);
}

void CarScreen::drawNavChoose(QPainter &p, const QRect &body)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    QFont f = p.font();
    m_navButtons.clear();
    auto button = [&](const QRect &r, const QString &kind, const QString &value, const QString &label,
                      const QColor &bg, const QColor &fg, int px) {
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(r, 12, 12);
        f.setPixelSize(px);
        f.setBold(true);
        p.setFont(f);
        p.setPen(fg);
        p.drawText(r.adjusted(8, 0, -8, 0), Qt::AlignCenter | Qt::TextWordWrap, label);
        m_navButtons.append({r, kind, value});
    };
    auto line = [&](const QRect &r, const QString &text, const QColor &col, int px, bool bold) {
        f.setPixelSize(px);
        f.setBold(bold);
        p.setFont(f);
        p.setPen(col);
        drawFit(p, r, Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    const int px = bar * 2 / 5;

    if (m_navView == 1) {
        // Keyboard (only when standing)
        const QRect field(body.left(), body.top(), body.width(), bar);
        p.setPen(QPen(c.accent, 2));
        p.setBrush(c.panel);
        p.drawRoundedRect(field, 10, 10);
        line(field.adjusted(14, 0, -14, 0), m_navInput.isEmpty() ? QStringLiteral("Adresse oder Ort …") : m_navInput + QStringLiteral("|"),
             m_navInput.isEmpty() ? c.dim : c.text, px, false);
        const QRect keys(body.left(), field.bottom() + 8, body.width(), body.bottom() - field.bottom() - 8);
        if (!m_parked) {
            line(QRect(keys.left(), keys.top(), keys.width(), bar), QStringLiteral("Eingabe nur im Stand (Gang P)."), c.dim, px, true);
            button(QRect(keys.left(), keys.bottom() - bar, keys.width() / 3, bar), QStringLiteral("cancel"), QString(),
                   QStringLiteral("Abbrechen"), c.panel, c.text, px);
            return;
        }
        const QStringList rows = {QStringLiteral("1234567890"), QStringLiteral("QWERTZUIOPÜ"),
                                  QStringLiteral("ASDFGHJKLÖÄ"), QStringLiteral("YXCVBNMß,.-")};
        const int rowH = (keys.height() - 4 * 6) / 5;
        for (int r = 0; r < rows.size(); ++r) {
            const QString row = rows[r];
            const int kw = (keys.width() - 10 * 5) / 11;
            const int x0 = keys.left() + (keys.width() - (row.size() * kw + (row.size() - 1) * 5)) / 2;
            for (int i = 0; i < row.size(); ++i)
                button(QRect(x0 + i * (kw + 5), keys.top() + r * (rowH + 6), kw, rowH), QStringLiteral("key"),
                       QString(row[i]), QString(row[i]), c.panel, c.text, qMin(px, rowH / 2));
        }
        const int y = keys.top() + 4 * (rowH + 6);
        const int w4 = (keys.width() - 3 * 6) / 4;
        button(QRect(keys.left(), y, w4, rowH), QStringLiteral("cancel"), QString(), QStringLiteral("Abbrechen"), c.panel, c.text, qMin(px, rowH / 2));
        button(QRect(keys.left() + (w4 + 6), y, w4, rowH), QStringLiteral("key"), QStringLiteral(" "), QStringLiteral("Leerzeichen"), c.panel, c.text, qMin(px, rowH / 2));
        button(QRect(keys.left() + 2 * (w4 + 6), y, w4, rowH), QStringLiteral("backspace"), QString(), QStringLiteral("⌫"), c.panel, c.text, qMin(px, rowH / 2));
        button(QRect(keys.left() + 3 * (w4 + 6), y, w4, rowH), QStringLiteral("search"), m_navInput, QStringLiteral("Suchen"),
               c.accent, c.bg, qMin(px, rowH / 2));
        return;
    }

    if (m_navView == 2) {
        // Contacts with a postal address, 4 per page
        QVector<QPair<QString, QString>> items; // name, address
        for (const PhoneContact &k : m_contacts)
            for (const QString &a : k.addresses) items.append(qMakePair(k.name, a));
        if (items.isEmpty()) {
            line(QRect(body.left(), body.top(), body.width(), bar), QStringLiteral("Kein Kontakt mit Adresse."), c.dim, px, true);
            return;
        }
        const int per = 4;
        const int pages = (items.size() + per - 1) / per;
        m_navContactPage = qBound(0, m_navContactPage, pages - 1);
        const int btnW = bar + 10;
        const QRect list(body.left(), body.top(), body.width() - btnW - 10, body.height());
        const int rh = (list.height() - 3 * 8) / per;
        for (int k = 0; k < per && m_navContactPage * per + k < items.size(); ++k) {
            const auto &it = items[m_navContactPage * per + k];
            const QRect r(list.left(), list.top() + k * (rh + 8), list.width(), rh);
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(r, 12, 12);
            line(r.adjusted(16, 4, -16, -rh / 2), it.first, c.text, qMin(px, rh / 3), true);
            line(r.adjusted(16, rh / 2, -16, -4), it.second, c.dim, qMin(px * 4 / 5, rh / 3), false);
            m_navButtons.append({r, QStringLiteral("dest"), it.second});
        }
        button(QRect(list.right() + 10, body.top(), btnW, body.height() / 2 - 5), QStringLiteral("cpage"), QStringLiteral("-1"),
               QStringLiteral("▲"), c.panel, m_navContactPage > 0 ? c.text : c.dim, px);
        button(QRect(list.right() + 10, body.center().y() + 5, btnW, body.height() / 2 - 5), QStringLiteral("cpage"), QStringLiteral("1"),
               QStringLiteral("▼"), c.panel, m_navContactPage + 1 < pages ? c.text : c.dim, px);
        return;
    }

    // Choose: status / start, Home, Work, Address, Contacts, recent destinations
    int y = body.top();
    const QColor green(40, 170, 80);
    if (m_nav.app && m_nav.hasRoute) {
        button(QRect(body.left(), y, body.width(), bar * 3 / 2), QStringLiteral("start"), QString(),
               QStringLiteral("Route bereit – Navigation starten"), green, Qt::white, px);
        y += bar * 3 / 2 + 10;
    } else {
        const QString hint = !m_nav.app ? QStringLiteral("Pure Maps läuft nicht – Ziel wählen startet es.")
                             : m_navSearchSent ? QStringLiteral("Am Telefon: Ergebnis antippen, dann „Route“. Danach hier starten.")
                                               : QStringLiteral("Ziel wählen:");
        f.setPixelSize(px * 9 / 10);
        f.setBold(false);
        p.setFont(f);
        p.setPen(m_navSearchSent ? c.accent : c.dim);
        p.drawText(QRect(body.left(), y, body.width(), bar), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, hint);
        y += bar + 6;
    }
    const int gap = 10;
    const int w = (body.width() - 3 * gap) / 4;
    const int bh = bar * 3 / 2;
    button(QRect(body.left(), y, w, bh), QStringLiteral("home"), m_home, QStringLiteral("Zuhause"),
           m_home.isEmpty() ? c.panel : c.panelPressed, m_home.isEmpty() ? c.dim : c.text, px);
    button(QRect(body.left() + (w + gap), y, w, bh), QStringLiteral("work"), m_work, QStringLiteral("Arbeit"),
           m_work.isEmpty() ? c.panel : c.panelPressed, m_work.isEmpty() ? c.dim : c.text, px);
    button(QRect(body.left() + 2 * (w + gap), y, w, bh), QStringLiteral("keyboard"), QString(), QStringLiteral("Adresse eingeben"),
           c.panelPressed, m_parked ? c.text : c.dim, px);
    button(QRect(body.left() + 3 * (w + gap), y, w, bh), QStringLiteral("contacts"), QString(), QStringLiteral("Kontakte"),
           c.panelPressed, c.text, px);
    y += bh + 10;
    // Recent destinations
    const int rh = qMax(bar * 4 / 5, 40);
    for (const QString &d : m_recentDest) {
        if (y + rh > body.bottom()) break;
        const QRect r(body.left(), y, body.width(), rh);
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(r, 10, 10);
        line(r.adjusted(16, 0, -16, 0), QStringLiteral("↺  ") + d, c.text, px * 9 / 10, false);
        m_navButtons.append({r, QStringLiteral("dest"), d});
        y += rh + 6;
    }
}

void CarScreen::navButtonTapped(const NavButton &b)
{
    if (b.kind == QLatin1String("choose")) {
        m_navChoosing = true;
        m_navView = 0;
    } else if (b.kind == QLatin1String("start")) {
        emit navStart();
    } else if (b.kind == QLatin1String("home") || b.kind == QLatin1String("work") || b.kind == QLatin1String("dest")) {
        if (b.value.isEmpty()) {
            showNotification(QStringLiteral("Navigation"), b.kind == QLatin1String("home") ? QStringLiteral("Zuhause nicht festgelegt")
                                                                                          : QStringLiteral("Arbeit nicht festgelegt"),
                             QStringLiteral("Am Telefon: SailAuto → Ziele"));
            return;
        }
        m_navView = 0;
        m_navSearchSent = true;
        m_navChoosing = false;
        emit navSearch(b.value);
    } else if (b.kind == QLatin1String("keyboard")) {
        m_navView = 1;
    } else if (b.kind == QLatin1String("contacts")) {
        m_navView = 2;
        m_navContactPage = 0;
    } else if (b.kind == QLatin1String("cpage")) {
        m_navContactPage += b.value.toInt();
    } else if (b.kind == QLatin1String("key")) {
        if (m_navInput.size() < 80) m_navInput += b.value;
    } else if (b.kind == QLatin1String("backspace")) {
        m_navInput.chop(1);
    } else if (b.kind == QLatin1String("cancel")) {
        m_navView = 0;
    } else if (b.kind == QLatin1String("search")) {
        const QString t = m_navInput.trimmed();
        if (t.isEmpty()) return;
        m_navView = 0;
        m_navSearchSent = true;
        m_navChoosing = false;
        m_navInput.clear();
        emit navSearch(t);
    }
}

void CarScreen::drawMusicList(QPainter &p, const QRect &body)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    m_musicRows.clear();
    const int per = 5;
    const int pages = qMax(1, (m_musicList.size() + per - 1) / per);
    m_musicListPage = qBound(0, m_musicListPage, pages - 1);
    const int btnW = bar + 10;
    const QRect list(body.left(), body.top(), body.width() - btnW - 10, body.height());
    const int rh = (list.height() - (per - 1) * 6) / per;
    QFont f = p.font();
    for (int k = 0; k < per && m_musicListPage * per + k < m_musicList.size(); ++k) {
        const int idx = m_musicListPage * per + k;
        const ListItem &it = m_musicList[idx];
        const QRect r(list.left(), list.top() + k * (rh + 6), list.width(), rh);
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(r, 10, 10);
        f.setPixelSize(qMin(bar * 2 / 5, rh * 2 / 5));
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        const QString title = (it.playlist ? QStringLiteral("▤  ") : QString()) + (it.title.isEmpty() ? QStringLiteral("(ohne Titel)") : it.title);
        const QRect tr = r.adjusted(14, 0, -14, it.sub.isEmpty() ? 0 : -rh / 3);
        drawFit(p, tr, Qt::AlignLeft | Qt::AlignVCenter, title);
        if (!it.sub.isEmpty()) {
            f.setBold(false);
            f.setPixelSize(qMin(bar * 3 / 10, rh * 3 / 10));
            p.setFont(f);
            p.setPen(c.dim);
            const QRect sr = r.adjusted(14, rh * 2 / 3 - 4, -14, 0);
            drawFit(p, sr, Qt::AlignLeft | Qt::AlignTop, it.sub);
        }
        m_musicRows.append(qMakePair(r, idx));
    }
    m_mlUp = QRect(list.right() + 10, body.top(), btnW, body.height() / 2 - 5);
    m_mlDown = QRect(list.right() + 10, body.center().y() + 5, btnW, body.height() / 2 - 5);
    for (const QRect &ar : {m_mlUp, m_mlDown}) {
        const bool up = ar == m_mlUp;
        const bool enabled = up ? m_musicListPage > 0 : m_musicListPage + 1 < pages;
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(ar, 12, 12);
        p.setBrush(enabled ? c.text : c.dim);
        const QPoint o = ar.center();
        const int k = ar.width() / 4;
        QPolygon tri;
        if (up) tri << QPoint(o.x() - k, o.y() + k / 2) << QPoint(o.x() + k, o.y() + k / 2) << QPoint(o.x(), o.y() - k / 2);
        else tri << QPoint(o.x() - k, o.y() - k / 2) << QPoint(o.x() + k, o.y() - k / 2) << QPoint(o.x(), o.y() + k / 2);
        p.drawPolygon(tri);
    }
}

void CarScreen::setApps(const QVector<CarApp> &apps)
{
    m_apps = apps;
    m_appPage = 0;
    layoutApps();
}

void CarScreen::layoutApps()
{
    const int bar = m_safe.height() / 8;
    const QRect body = m_safe.adjusted(m_safe.width() / 30, bar + 10, -m_safe.width() / 30, -10);
    const int perPage = 8;
    const int pages = qMax(1, (m_apps.size() + perPage - 1) / perPage);
    m_appPage = qBound(0, m_appPage, pages - 1);
    // Paging arrows on the right if needed.
    const int arrowW = pages > 1 ? bar + 10 : 0;
    const QRect grid = body.adjusted(0, 0, -arrowW, 0);
    const int gap = 12;
    const int cw = (grid.width() - 3 * gap) / 4;
    const int ch = (grid.height() - gap) / 2;
    m_appRects.clear();
    const int first = m_appPage * perPage;
    for (int i = first; i < qMin(m_apps.size(), first + perPage); ++i) {
        const int k = i - first;
        m_appRects.append(QRect(grid.left() + (k % 4) * (cw + gap), grid.top() + (k / 4) * (ch + gap), cw, ch));
    }
    if (pages > 1) {
        m_appPrev = QRect(body.right() - bar, body.top(), bar, body.height() / 2 - 6);
        m_appNext = QRect(body.right() - bar, body.center().y() + 6, bar, body.height() / 2 - 6);
    } else {
        m_appPrev = m_appNext = QRect();
    }
}

void CarScreen::setNowPlaying(bool hasPlayer, const QString &player, const QString &title, const QString &artist,
                              const QString &album, const QImage &art, bool playing, qint64 posMs, qint64 lenMs)
{
    m_hasPlayer = hasPlayer;
    m_player = player;
    m_title = title;
    m_artist = artist;
    m_album = album;
    m_art = art;
    m_mediaPlaying = playing;
    m_posMs = posMs;
    m_lenMs = lenMs;
    m_posClock.start();
}

void CarScreen::touch(int action, int x, int y)
{
    m_touch = QPoint(x, y);
    if (m_page == Phone && m_phoneDetail < 0 && !callFullScreen()) {
        if (action == 0) {
            m_dragStartY = y;
            m_dragStartScroll = m_phoneScroll;
            m_dragMoved = false;
        } else if (action == 2 && m_phoneList.contains(QPoint(x, m_dragStartY))) {
            if (qAbs(y - m_dragStartY) > 12) m_dragMoved = true;
            if (m_dragMoved) m_phoneScroll = m_dragStartScroll - (y - m_dragStartY);
        }
    }
    if (action == 0) { // press
        m_pressed = true;
        m_trail.clear();
        m_pressedTile = -1;
        m_pressedApp = -1;
        if (m_page == Home)
            for (int i = 0; i < m_tiles.size(); ++i)
                if (m_tiles[i].rect.contains(m_touch)) m_pressedTile = i;
        if (m_page == Apps)
            for (int i = 0; i < m_appRects.size(); ++i)
                if (m_appRects[i].contains(m_touch)) m_pressedApp = i;
    } else if (action == 1) { // release
        m_pressed = false;
        for (const CallButton &b : m_callButtons) {
            if (b.rect.contains(m_touch)) {
                emit callAction(b.action);
                m_pressedTile = -1;
                m_pressedApp = -1;
                return;
            }
        }
        if (callFullScreen()) return; // nothing else is reachable while it rings
        if (!m_banners.isEmpty() && m_bannerRect.contains(m_touch)) {
            m_banners.removeFirst();
            m_bannerClock.start();
            m_pressedTile = -1;
            m_pressedApp = -1;
            return;
        }
        if (m_page == Home) {
            if (m_pressedTile >= 0 && m_tiles[m_pressedTile].rect.contains(m_touch)) {
                m_page = m_tiles[m_pressedTile].page;
                emit tapped(m_tiles[m_pressedTile].title);
            }
        } else if (m_backButton.contains(m_touch)) {
            if (m_page == Navigation && m_navView != 0) {
                m_navView = 0;
            } else if (m_page == Navigation && m_map && m_navChoosing) {
                m_navChoosing = false;
            } else if (m_page == Music && m_musicListOn) {
                m_musicListOn = false;
            } else if (m_page == Phone && m_phoneDetail >= 0) {
                m_phoneDetail = -1; // back to the list first
            } else {
                m_page = Home;
                emit tapped(QStringLiteral("Zurück"));
            }
        } else if (m_page == Phone) {
            if (!m_dragMoved) phoneTap(m_touch);
            m_dragMoved = false;
        } else if (m_page == Navigation) {
            for (const NavButton &b : m_navButtons)
                if (b.rect.contains(m_touch)) {
                    navButtonTapped(b);
                    break;
                }
        } else if (m_page == Apps) {
            if (m_appPrev.contains(m_touch) && m_appPage > 0) {
                --m_appPage;
                layoutApps();
            } else if (m_appNext.contains(m_touch) && (m_appPage + 1) * 8 < m_apps.size()) {
                ++m_appPage;
                layoutApps();
            } else if (m_pressedApp >= 0 && m_pressedApp < m_appRects.size() && m_appRects[m_pressedApp].contains(m_touch)) {
                const CarApp &a = m_apps[m_appPage * 8 + m_pressedApp];
                emit launchApp(a.id);
                if (a.media) m_page = Music; // its player shows up there
            }
            m_pressedApp = -1;
        } else if (m_page == Music && m_hasPlayer && m_musicListOn) {
            if (m_mlUp.contains(m_touch)) --m_musicListPage;
            else if (m_mlDown.contains(m_touch)) ++m_musicListPage;
            else
                for (const auto &row : m_musicRows)
                    if (row.first.contains(m_touch)) {
                        emit musicListPicked(row.second);
                        m_musicListOn = false;
                        break;
                    }
        } else if (m_page == Music && m_hasPlayer && m_listButton.contains(m_touch)) {
            emit musicListRequested();
        } else if (m_page == Music && m_hasPlayer) {
            if (m_playButton.contains(m_touch)) emit mediaCommand(QStringLiteral("playpause"));
            else if (m_prevButton.contains(m_touch)) emit mediaCommand(QStringLiteral("previous"));
            else if (m_nextButton.contains(m_touch)) emit mediaCommand(QStringLiteral("next"));
            else if (m_lenMs > 0 && m_progressRect.adjusted(0, -24, 0, 24).contains(m_touch))
                emit mediaSeek(double(m_touch.x() - m_progressRect.left()) / qMax(1, m_progressRect.width()));
        } else if (m_page == Music && m_playButton.contains(m_touch)) {
            // No player on the phone: the button runs the sound test.
            m_playing = !m_playing;
            emit tapped(m_playing ? QStringLiteral("Play") : QStringLiteral("Pause"));
        }
        m_pressedTile = -1;
    }
    if (m_pressed) {
        m_trail.append(m_touch);
        if (m_trail.size() > 40) m_trail.remove(0);
    }
}

void CarScreen::drawTopBar(QPainter &p)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    QFont f = p.font();
    f.setPixelSize(bar / 2);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    const QRect r(m_safe.left(), m_safe.top(), m_safe.width() - m_safe.width() / 40, bar);
    const QString clock = QTime::currentTime().toString(QStringLiteral("HH:mm"));
    p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, clock);
    const int clockW = QFontMetrics(f).width(clock);
    drawStatusIcons(p, r.right() - clockW - bar / 3, r.top(), bar);
    p.setPen(c.accent);
    p.setFont(f);
    if (m_nav.running && m_page != Navigation) {
        // Next maneuver, small, left in the bar (right of the back button).
        const int x0 = m_page == Home ? r.left() + m_safe.width() / 40 : m_backButton.right() + bar / 3;
        drawManeuver(p, QRect(x0, r.top() + bar / 8, bar * 3 / 4, bar * 3 / 4), true);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(QRect(x0 + bar, r.top(), m_safe.width() / 3, bar), Qt::AlignLeft | Qt::AlignVCenter, m_nav.manDist);
    } else if (m_page == Home) {
        p.drawText(r.adjusted(m_safe.width() / 40, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SailAuto"));
    }
}

int CarScreen::drawStatusIcons(QPainter &p, int right, int top, int height)
{
    const Palette c = palette(m_night);
    const int h = height * 2 / 5;          // icon height
    const int cy = top + height / 2;
    const int gap = height / 3;
    int x = right;
    QFont small = p.font();
    small.setPixelSize(height * 3 / 10);
    small.setBold(true);
    const QFontMetrics fm(small);
    p.setFont(small);

    // Battery: outline, fill by level (red < 20 %, green while charging), percent left of it.
    if (m_phone.battery >= 0) {
        const int w = h * 2;
        const QRect body(x - w - 3, cy - h / 2, w, h);
        p.setPen(QPen(c.text, 2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(body, 3, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(c.text);
        p.drawRect(QRect(body.right() + 1, cy - h / 5, 3, h * 2 / 5));
        const QColor fill = m_phone.charging ? QColor(60, 200, 90)
                            : m_phone.battery < 20 ? QColor(230, 60, 50) : c.text;
        p.setBrush(fill);
        const QRect inner = body.adjusted(3, 3, -3, -3);
        p.drawRect(QRect(inner.left(), inner.top(), inner.width() * m_phone.battery / 100, inner.height()));
        if (m_phone.charging) { // lightning bolt
            p.setBrush(c.bg);
            const QPoint o = body.center();
            QPolygon bolt;
            bolt << QPoint(o.x() + 2, o.y() - h / 2 + 2) << QPoint(o.x() - 5, o.y() + 1) << QPoint(o.x(), o.y() + 1)
                 << QPoint(o.x() - 2, o.y() + h / 2 - 2) << QPoint(o.x() + 5, o.y() - 1) << QPoint(o.x(), o.y() - 1);
            p.drawPolygon(bolt);
        }
        const QString pct = QString::number(m_phone.battery) + QLatin1Char('%');
        const int tw = fm.width(pct);
        x = body.left() - 4 - tw;
        p.setPen(c.text);
        p.drawText(QRect(x, top, tw, height), Qt::AlignVCenter | Qt::AlignLeft, pct);
        x -= gap;
    }

    // Mobile: 4 bars + technology, dimmed bars when no signal.
    if (m_phone.signal >= 0) {
        const int bw = qMax(3, h / 5);
        const int bars = m_phone.signal >= 80 ? 4 : m_phone.signal >= 55 ? 3 : m_phone.signal >= 30 ? 2 : m_phone.signal > 5 ? 1 : 0;
        const int w = 4 * bw + 3 * 2;
        int bx = x - w;
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            const int bh = h * (i + 1) / 4;
            p.setBrush(i < bars ? c.text : c.panel);
            p.drawRect(QRect(bx + i * (bw + 2), cy + h / 2 - bh, bw, bh));
        }
        x = bx - 4;
        const QString t = m_phone.tech + (m_phone.roaming ? QStringLiteral(" R") : QString());
        if (!t.isEmpty()) {
            const int tw = fm.width(t);
            x -= tw;
            p.setPen(c.text);
            p.drawText(QRect(x, top, tw, height), Qt::AlignVCenter | Qt::AlignLeft, t);
        }
        x -= gap;
    }

    // Wi-Fi: three arcs above a dot, lit by strength.
    if (m_phone.wifi) {
        const int w = h + 4;
        const QPoint o(x - w / 2, cy + h / 2);
        const int lit = m_phone.wifiStrength >= 60 ? 3 : m_phone.wifiStrength >= 30 ? 2 : 1;
        p.setBrush(Qt::NoBrush);
        for (int i = 1; i <= 3; ++i) {
            const int rad = h * i / 3;
            p.setPen(QPen(i <= lit ? c.text : c.panel, qMax(2, h / 8), Qt::SolidLine, Qt::RoundCap));
            p.drawArc(QRect(o.x() - rad, o.y() - rad, 2 * rad, 2 * rad), 45 * 16, 90 * 16);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(c.text);
        p.drawEllipse(o, 2, 2);
        x -= w + gap;
    }

    // Bluetooth rune, accent when a device is connected (e.g. the car).
    if (m_phone.btOn) {
        const int w = h / 2 + 2;
        const int l = x - w;
        const QColor col = m_phone.btConnected ? c.accent : c.dim;
        p.setPen(QPen(col, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        const int t = cy - h / 2, b = cy + h / 2, mx = l + w / 2;
        QPolygon rune;
        rune << QPoint(l, cy - h / 4) << QPoint(l + w, cy + h / 4) << QPoint(mx, b) << QPoint(mx, t)
             << QPoint(l + w, cy - h / 4) << QPoint(l, cy + h / 4);
        p.drawPolyline(rune);
        x = l - gap;
    }
    return x;
}

void CarScreen::drawHome(QPainter &p)
{
    const Palette c = palette(m_night);
    for (int i = 0; i < m_tiles.size(); ++i) {
        const Tile &t = m_tiles[i];
        p.setPen(Qt::NoPen);
        p.setBrush(i == m_pressedTile ? c.panelPressed : c.panel);
        p.drawRoundedRect(t.rect, 18, 18);
        QFont f = p.font();
        f.setPixelSize(t.rect.width() / 3);
        p.setFont(f);
        p.setPen(c.accent);
        if (t.page == Apps) { // 3x3 grid of dots
            const QRect g = t.rect.adjusted(0, 0, 0, -t.rect.height() / 4);
            const int d = t.rect.width() / 12, step = d * 2;
            p.setPen(Qt::NoPen);
            p.setBrush(c.accent);
            for (int yy = -1; yy <= 1; ++yy)
                for (int xx = -1; xx <= 1; ++xx)
                    p.drawRoundedRect(QRect(g.center().x() + xx * step - d / 2, g.center().y() + yy * step - d / 2, d, d), 3, 3);
        } else {
            p.drawText(t.rect.adjusted(0, 0, 0, -t.rect.height() / 4), Qt::AlignCenter, t.glyph);
        }
        f.setPixelSize(qMax(12, t.rect.width() / 9));
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(t.rect.adjusted(0, t.rect.height() / 2, 0, 0), Qt::AlignCenter, t.title);
    }
}

void CarScreen::drawPage(QPainter &p)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    p.setPen(Qt::NoPen);
    p.setBrush(c.panel);
    p.drawRoundedRect(m_backButton, 12, 12);
    QFont f = p.font();
    f.setPixelSize(bar / 2);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    p.drawText(m_backButton, Qt::AlignCenter, QStringLiteral("← Zurück"));

    const QRect body = m_safe.adjusted(m_safe.width() / 20, bar + 10, -m_safe.width() / 20, -10);
    f.setPixelSize(bar * 2 / 3);
    p.setFont(f);
    switch (m_page) {
    case Navigation:
        drawNavPage(p, body);
        break;
    case Music: {
        auto drawRound = [&](const QRect &r, bool accent) {
            p.setPen(Qt::NoPen);
            p.setBrush(accent ? c.accent : c.panel);
            p.drawEllipse(r);
        };
        auto drawPlayPause = [&](const QRect &btn, bool showPause) {
            p.setBrush(c.bg);
            const QRect r = btn.adjusted(btn.width() / 3, btn.height() / 4, -btn.width() / 4, -btn.height() / 4);
            if (showPause) {
                p.drawRect(QRect(r.left(), r.top(), r.width() / 3, r.height()));
                p.drawRect(QRect(r.right() - r.width() / 3, r.top(), r.width() / 3, r.height()));
            } else {
                QPolygon tri;
                tri << r.topLeft() << QPoint(r.right(), r.center().y()) << r.bottomLeft();
                p.drawPolygon(tri);
            }
        };
        auto drawSkip = [&](const QRect &btn, bool forward) {
            p.setBrush(c.text);
            const QRect r = btn.adjusted(btn.width() / 4, btn.height() / 3, -btn.width() / 4, -btn.height() / 3);
            const int half = r.width() / 2;
            for (int k = 0; k < 2; ++k) {
                QPolygon tri;
                const int x0 = r.left() + k * half;
                if (forward) tri << QPoint(x0, r.top()) << QPoint(x0 + half, r.center().y()) << QPoint(x0, r.bottom());
                else tri << QPoint(x0 + half, r.top()) << QPoint(x0, r.center().y()) << QPoint(x0 + half, r.bottom());
                p.drawPolygon(tri);
            }
        };

        if (m_hasPlayer && m_musicListOn) {
            drawMusicList(p, body);
            break;
        }
        m_listButton = QRect();
        if (!m_hasPlayer) {
            p.setPen(c.text);
            p.drawText(body.adjusted(0, 0, 0, -m_playButton.height()), Qt::AlignCenter | Qt::TextWordWrap,
                       QStringLiteral("Kein Musik-Player aktiv.\nMusik am Telefon starten.\n(Knopf: Tontest)"));
            drawRound(m_playButton, true);
            drawPlayPause(m_playButton, m_playing);
            break;
        }
        // Cover left, text right, progress, buttons.
        const int artSize = qMin(body.height() - m_playButton.height() - 30, body.width() / 3);
        const QRect artRect(body.left(), body.top() + 5, artSize, artSize);
        if (!m_art.isNull()) {
            p.drawImage(artRect, m_art);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(artRect, 12, 12);
            QFont g = p.font();
            g.setPixelSize(artSize / 2);
            p.setFont(g);
            p.setPen(c.accent);
            p.drawText(artRect, Qt::AlignCenter, QString::fromUtf8("\u266B"));
        }
        const QRect textRect(artRect.right() + 20, artRect.top(), body.right() - artRect.right() - 20, artSize);
        QFont t = p.font();
        t.setPixelSize(bar * 2 / 3);
        t.setBold(true);
        p.setFont(t);
        p.setPen(c.text);
        const QFontMetrics fmTitle(t);
        drawFit(p, textRect.adjusted(0, 0, 0, -artSize * 2 / 3), Qt::AlignLeft | Qt::AlignVCenter,
                   m_title.isEmpty() ? QStringLiteral("(ohne Titel)") : m_title);
        t.setBold(false);
        t.setPixelSize(bar / 2);
        p.setFont(t);
        const QFontMetrics fm(t);
        drawFit(p, textRect.adjusted(0, artSize / 3, 0, -artSize / 3), Qt::AlignLeft | Qt::AlignVCenter,
                   m_artist);
        p.setPen(c.dim);
        drawFit(p, textRect.adjusted(0, artSize * 2 / 3, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   m_album.isEmpty() ? m_player : m_album + QStringLiteral(" · ") + m_player);
        // Progress bar (position extrapolated between polls while playing) and
        // time. Some players (podcasts) give no length: then only the time.
        {
            qint64 pos = m_posMs + (m_mediaPlaying && m_posClock.isValid() ? m_posClock.elapsed() : 0);
            if (m_lenMs > 0) pos = qMin(pos, m_lenMs);
            auto fmt = [](qint64 ms) {
                const qint64 sec = ms / 1000;
                return sec >= 3600 ? QStringLiteral("%1:%2:%3").arg(sec / 3600).arg((sec / 60) % 60, 2, 10, QLatin1Char('0'))
                                         .arg(sec % 60, 2, 10, QLatin1Char('0'))
                                   : QStringLiteral("%1:%2").arg(sec / 60).arg(sec % 60, 2, 10, QLatin1Char('0'));
            };
            const QRect bar2(body.left(), artRect.bottom() + 12, body.width(), 6);
            if (m_lenMs > 0) {
                m_progressRect = bar2;
                p.setPen(Qt::NoPen);
                p.setBrush(c.panel);
                p.drawRoundedRect(bar2, 3, 3);
                p.setBrush(c.accent);
                p.drawRoundedRect(QRect(bar2.left(), bar2.top(), int(bar2.width() * pos / m_lenMs), bar2.height()), 3, 3);
            } else {
                m_progressRect = QRect();
            }
            if (pos > 0 || m_lenMs > 0) {
                QFont tf = p.font();
                tf.setPixelSize(bar * 3 / 10);
                tf.setBold(false);
                p.setFont(tf);
                p.setPen(c.dim);
                const QRect tr(bar2.left(), bar2.bottom() + 4, bar2.width(), bar / 2);
                p.drawText(tr, Qt::AlignLeft | Qt::AlignTop, fmt(pos));
                if (m_lenMs > 0)
                    p.drawText(tr, Qt::AlignRight | Qt::AlignTop, QStringLiteral("-") + fmt(m_lenMs - pos));
            }
        }
        drawRound(m_prevButton, false);
        drawSkip(m_prevButton, false);
        drawRound(m_nextButton, false);
        drawSkip(m_nextButton, true);
        drawRound(m_playButton, true);
        drawPlayPause(m_playButton, m_mediaPlaying);
        // "Liste": playlists / track list of the player (if it offers them)
        m_listButton = QRect(m_nextButton.right() + m_nextButton.width() / 2, m_nextButton.top() + m_nextButton.height() / 6,
                             qMin(m_safe.right() - 10 - (m_nextButton.right() + m_nextButton.width() / 2), bar * 3),
                             m_nextButton.height() * 2 / 3);
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(m_listButton, 12, 12);
        {
            QFont lf = p.font();
            lf.setPixelSize(bar * 2 / 5);
            lf.setBold(true);
            p.setFont(lf);
            p.setPen(c.text);
            p.drawText(m_listButton, Qt::AlignCenter, QStringLiteral("Liste"));
        }
        break;
    }
    case Apps: {
        if (m_apps.isEmpty()) {
            p.setPen(c.text);
            p.drawText(body, Qt::AlignCenter | Qt::TextWordWrap,
                       QStringLiteral("Keine Apps gewählt.\nAm Telefon: SailAuto → Apps fürs Auto"));
            break;
        }
        QFont nf = p.font();
        nf.setPixelSize(bar * 2 / 5);
        nf.setBold(true);
        const QFontMetrics nfm(nf);
        for (int i = 0; i < m_appRects.size(); ++i) {
            const QRect r = m_appRects[i];
            const CarApp &a = m_apps[m_appPage * 8 + i];
            p.setPen(Qt::NoPen);
            p.setBrush(i == m_pressedApp ? c.panelPressed : c.panel);
            p.drawRoundedRect(r, 14, 14);
            const int is = qMin(r.width(), r.height()) * 11 / 20;
            const QRect ir(r.center().x() - is / 2, r.top() + r.height() / 10, is, is);
            if (!a.icon.isNull()) {
                p.drawImage(ir, a.icon);
            } else {
                p.setBrush(c.accent);
                p.drawEllipse(ir);
                p.setPen(c.bg);
                QFont lf = nf;
                lf.setPixelSize(is / 2);
                p.setFont(lf);
                p.drawText(ir, Qt::AlignCenter, a.name.left(1).toUpper());
            }
            p.setFont(nf);
            p.setPen(c.text);
            const QRect tr(r.left() + 6, ir.bottom() + 4, r.width() - 12, r.bottom() - ir.bottom() - 6);
            drawFit(p, tr, Qt::AlignCenter, a.name);
        }
        if (!m_appPrev.isNull()) {
            const int pages = (m_apps.size() + 7) / 8;
            for (const QRect &ar : {m_appPrev, m_appNext}) {
                const bool up = ar == m_appPrev;
                const bool enabled = up ? m_appPage > 0 : m_appPage + 1 < pages;
                p.setPen(Qt::NoPen);
                p.setBrush(c.panel);
                p.drawRoundedRect(ar, 12, 12);
                p.setBrush(enabled ? c.text : c.dim);
                const QPoint o = ar.center();
                const int k = ar.width() / 4;
                QPolygon tri;
                if (up) tri << QPoint(o.x() - k, o.y() + k / 2) << QPoint(o.x() + k, o.y() + k / 2) << QPoint(o.x(), o.y() - k / 2);
                else tri << QPoint(o.x() - k, o.y() - k / 2) << QPoint(o.x() + k, o.y() - k / 2) << QPoint(o.x(), o.y() + k / 2);
                p.drawPolygon(tri);
            }
        }
        break;
    }
    case Phone:
        drawPhonePage(p, body);
        break;
    case Info: {
        p.setPen(c.text);
        f.setPixelSize(bar / 2);
        f.setBold(false);
        p.setFont(f);
        const QString txt = QStringLiteral("SailAuto %10\nHead Unit: %1\nVideo: %2x%3, sicher %4x%5\nEncoder: %6\n%7 fps, %8 kbit/s\nNachtmodus: %9")
                                .arg(m_headUnit.isEmpty() ? QStringLiteral("?") : m_headUnit)
                                .arg(m_image.width()).arg(m_image.height())
                                .arg(m_safe.width()).arg(m_safe.height())
                                .arg(m_encoder)
                                .arg(m_fps, 0, 'f', 1).arg(m_kbps, 0, 'f', 0)
                                .arg(m_night ? QStringLiteral("an") : QStringLiteral("aus"))
                                .arg(m_version);
        p.drawText(body, Qt::AlignLeft | Qt::AlignVCenter, txt);
        break;
    }
    case Home:
        break;
    }
}

const QImage &CarScreen::render()
{
    ++m_frame;
    const Palette c = palette(m_night);
    m_image.fill(Qt::black);
    QPainter p(&m_image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.fillRect(m_safe, c.bg);
    drawTopBar(p);
    if (m_page == Home) drawHome(p);
    else drawPage(p);

    drawBanner(p);
    drawCall(p);

    // Touch feedback: proves the input path end to end.
    if (!m_trail.isEmpty()) {
        p.setPen(QPen(c.accent, 4, Qt::SolidLine, Qt::RoundCap));
        for (int i = 1; i < m_trail.size(); ++i) p.drawLine(m_trail[i - 1], m_trail[i]);
    }
    if (m_pressed) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 90));
        p.drawEllipse(m_touch, 22, 22);
    }
    return m_image;
}
