#include "carscreen.h"

#include <QDateTime>
#include <QFont>
#include <QPainter>
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
    const int w = (area.width() - 3 * gap) / 4;
    const int h = area.height();
    const struct { Page p; const char *t; const char *g; } defs[] = {
        {Navigation, "Navigation", "⮝"}, {Music, "Musik", "♫"},
        {Phone, "Telefon", "☎"}, {Info, "Info", "i"},
    };
    for (int i = 0; i < 4; ++i)
        m_tiles.append({QRect(area.left() + i * (w + gap), area.top(), w, h), defs[i].p,
                        QString::fromUtf8(defs[i].t), QString::fromUtf8(defs[i].g)});
    m_backButton = QRect(m_safe.left() + m_safe.width() / 40, m_safe.top() + bar / 8, bar * 3, bar * 3 / 4);
    const int pb = m_safe.height() / 4;
    m_playButton = QRect(m_safe.center().x() - pb / 2, m_safe.center().y(), pb, pb);
}

void CarScreen::touch(int action, int x, int y)
{
    m_touch = QPoint(x, y);
    if (action == 0) { // press
        m_pressed = true;
        m_trail.clear();
        m_pressedTile = -1;
        if (m_page == Home)
            for (int i = 0; i < m_tiles.size(); ++i)
                if (m_tiles[i].rect.contains(m_touch)) m_pressedTile = i;
    } else if (action == 1) { // release
        m_pressed = false;
        if (m_page == Home) {
            if (m_pressedTile >= 0 && m_tiles[m_pressedTile].rect.contains(m_touch)) {
                m_page = m_tiles[m_pressedTile].page;
                emit tapped(m_tiles[m_pressedTile].title);
            }
        } else if (m_backButton.contains(m_touch)) {
            m_page = Home;
            emit tapped(QStringLiteral("Zurück"));
        } else if (m_page == Music && m_playButton.contains(m_touch)) {
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
    p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, QTime::currentTime().toString(QStringLiteral("HH:mm")));
    p.setPen(c.accent);
    if (m_page == Home)
        p.drawText(r.adjusted(m_safe.width() / 40, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SailAuto"));
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
        p.drawText(t.rect.adjusted(0, 0, 0, -t.rect.height() / 4), Qt::AlignCenter, t.glyph);
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
    case Navigation: {
        // Placeholder "map": a moving grid with a position marker.
        p.setPen(QPen(c.dim, 1));
        const int step = 40;
        const int off = static_cast<int>(m_frame % step);
        for (int x = body.left() - off; x < body.right(); x += step) p.drawLine(x, body.top(), x, body.bottom());
        for (int y = body.top() + off; y < body.bottom(); y += step) p.drawLine(body.left(), y, body.right(), y);
        p.setBrush(c.accent);
        p.setPen(Qt::NoPen);
        p.drawEllipse(body.center(), 14, 14);
        p.setPen(c.text);
        p.drawText(body, Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Navigation (Platzhalter)"));
        break;
    }
    case Music: {
        p.setPen(c.text);
        p.drawText(body, Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("Musik (Demo, noch kein Ton)"));
        p.setPen(Qt::NoPen);
        p.setBrush(c.accent);
        p.drawEllipse(m_playButton);
        p.setBrush(c.bg);
        const QRect r = m_playButton.adjusted(m_playButton.width() / 3, m_playButton.height() / 4,
                                              -m_playButton.width() / 4, -m_playButton.height() / 4);
        if (m_playing) {
            p.drawRect(QRect(r.left(), r.top(), r.width() / 3, r.height()));
            p.drawRect(QRect(r.right() - r.width() / 3, r.top(), r.width() / 3, r.height()));
        } else {
            QPolygon tri;
            tri << r.topLeft() << QPoint(r.right(), r.center().y()) << r.bottomLeft();
            p.drawPolygon(tri);
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
        const QString txt = QStringLiteral("Head Unit: %1\nVideo: %2x%3, sicher %4x%5\nEncoder: %6\n%7 fps, %8 kbit/s\nNachtmodus: %9")
                                .arg(m_headUnit.isEmpty() ? QStringLiteral("?") : m_headUnit)
                                .arg(m_image.width()).arg(m_image.height())
                                .arg(m_safe.width()).arg(m_safe.height())
                                .arg(m_encoder)
                                .arg(m_fps, 0, 'f', 1).arg(m_kbps, 0, 'f', 0)
                                .arg(m_night ? QStringLiteral("an") : QStringLiteral("aus"));
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
    p.fillRect(m_safe, c.bg);
    drawTopBar(p);
    if (m_page == Home) drawHome(p);
    else drawPage(p);

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
