#include "carscreen.h"

#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

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
    p.drawText(QRect(in.left(), in.top(), in.width(), bar / 2), Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(f).elidedText(head, Qt::ElideRight, in.width()));
    f.setPixelSize(bar * 9 / 20);
    p.setFont(f);
    p.setPen(c.text);
    const QRect titleR(in.left(), in.top() + bar / 2, in.width(), bar * 2 / 3);
    p.drawText(titleR, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(f).elidedText(b.title, Qt::ElideRight, in.width()));
    f.setBold(false);
    f.setPixelSize(bar * 2 / 5);
    p.setFont(f);
    p.setPen(m_parked ? c.text : c.dim);
    const QString body = m_parked ? b.body : (b.body.isEmpty() ? QString() : QStringLiteral("Text nur im Stand"));
    const QRect bodyR(in.left(), titleR.bottom(), in.width(), in.bottom() - titleR.bottom());
    p.drawText(bodyR, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(f).elidedText(QString(body).replace(QLatin1Char('\n'), QLatin1Char(' ')), Qt::ElideRight, in.width()));
}

void CarScreen::setNav(const NavState &s, const QImage &icon)
{
    // Show the navigation page when a route starts.
    if (s.running && !m_nav.running && m_page != Navigation) m_page = Navigation;
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
    QFont f = p.font();
    if (!m_nav.running) {
        f.setPixelSize(bar / 2);
        f.setBold(true);
        p.setFont(f);
        p.setPen(c.text);
        const QString msg = !m_nav.app ? QStringLiteral("Pure Maps läuft nicht.")
                                       : QStringLiteral("Keine Route.\nZiel in Pure Maps am Telefon wählen\nund Navigation starten.");
        p.drawText(body.adjusted(0, 0, 0, -bar * 2), Qt::AlignCenter | Qt::TextWordWrap, msg);
        if (!m_nav.app) {
            m_navStartButton = QRect(body.center().x() - bar * 3, body.bottom() - bar * 2, bar * 6, bar * 3 / 2);
            p.setPen(Qt::NoPen);
            p.setBrush(c.accent);
            p.drawRoundedRect(m_navStartButton, 14, 14);
            p.setPen(c.bg);
            p.drawText(m_navStartButton, Qt::AlignCenter, QStringLiteral("Pure Maps starten"));
        }
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
            m_page = Home;
            emit tapped(QStringLiteral("Zurück"));
        } else if (m_page == Navigation && m_navStartButton.contains(m_touch)) {
            emit launchApp(QStringLiteral("harbour-pure-maps"));
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
        p.drawText(textRect.adjusted(0, 0, 0, -artSize * 2 / 3), Qt::AlignLeft | Qt::AlignVCenter,
                   fmTitle.elidedText(m_title.isEmpty() ? QStringLiteral("(ohne Titel)") : m_title, Qt::ElideRight, textRect.width()));
        t.setBold(false);
        t.setPixelSize(bar / 2);
        p.setFont(t);
        const QFontMetrics fm(t);
        p.drawText(textRect.adjusted(0, artSize / 3, 0, -artSize / 3), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_artist, Qt::ElideRight, textRect.width()));
        p.setPen(c.dim);
        p.drawText(textRect.adjusted(0, artSize * 2 / 3, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_album.isEmpty() ? m_player : m_album + QStringLiteral(" · ") + m_player,
                                 Qt::ElideRight, textRect.width()));
        // Progress bar (position extrapolated between polls while playing).
        if (m_lenMs > 0) {
            qint64 pos = m_posMs + (m_mediaPlaying && m_posClock.isValid() ? m_posClock.elapsed() : 0);
            pos = qMin(pos, m_lenMs);
            const QRect bar2(body.left(), artRect.bottom() + 12, body.width(), 6);
            m_progressRect = bar2;
            p.setPen(Qt::NoPen);
            p.setBrush(c.panel);
            p.drawRoundedRect(bar2, 3, 3);
            p.setBrush(c.accent);
            p.drawRoundedRect(QRect(bar2.left(), bar2.top(), int(bar2.width() * pos / m_lenMs), bar2.height()), 3, 3);
        }
        drawRound(m_prevButton, false);
        drawSkip(m_prevButton, false);
        drawRound(m_nextButton, false);
        drawSkip(m_nextButton, true);
        drawRound(m_playButton, true);
        drawPlayPause(m_playButton, m_mediaPlaying);
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
            p.drawText(tr, Qt::AlignCenter, nfm.elidedText(a.name, Qt::ElideRight, tr.width()));
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
        p.setPen(c.text);
        p.drawText(body, Qt::AlignCenter, QStringLiteral("Telefon (Platzhalter)"));
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
