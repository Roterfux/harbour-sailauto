#include "carscreen.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QtMath>


static void drawTileIcon(QPainter &p, const QRectF &r, int page, const QColor &col);
static void drawMdi(QPainter &p, const QRectF &r, const QString &name, const QColor &col);

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
    // Navigation, Musik, Telefon big; Info narrow on the right.
    const struct { Page p; const char *t; const char *g; double weight; } defs[] = {
        {Navigation, "Navigation", "⮝", 1.0}, {Music, "Musik", "♫", 1.0}, {Phone, "Telefon", "☎", 1.0},
    };
    const int n = 3; // Info: icon in the top bar
    double total = 0;
    for (const auto &d : defs) total += d.weight;
    const double unit = (area.width() - (n - 1) * gap) / total;
    const int h = area.height();
    double x = area.left();
    for (int i = 0; i < n; ++i) {
        const int w = int(unit * defs[i].weight);
        m_tiles.append({QRect(int(x), area.top(), w, h), defs[i].p, QString::fromUtf8(defs[i].t), QString::fromUtf8(defs[i].g)});
        x += w + gap;
    }
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
    {
        const QRect stopR(body.right() - bar * 3, body.top(), bar * 3, bar);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(200, 60, 50));
        p.drawRoundedRect(stopR, 12, 12);
        f.setPixelSize(bar * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(stopR, Qt::AlignCenter, QStringLiteral("Beenden"));
        m_navButtons.append({stopR, QStringLiteral("stopnav"), QString()});
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
        // Two cards of the same size: turn (top left), destination (bottom left)
        const QSize cs(area.width() * 2 / 5, bar * 3 / 2);
        const QRect cardR(area.left() + 10, area.top() + 10, cs.width(), cs.height());
        const QRect strip(area.left() + 10, area.bottom() - cs.height() - 10, cs.width(), cs.height());
        p.setPen(Qt::NoPen);
        p.setBrush(card);
        p.drawRoundedRect(cardR, 14, 14);
        p.drawRoundedRect(strip, 14, 14);
        const int is = cs.height() - 16;
        drawManeuver(p, QRect(cardR.left() + 8, cardR.top() + 8, is, is), false);
        const QRect tr(cardR.left() + is + 18, cardR.top() + 6, cardR.width() - is - 26, cardR.height() - 12);
        f.setPixelSize(cs.height() * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        drawFit(p, QRect(tr.left(), tr.top(), tr.width(), tr.height() * 11 / 20), Qt::AlignLeft | Qt::AlignVCenter, m_nav.manDist);
        f.setPixelSize(cs.height() / 4);
        f.setBold(false);
        p.setFont(f);
        p.setPen(c.dim);
        const QString street = m_nav.street.isEmpty() ? m_nav.narrative : m_nav.street;
        drawFit(p, QRect(tr.left(), tr.top() + tr.height() * 11 / 20, tr.width(), tr.height() * 9 / 20),
                Qt::AlignLeft | Qt::AlignVCenter, street);
        // Destination card: remaining distance · time, arrival below, progress line
        const QRect sr = strip.adjusted(16, 6, -16, -10);
        f.setPixelSize(cs.height() * 2 / 5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        drawFit(p, QRect(sr.left(), sr.top(), sr.width(), sr.height() * 11 / 20), Qt::AlignLeft | Qt::AlignVCenter,
                QStringLiteral("%1 · %2").arg(m_nav.destDist, m_nav.destTime));
        f.setPixelSize(cs.height() / 4);
        f.setBold(false);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(QRect(sr.left(), sr.top() + sr.height() * 11 / 20, sr.width(), sr.height() * 9 / 20),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Ankunft %1").arg(m_nav.destEta));
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        p.drawRoundedRect(QRect(strip.left() + 12, strip.bottom() - 7, (strip.width() - 24) * qBound(0, m_nav.progress, 100) / 100, 4), 2, 2);
        if (!m_nav.alongRoute) {
            f.setBold(true);
            p.setFont(f);
            p.setPen(QColor(240, 170, 40));
            p.drawText(QRect(cardR.left(), cardR.bottom() + 4, cardR.width(), bar / 2), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("Abseits der Route"));
        }
        button(QRect(area.right() - bar * 3 - 10, area.top() + 10, bar * 3, bar), QStringLiteral("stopnav"),
               QStringLiteral("Beenden"), QColor(200, 60, 50), Qt::white);
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

    if (m_navView == 3) {
        // Search results, 4 per page
        if (m_searching || m_results.isEmpty()) {
            f.setPixelSize(bar / 2);
            f.setBold(true);
            p.setFont(f);
            p.setPen(m_searching ? c.text : c.dim);
            p.drawText(body, Qt::AlignCenter | Qt::TextWordWrap,
                       m_searching ? QStringLiteral("Suche läuft") : m_resultNote);
            return;
        }
        const int per = 4;
        const int pages = (m_results.size() + per - 1) / per;
        m_resultPage = qBound(0, m_resultPage, pages - 1);
        const int btnW = bar + 10;
        const QRect list(body.left(), body.top(), body.width() - btnW - 10, body.height());
        const int rh = (list.height() - 3 * 8) / per;
        for (int k = 0; k < per && m_resultPage * per + k < m_results.size(); ++k) {
            const int idx = m_resultPage * per + k;
            const GeoResult &g = m_results[idx];
            const QRect r(list.left(), list.top() + k * (rh + 8), list.width(), rh);
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(r, 12, 12);
            const int mi = rh / 2;
            drawMdi(p, QRectF(r.left() + 12, r.center().y() - mi / 2.0, mi, mi), QStringLiteral("marker"), c.accent);
            line(r.adjusted(24 + mi, 4, -16, -rh / 2), g.title, c.text, qMin(px, rh / 3), true);
            line(r.adjusted(24 + mi, rh / 2, -16, -4), g.region, c.dim, qMin(px * 4 / 5, rh / 3), false);
            m_navButtons.append({r, QStringLiteral("pick"), QString::number(idx)});
        }
        button(QRect(list.right() + 10, body.top(), btnW, body.height() / 2 - 5), QStringLiteral("rpage"), QStringLiteral("-1"),
               QStringLiteral("▲"), c.panel, m_resultPage > 0 ? c.text : c.dim, px);
        button(QRect(list.right() + 10, body.center().y() + 5, btnW, body.height() / 2 - 5), QStringLiteral("rpage"), QStringLiteral("1"),
               QStringLiteral("▼"), c.panel, m_resultPage + 1 < pages ? c.text : c.dim, px);
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
                             : m_navSearchSent ? QStringLiteral("Am Telefon in Pure Maps „Navigieren“ antippen, dann hier starten.")
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
    auto iconButton = [&](const QRect &r, const QString &kind, const QString &value, const QString &label,
                          const QString &icon, bool enabled) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.panelPressed);
        p.drawRoundedRect(r, 12, 12);
        const int is = r.height() * 2 / 5;
        drawMdi(p, QRectF(r.center().x() - is / 2.0, r.top() + r.height() / 8, is, is), icon, enabled ? c.accent : c.dim);
        f.setPixelSize(qMin(px, r.height() / 4));
        f.setBold(true);
        p.setFont(f);
        p.setPen(enabled ? c.text : c.dim);
        drawFit(p, QRect(r.left() + 6, r.top() + r.height() / 8 + is + 2, r.width() - 12, r.bottom() - (r.top() + r.height() / 8 + is + 2)),
                Qt::AlignCenter, label);
        m_navButtons.append({r, kind, value});
    };
    const int bh2 = bh * 4 / 3;
    iconButton(QRect(body.left(), y, w, bh2), QStringLiteral("home"), m_home, QStringLiteral("Zuhause"), QStringLiteral("home"), !m_home.isEmpty());
    iconButton(QRect(body.left() + (w + gap), y, w, bh2), QStringLiteral("work"), m_work, QStringLiteral("Arbeit"), QStringLiteral("work"), !m_work.isEmpty());
    iconButton(QRect(body.left() + 2 * (w + gap), y, w, bh2), QStringLiteral("keyboard"), QString(), QStringLiteral("Adresse"),
               QStringLiteral("keyboard"), m_parked);
    iconButton(QRect(body.left() + 3 * (w + gap), y, w, bh2), QStringLiteral("contacts"), QString(), QStringLiteral("Kontakte"),
               QStringLiteral("account"), true);
    y += bh2 - bh;
    y += bh + 10;
    // Recent destinations
    const int rh = qMax(bar * 4 / 5, 40);
    for (const QString &d : m_recentDest) {
        if (y + rh > body.bottom()) break;
        const QRect r(body.left(), y, body.width(), rh);
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(r, 10, 10);
        drawMdi(p, QRectF(r.left() + 12, r.center().y() - rh * 0.3, rh * 0.6, rh * 0.6), QStringLiteral("history"), c.dim);
        line(r.adjusted(16 + int(rh * 0.6) + 8, 0, -16, 0), d, c.text, px * 9 / 10, false);
        m_navButtons.append({r, QStringLiteral("dest"), d});
        y += rh + 6;
    }
}

void CarScreen::navButtonTapped(const NavButton &b)
{
    if (b.kind == QLatin1String("stopnav")) {
        emit navStop();
        return;
    }
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
        m_navView = 3;
        m_searching = true;
        emit navSearch(b.value);
    } else if (b.kind == QLatin1String("keyboard")) {
        m_navView = 1;
    } else if (b.kind == QLatin1String("contacts")) {
        m_navView = 2;
        m_navContactPage = 0;
    } else if (b.kind == QLatin1String("pick")) {
        m_navView = 0;
        m_navChoosing = false;
        m_navSearchSent = true;
        emit navPick(b.value.toInt());
    } else if (b.kind == QLatin1String("rpage")) {
        m_resultPage += b.value.toInt();
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
        m_navView = 3;
        m_searching = true;
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
        const QString title = it.title.isEmpty() ? QStringLiteral("(ohne Titel)") : it.title;
        int textLeft = 14;
        if (!it.icon.isEmpty()) {
            const int is = rh / 2;
            drawMdi(p, QRectF(r.left() + 14, r.center().y() - is / 2.0, is, is), it.icon,
                    it.icon == QLatin1String("close") ? QColor(220, 90, 80) : c.accent);
            textLeft = 14 + is + 14;
        }
        const QRect tr = r.adjusted(textLeft, 0, -14, it.sub.isEmpty() ? 0 : -rh / 3);
        drawFit(p, tr, Qt::AlignLeft | Qt::AlignVCenter, title);
        if (!it.sub.isEmpty()) {
            f.setBold(false);
            f.setPixelSize(qMin(bar * 3 / 10, rh * 3 / 10));
            p.setFont(f);
            p.setPen(c.dim);
            const QRect sr = r.adjusted(textLeft, rh * 2 / 3 - 4, -14, 0);
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

void CarScreen::drawWeatherIcon(QPainter &p, const QRectF &r, int code, bool day) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double u = r.width();
    const QColor sun(250, 200, 50), cloud = m_night ? QColor(170, 176, 190) : QColor(225, 230, 240);
    const QColor rain(90, 170, 250), snow(240, 245, 255), bolt(250, 210, 60);
    auto drawSun = [&](const QPointF &c, double rad) {
        p.setPen(QPen(sun, qMax(1.5, u / 18), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 8; ++i) {
            const double a = i * M_PI / 4;
            p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * rad * 1.35, c + QPointF(std::cos(a), std::sin(a)) * rad * 1.8);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(sun);
        p.drawEllipse(c, rad, rad);
    };
    auto drawMoon = [&](const QPointF &c, double rad) {
        QPainterPath m;
        m.addEllipse(c, rad, rad);
        QPainterPath cut;
        cut.addEllipse(c + QPointF(rad * 0.55, -rad * 0.35), rad * 0.9, rad * 0.9);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(235, 225, 160));
        p.drawPath(m.subtracted(cut));
    };
    auto drawCloud = [&](const QPointF &o, double s) {
        p.setPen(Qt::NoPen);
        p.setBrush(cloud);
        p.drawEllipse(QRectF(o.x(), o.y() + s * 0.25, s * 0.55, s * 0.45));
        p.drawEllipse(QRectF(o.x() + s * 0.25, o.y(), s * 0.55, s * 0.6));
        p.drawEllipse(QRectF(o.x() + s * 0.5, o.y() + s * 0.2, s * 0.5, s * 0.5));
        p.drawRoundedRect(QRectF(o.x() + s * 0.1, o.y() + s * 0.35, s * 0.85, s * 0.35), s * 0.15, s * 0.15);
    };
    const QPointF c = r.center();
    if (code == 0) {
        if (day) drawSun(c, u * 0.22); else drawMoon(c, u * 0.3);
    } else if (code <= 2) {
        if (day) drawSun(QPointF(r.left() + u * 0.38, r.top() + u * 0.36), u * 0.16);
        else drawMoon(QPointF(r.left() + u * 0.38, r.top() + u * 0.36), u * 0.2);
        drawCloud(QPointF(r.left() + u * 0.18, r.top() + u * 0.38), u * 0.78);
    } else if (code == 45 || code == 48) {
        p.setPen(QPen(cloud, qMax(2.0, u / 10), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 3; ++i)
            p.drawLine(QPointF(r.left() + u * (0.15 + 0.08 * (i % 2)), r.top() + u * (0.35 + 0.18 * i)),
                       QPointF(r.right() - u * (0.15 + 0.08 * ((i + 1) % 2)), r.top() + u * (0.35 + 0.18 * i)));
    } else {
        drawCloud(QPointF(r.left() + u * 0.08, r.top() + u * 0.12), u * 0.84);
        const bool isSnow = (code >= 71 && code <= 77) || code == 85 || code == 86;
        const bool isRain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
        if (isRain || isSnow) {
            for (int i = 0; i < 3; ++i) {
                const QPointF d(r.left() + u * (0.3 + 0.2 * i), r.top() + u * 0.78);
                if (isSnow) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(snow);
                    p.drawEllipse(d, u * 0.05, u * 0.05);
                } else {
                    p.setPen(QPen(rain, qMax(1.5, u / 16), Qt::SolidLine, Qt::RoundCap));
                    p.drawLine(d, d + QPointF(-u * 0.06, u * 0.14));
                }
            }
        } else if (code >= 95) {
            p.setPen(Qt::NoPen);
            p.setBrush(bolt);
            QPolygonF b({QPointF(c.x() + u * 0.05, r.top() + u * 0.58), QPointF(c.x() - u * 0.12, r.top() + u * 0.82),
                         QPointF(c.x(), r.top() + u * 0.82), QPointF(c.x() - u * 0.06, r.bottom()),
                         QPointF(c.x() + u * 0.14, r.top() + u * 0.74), QPointF(c.x() + u * 0.02, r.top() + u * 0.74)});
            p.drawPolygon(b);
        }
    }
    p.restore();
}

void CarScreen::drawWeatherPage(QPainter &p, const QRect &body)
{
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    QFont f = p.font();
    if (!m_weather.valid) {
        f.setPixelSize(bar / 2);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(body, Qt::AlignCenter | Qt::TextWordWrap,
                   QStringLiteral("Noch kein Wetter.\nBraucht die Position des Autos und Internet."));
        return;
    }
    // Now: big icon, temperature, description
    const int big = body.height() * 2 / 5;
    drawWeatherIcon(p, QRectF(body.left(), body.top(), big, big), m_weather.code, m_weather.day);
    f.setPixelSize(bar);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    const QRect tr(body.left() + big + 20, body.top(), body.width() - big - 20, big);
    p.drawText(tr.adjusted(0, 0, 0, -big / 2), Qt::AlignLeft | Qt::AlignBottom, QString::number(qRound(m_weather.temp)) + QStringLiteral(" °C"));
    f.setPixelSize(bar / 2);
    f.setBold(false);
    p.setFont(f);
    p.setPen(c.dim);
    p.drawText(tr.adjusted(0, big / 2 + 6, 0, 0), Qt::AlignLeft | Qt::AlignTop, WeatherClient::describe(m_weather.code));
    // Next hours (left) and days (right)
    const int rowTop = body.top() + big + 14;
    const int rowH = body.bottom() - rowTop;
    const QRect hoursR(body.left(), rowTop, body.width() * 3 / 5 - 8, rowH);
    const QRect daysR(hoursR.right() + 16, rowTop, body.right() - hoursR.right() - 16, rowH);
    auto cell = [&](const QRect &r, const QString &top, int code, bool day, const QString &bottom) {
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(r, 10, 10);
        f.setPixelSize(qMin(bar * 3 / 10, r.height() / 6));
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.dim);
        p.drawText(QRect(r.left(), r.top() + 4, r.width(), r.height() / 4), Qt::AlignCenter, top);
        const int is = qMin(r.width() - 8, r.height() * 2 / 5);
        drawWeatherIcon(p, QRectF(r.center().x() - is / 2.0, r.top() + r.height() / 4 + 2, is, is), code, day);
        p.setPen(c.text);
        f.setPixelSize(qMin(bar * 2 / 5, r.height() / 5));
        p.setFont(f);
        p.drawText(QRect(r.left(), r.bottom() - r.height() / 4 - 2, r.width(), r.height() / 4), Qt::AlignCenter, bottom);
    };
    const int nh = m_weather.hours.size();
    for (int i = 0; i < nh; ++i) {
        const int w = (hoursR.width() - (nh - 1) * 6) / qMax(1, nh);
        const auto &h = m_weather.hours[i];
        cell(QRect(hoursR.left() + i * (w + 6), hoursR.top(), w, hoursR.height()), QStringLiteral("%1 Uhr").arg(h.hour),
             h.code, h.hour >= 7 && h.hour < 19, QString::number(qRound(h.temp)) + QStringLiteral("°"));
    }
    static const char *dayNames[] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
    const int nd = m_weather.days.size();
    for (int i = 0; i < nd; ++i) {
        const int w = (daysR.width() - (nd - 1) * 6) / qMax(1, nd);
        const auto &d = m_weather.days[i];
        const QString name = i == 0 ? QStringLiteral("Heute") : QString::fromLatin1(dayNames[(d.date.dayOfWeek() + 6) % 7]);
        cell(QRect(daysR.left() + i * (w + 6), daysR.top(), w, daysR.height()), name, d.code, true,
             QStringLiteral("%1° / %2°").arg(qRound(d.max)).arg(qRound(d.min)));
    }
}

void CarScreen::showLoading(const QString &name, const QImage &icon)
{
    m_loadingName = name;
    m_loadingIcon = icon;
    m_loadingClock.start();
}

void CarScreen::drawLoading(QPainter &p)
{
    if (m_loadingName.isEmpty()) return;
    if (m_loadingClock.elapsed() > 15000) {
        m_loadingName.clear();
        return;
    }
    const Palette c = palette(m_night);
    const int bar = m_safe.height() / 8;
    const QRect area(m_safe.left(), m_safe.top() + bar, m_safe.width(), m_safe.height() - bar);
    p.fillRect(area, c.bg);
    const int is = area.height() / 3;
    const QRect iconR(area.center().x() - is / 2, area.top() + area.height() / 6, is, is);
    if (!m_loadingIcon.isNull()) p.drawImage(iconR, m_loadingIcon);
    // Spinner around the icon
    const int pad = is / 6;
    p.setPen(QPen(c.accent, qMax(4, is / 18), Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    const int start = int((m_loadingClock.elapsed() % 1200) * 360 / 1200);
    p.drawArc(iconR.adjusted(-pad, -pad, pad, pad), -start * 16, 100 * 16);
    QFont f = p.font();
    f.setPixelSize(bar / 2);
    f.setBold(true);
    p.setFont(f);
    p.setPen(c.text);
    drawFit(p, QRect(area.left() + 20, iconR.bottom() + pad + 16, area.width() - 40, bar), Qt::AlignCenter,
            m_loadingName + QStringLiteral(" wird gestartet"));
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
        if (!m_loadingName.isEmpty()) { // tap closes the loading screen
            m_loadingName.clear();
            m_pressedTile = -1;
            return;
        }
        if (m_infoRect.contains(m_touch)) {
            m_page = Info;
            m_pressedTile = -1;
            return;
        }
        if (m_musicBarRect.contains(m_touch)) {
            m_page = Music;
            m_pressedTile = -1;
            return;
        }
        if (m_weatherRect.contains(m_touch)) {
            m_page = WeatherPage;
            m_pressedTile = -1;
            emit tapped(QStringLiteral("Wetter"));
            return;
        }
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
                if (m_page == Navigation) { m_navChoosing = false; m_navView = 0; } // map first
                emit tapped(m_tiles[m_pressedTile].title);
            }
        } else if (m_backButton.contains(m_touch)) {
            if (m_page == Navigation && m_navView != 0) {
                m_navView = 0;
            } else if (m_page == Navigation && m_map && m_navChoosing) {
                m_navChoosing = false;
            } else if (m_page == Music && m_musicKb) {
                m_musicKb = false;
            } else if (m_page == Music && m_musicListOn) {
                emit musicListBack();
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
        } else if (m_page == Music && m_musicKb) {
            for (const NavButton &b : m_navButtons)
                if (b.rect.contains(m_touch)) {
                    if (b.kind == QLatin1String("search")) {
                        const QString t = m_navInput.trimmed();
                        if (!t.isEmpty()) { m_musicKb = false; m_navInput.clear(); emit radioSearch(t); }
                    } else if (b.kind == QLatin1String("cancel")) {
                        m_musicKb = false;
                    } else {
                        navButtonTapped(b); // keys, backspace
                    }
                    break;
                }
        } else if (m_page == Music && m_musicListOn) {
            if (m_mlUp.contains(m_touch)) --m_musicListPage;
            else if (m_mlDown.contains(m_touch)) ++m_musicListPage;
            else
                for (const auto &row : m_musicRows)
                    if (row.first.contains(m_touch)) {
                        m_musicListOn = false;          // the controller may open the next level
                        emit musicListPicked(row.second);
                        break;
                    }
        } else if (m_page == Music && m_listButton.contains(m_touch)) {
            emit musicListRequested();
        } else if (m_page == Music && m_hasPlayer) {
            if (m_playButton.contains(m_touch)) emit mediaCommand(QStringLiteral("playpause"));
            else if (m_prevButton.contains(m_touch)) emit mediaCommand(QStringLiteral("previous"));
            else if (m_nextButton.contains(m_touch)) emit mediaCommand(QStringLiteral("next"));
            else if (m_lenMs > 0 && m_progressRect.adjusted(0, -24, 0, 24).contains(m_touch))
                emit mediaSeek(double(m_touch.x() - m_progressRect.left()) / qMax(1, m_progressRect.width()));
        } else if (m_page == Music && !m_hasPlayer) {
            // No player: start one of the music apps
            for (const auto &b : m_musicAppButtons)
                if (b.first.contains(m_touch)) {
                    emit launchApp(b.second);
                    break;
                }
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
    const int iconsLeft = drawStatusIcons(p, r.right() - clockW - bar / 3, r.top(), bar);
    m_weatherRect = QRect();
    if (m_weather.valid) {
        QFont wf = f;
        wf.setPixelSize(bar * 2 / 5);
        p.setFont(wf);
        const QString t = QString::number(qRound(m_weather.temp)) + QStringLiteral("°");
        const int tw = QFontMetrics(wf).width(t);
        const int is = bar * 3 / 5;
        const int x0 = iconsLeft - tw - is - 6;
        drawWeatherIcon(p, QRectF(x0, r.top() + (bar - is) / 2.0, is, is), m_weather.code, m_weather.day);
        p.setPen(c.text);
        p.drawText(QRect(x0 + is + 4, r.top(), tw + 2, bar), Qt::AlignLeft | Qt::AlignVCenter, t);
        m_weatherRect = QRect(x0 - 6, r.top(), is + tw + 16, bar);
        p.setFont(f);
    }
    // Info: small icon left of the weather
    const int is = bar * 3 / 5;
    const int infoRight = (m_weatherRect.isNull() ? iconsLeft : m_weatherRect.left()) - bar / 4;
    m_infoRect = QRect(infoRight - is, r.top() + (bar - is) / 2, is, is);
    drawTileIcon(p, QRectF(m_infoRect), 0, c.dim);
    m_infoRect.adjust(-8, -8, 8, 8); // easier to hit
    p.setPen(c.accent);
    p.setFont(f);
    int leftEnd;
    if (m_nav.running && m_page != Navigation) {
        // Next maneuver, small, left in the bar (right of the back button).
        const int x0 = m_page == Home ? r.left() + m_safe.width() / 40 : m_backButton.right() + bar / 3;
        drawManeuver(p, QRect(x0, r.top() + bar / 8, bar * 3 / 4, bar * 3 / 4), true);
        p.setFont(f);
        p.setPen(c.text);
        p.drawText(QRect(x0 + bar, r.top(), m_safe.width() / 3, bar), Qt::AlignLeft | Qt::AlignVCenter, m_nav.manDist);
        leftEnd = x0 + bar + QFontMetrics(f).width(m_nav.manDist) + bar / 2;
    } else if (m_page == Home) {
        p.drawText(r.adjusted(m_safe.width() / 40, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SailAuto"));
        leftEnd = r.left() + m_safe.width() / 40 + QFontMetrics(f).width(QStringLiteral("SailAuto")) + bar / 2;
    } else {
        static const char *titles[] = {"", "Navigation", "Musik", "Apps", "Telefon", "Info", "Wetter"};
        QFont tf = f;
        tf.setPixelSize(bar * 9 / 20);
        leftEnd = m_backButton.right() + bar / 2 + QFontMetrics(tf).width(QString::fromUtf8(titles[m_page])) + bar / 2;
    }
    // Short music info (not on the music page)
    m_musicBarRect = QRect();
    if (m_hasPlayer && !m_title.isEmpty() && m_page != Music && m_mediaPlaying) {
        const QRect mr(leftEnd, r.top(), m_infoRect.left() - 8 - leftEnd, bar);
        if (mr.width() > bar * 2) {
            QFont mf = f;
            mf.setPixelSize(bar * 2 / 5);
            mf.setBold(false);
            p.setFont(mf);
            p.setPen(c.dim);
            drawFit(p, mr, Qt::AlignLeft | Qt::AlignVCenter,
                    QString::fromUtf8("\u266B ") + m_title + (m_artist.isEmpty() ? QString() : QStringLiteral(" · ") + m_artist));
            m_musicBarRect = mr;
            p.setFont(f);
        }
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

    // Battery, compact: percent inside the outline (green = charging, red < 20 %).
    if (m_phone.battery >= 0) {
        const QString pct = QString::number(m_phone.battery);
        QFont bf = small;
        bf.setPixelSize(h * 7 / 10);
        const int w = QFontMetrics(bf).width(QStringLiteral("100")) + 8;
        const QRect body(x - w - 3, cy - h / 2, w, h);
        const QColor fill = m_phone.charging ? QColor(60, 200, 90)
                            : m_phone.battery < 20 ? QColor(230, 60, 50) : c.panelPressed;
        p.setPen(Qt::NoPen);
        p.setBrush(c.panel);
        p.drawRoundedRect(body, 3, 3);
        p.setBrush(fill);
        p.drawRoundedRect(QRect(body.left(), body.top(), body.width() * m_phone.battery / 100, body.height()), 3, 3);
        p.setBrush(c.text);
        p.drawRect(QRect(body.right() + 1, cy - h / 5, 2, h * 2 / 5));
        p.setFont(bf);
        p.setPen(c.text);
        p.drawText(body, Qt::AlignCenter, pct);
        p.setFont(small);
        x = body.left() - gap;
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

    return x;
}

// Icons: Material Design Icons (Apache License 2.0), 24x24 paths.
static void drawMdi(QPainter &p, const QRectF &r, const QString &name, const QColor &col)
{
    static const QHash<QString, const char *> paths = {
        {QStringLiteral("info"), "M11,9H13V7H11M12,20C7.59,20 4,16.41 4,12C4,7.59 7.59,4 12,4C16.41,4 20,7.59 20,12C20,16.41 16.41,20 12,20M12,2A10,10 0 0,0 2,12A10,10 0 0,0 12,22A10,10 0 0,0 22,12A10,10 0 0,0 12,2M11,17H13V11H11V17Z"},
        {QStringLiteral("navigation"), "M12,2L4.5,20.29L5.21,21L12,18L18.79,21L19.5,20.29L12,2Z"},
        {QStringLiteral("music"), "M21,3V15.5A3.5,3.5 0 0,1 17.5,19A3.5,3.5 0 0,1 14,15.5A3.5,3.5 0 0,1 17.5,12C18.04,12 18.55,12.12 19,12.34V6.47L9,8.6V17.5A3.5,3.5 0 0,1 5.5,21A3.5,3.5 0 0,1 2,17.5A3.5,3.5 0 0,1 5.5,14C6.04,14 6.55,14.12 7,14.34V6L21,3Z"},
        {QStringLiteral("phone"), "M6.62,10.79C8.06,13.62 10.38,15.94 13.21,17.38L15.41,15.18C15.69,14.9 16.08,14.82 16.43,14.93C17.55,15.3 18.75,15.5 20,15.5A1,1 0 0,1 21,16.5V20A1,1 0 0,1 20,21A17,17 0 0,1 3,4A1,1 0 0,1 4,3H7.5A1,1 0 0,1 8.5,4C8.5,5.25 8.7,6.45 9.07,7.57C9.18,7.92 9.1,8.31 8.82,8.59L6.62,10.79Z"},
        {QStringLiteral("radio"), "M3.24,6.15C2.51,6.43 2,7.17 2,8V20A2,2 0 0,0 4,22H20A2,2 0 0,0 22,20V8C22,6.89 21.1,6 20,6H8.3L16.57,2.66L15.88,1L3.24,6.15M7,20A3,3 0 0,1 4,17A3,3 0 0,1 7,14A3,3 0 0,1 10,17A3,3 0 0,1 7,20M20,12H18V10H16V12H4V8H20V12Z"},
        {QStringLiteral("search"), "M9.5,3A6.5,6.5 0 0,1 16,9.5C16,11.11 15.41,12.59 14.44,13.73L14.71,14H15.5L20.5,19L19,20.5L14,15.5V14.71L13.73,14.44C12.59,15.41 11.11,16 9.5,16A6.5,6.5 0 0,1 3,9.5A6.5,6.5 0 0,1 9.5,3M9.5,5C7,5 5,7 5,9.5C5,12 7,14 9.5,14C12,14 14,12 14,9.5C14,7 12,5 9.5,5Z"},
        {QStringLiteral("folder"), "M10,4H4C2.89,4 2,4.89 2,6V18A2,2 0 0,0 4,20H20A2,2 0 0,0 22,18V8C22,6.89 21.1,6 20,6H12L10,4Z"},
        {QStringLiteral("note"), "M12,3V13.55C11.41,13.21 10.73,13 10,13C7.79,13 6,14.79 6,17C6,19.21 7.79,21 10,21C12.21,21 14,19.21 14,17V7H18V3H12Z"},
        {QStringLiteral("podcast"), "M12,2A3,3 0 0,1 15,5V11A3,3 0 0,1 12,14A3,3 0 0,1 9,11V5A3,3 0 0,1 12,2M19,11C19,14.53 16.39,17.44 13,17.93V21H11V17.93C7.61,17.44 5,14.53 5,11H7A5,5 0 0,0 12,16A5,5 0 0,0 17,11H19Z"},
        {QStringLiteral("apps"), "M16,20H20V16H16M16,14H20V10H16M10,8H14V4H10M16,8H20V4H16M10,14H14V10H10M4,14H8V10H4M4,20H8V16H4M10,20H14V16H10M4,8H8V4H4V8Z"},
        {QStringLiteral("close"), "M19,6.41L17.59,5L12,10.59L6.41,5L5,6.41L10.59,12L5,17.59L6.41,19L12,13.41L17.59,19L19,17.59L13.41,12L19,6.41Z"},
        {QStringLiteral("star"), "M12,17.27L18.18,21L16.54,13.64L22,8.88L14.81,8.38L12,2L9.19,8.38L2,8.88L7.45,13.64L5.82,21L12,17.27Z"},
        {QStringLiteral("playlist"), "M15,6H3V8H15V6M15,10H3V12H15V10M3,16H11V14H3V16M17,6V14.18C16.69,14.07 16.35,14 16,14A3,3 0 0,0 13,17A3,3 0 0,0 16,20A3,3 0 0,0 19,17V8H22V6H17Z"},
        {QStringLiteral("home"), "M10,20V14H14V20H19V12H22L12,3L2,12H5V20H10Z"},
        {QStringLiteral("work"), "M10,2H14A2,2 0 0,1 16,4V6H20A2,2 0 0,1 22,8V19A2,2 0 0,1 20,21H4C2.89,21 2,20.1 2,19V8C2,6.89 2.89,6 4,6H8V4C8,2.89 8.89,2 10,2M14,6V4H10V6H14Z"},
        {QStringLiteral("keyboard"), "M19,10H17V8H19M19,13H17V11H19M16,10H14V8H16M16,13H14V11H16M16,17H8V15H16M7,10H5V8H7M7,13H5V11H7M8,11H10V13H8M8,8H10V10H8M11,11H13V13H11M11,8H13V10H11M20,5H4C2.89,5 2,5.89 2,7V17A2,2 0 0,0 4,19H20A2,2 0 0,0 22,17V7C22,5.89 21.1,5 20,5Z"},
        {QStringLiteral("account"), "M12,4A4,4 0 0,1 16,8A4,4 0 0,1 12,12A4,4 0 0,1 8,8A4,4 0 0,1 12,4M12,14C16.42,14 20,15.79 20,18V20H4V18C4,15.79 7.58,14 12,14Z"},
        {QStringLiteral("history"), "M13.5,8H12V13L16.28,15.54L17,14.33L13.5,12.25V8M13,3A9,9 0 0,0 4,12H1L4.96,16.03L9,12H6A7,7 0 0,1 13,5A7,7 0 0,1 20,12A7,7 0 0,1 13,19C11.07,19 9.32,18.21 8.06,16.94L6.64,18.36C8.27,20 10.5,21 13,21A9,9 0 0,0 22,12A9,9 0 0,0 13,3"},
        {QStringLiteral("marker"), "M12,11.5A2.5,2.5 0 0,1 9.5,9A2.5,2.5 0 0,1 12,6.5A2.5,2.5 0 0,1 14.5,9A2.5,2.5 0 0,1 12,11.5M12,2A7,7 0 0,0 5,9C5,14.25 12,22 12,22C12,22 19,14.25 19,9A7,7 0 0,0 12,2Z"},
    };
    const char *d = paths.value(name, nullptr);
    if (!d) return;
    const QByteArray svg = QByteArrayLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='")
                           + col.name().toLatin1() + "' d='" + d + "'/></svg>";
    QSvgRenderer r2(svg);
    r2.render(&p, r);
}

static void drawTileIcon(QPainter &p, const QRectF &r, int page, const QColor &col)
{
    drawMdi(p, r, page == 1 ? QStringLiteral("navigation") : page == 2 ? QStringLiteral("music")
                  : page == 4 ? QStringLiteral("phone") : QStringLiteral("info"), col);
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
        const int is = qMin(t.rect.width() * 2 / 5, t.rect.height() / 4);
        drawTileIcon(p, QRectF(t.rect.center().x() - is / 2.0, t.rect.top() + t.rect.height() * 0.28 - is / 2.0, is, is),
                     t.page == Navigation ? 1 : t.page == Music ? 2 : t.page == Phone ? 4 : 0, c.accent);
        f.setPixelSize(qMax(12, qMin(m_safe.height() / 16, t.rect.width() / 7)));
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        drawFit(p, t.rect.adjusted(6, t.rect.height() / 2, -6, 0), Qt::AlignCenter, t.title);
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
    {
        // chevron + "Zurück"
        const QRect b = m_backButton;
        const int ch = b.height() / 3;
        const QPoint o(b.left() + b.height() / 2, b.center().y());
        p.setPen(QPen(c.text, qMax(3, ch / 3), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(QPolygon({o + QPoint(ch / 2, -ch), o + QPoint(-ch / 2, 0), o + QPoint(ch / 2, ch)}));
        p.drawText(b.adjusted(b.height() * 3 / 4, 0, 0, 0), Qt::AlignCenter, QStringLiteral("Zurück"));
    }
    // Page title in the top bar (unless the turn hint uses that space)
    if (!(m_nav.running && m_page != Navigation)) {
        static const char *titles[] = {"", "Navigation", "Musik", "Apps", "Telefon", "Info", "Wetter"};
        QFont tf = f;
        tf.setPixelSize(bar * 9 / 20);
        p.setFont(tf);
        p.setPen(c.dim);
        p.drawText(QRect(m_backButton.right() + bar / 2, m_safe.top(), m_safe.width() / 3, bar), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromUtf8(titles[m_page]));
        p.setFont(f);
    }

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

        if (m_musicKb) {
            const int saved = m_navView;
            m_navView = 1;
            drawNavChoose(p, body); // same keyboard as for destinations
            m_navView = saved;
            break;
        }
        if (m_musicListOn) {
            drawMusicList(p, body);
            break;
        }
        m_listButton = QRect();
        if (!m_hasPlayer) {
            // Friendly empty state: note, short text, the music apps to start.
            m_musicAppButtons.clear();
            const int note = body.height() / 4;
            QFont g = p.font();
            g.setPixelSize(note);
            p.setFont(g);
            p.setPen(c.accent);
            p.drawText(QRect(body.left(), body.top(), body.width(), note + 10), Qt::AlignCenter, QString::fromUtf8("\u266B"));
            g.setPixelSize(bar / 2);
            g.setBold(true);
            p.setFont(g);
            p.setPen(c.text);
            p.drawText(QRect(body.left(), body.top() + note + 10, body.width(), bar * 2 / 3), Qt::AlignCenter,
                       QStringLiteral("Gerade läuft keine Musik"));
            QVector<CarApp> media;
            for (const CarApp &a : m_apps)
                if (a.media && media.size() < 4) media.append(a);
            g.setPixelSize(bar * 2 / 5);
            g.setBold(false);
            p.setFont(g);
            p.setPen(c.dim);
            const int textBottom = body.top() + note + 10 + bar * 2 / 3;
            // Own sources: radio and music files
            m_listButton = QRect(body.right() - bar * 4, body.top(), bar * 4, bar);
            p.setPen(Qt::NoPen);
            p.setBrush(c.accent);
            p.drawRoundedRect(m_listButton, 12, 12);
            g.setPixelSize(bar * 2 / 5);
            g.setBold(true);
            p.setFont(g);
            p.setPen(c.bg);
            p.drawText(m_listButton, Qt::AlignCenter, QStringLiteral("Radio & Musik"));
            g.setBold(false);
            p.drawText(QRect(body.left(), textBottom, body.width(), bar / 2), Qt::AlignCenter,
                       media.isEmpty() ? QStringLiteral("Starte eine Musik- oder Podcast-App am Telefon")
                                       : QStringLiteral("Antippen startet die App"));
            if (!media.isEmpty()) {
                const int gap = 14;
                const int th = body.bottom() - textBottom - bar / 2 - 12;
                const int tw = qMin(th, (body.width() - (media.size() - 1) * gap) / media.size());
                const int x0 = body.center().x() - (media.size() * tw + (media.size() - 1) * gap) / 2;
                for (int i = 0; i < media.size(); ++i) {
                    const QRect r(x0 + i * (tw + gap), textBottom + bar / 2 + 8, tw, th);
                    p.setPen(Qt::NoPen);
                    p.setBrush(c.panel);
                    p.drawRoundedRect(r, 14, 14);
                    const int is = qMin(r.width(), r.height()) * 11 / 20;
                    const QRect ir(r.center().x() - is / 2, r.top() + r.height() / 10, is, is);
                    if (!media[i].icon.isNull()) p.drawImage(ir, media[i].icon);
                    g.setPixelSize(qMin(bar * 2 / 5, r.height() / 6));
                    g.setBold(true);
                    p.setFont(g);
                    p.setPen(c.text);
                    drawFit(p, QRect(r.left() + 4, ir.bottom() + 2, r.width() - 8, r.bottom() - ir.bottom() - 4),
                            Qt::AlignCenter, media[i].name);
                    m_musicAppButtons.append(qMakePair(r, media[i].id));
                }
            }
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
    case WeatherPage:
        drawWeatherPage(p, body);
        break;
    case Info: {
        const QStringList keys = {QStringLiteral("SailAuto"), QStringLiteral("Auto"), QStringLiteral("Bild"),
                                  QStringLiteral("Encoder"), QStringLiteral("Leistung"), QStringLiteral("Nachtmodus")};
        const QStringList vals = {m_version, m_headUnit.isEmpty() ? QStringLiteral("?") : m_headUnit,
                                  QStringLiteral("%1×%2").arg(m_image.width()).arg(m_image.height()), m_encoder,
                                  QStringLiteral("%1 fps · %2 kbit/s").arg(m_fps, 0, 'f', 1).arg(m_kbps, 0, 'f', 0),
                                  m_night ? QStringLiteral("an") : QStringLiteral("aus")};
        const int rh = qMin(bar, (body.height() - 10) / keys.size());
        for (int i = 0; i < keys.size(); ++i) {
            const QRect r(body.left(), body.top() + i * rh, body.width(), rh - 6);
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(r, 12, 12);
            f.setPixelSize(rh * 2 / 5);
            f.setBold(true);
            p.setFont(f);
            p.setPen(c.dim);
            p.drawText(r.adjusted(16, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, keys[i]);
            f.setBold(false);
            p.setFont(f);
            p.setPen(c.text);
            drawFit(p, r.adjusted(r.width() / 4, 0, -16, 0), Qt::AlignLeft | Qt::AlignVCenter, vals[i]);
        }
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

    drawLoading(p);
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
