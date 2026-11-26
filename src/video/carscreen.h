// The UI shown on the car display. Rendered with QPainter into a QImage,
// which then goes to the H.264 encoder. Deliberately simple for the MVP;
// later this becomes an offscreen QML scene (QQuickRenderControl).
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QVector>

#include "../phone/phonestatus.h"
#include "../nav/puremaps.h"
#include "../phone/callmonitor.h"

class CarScreen : public QObject
{
    Q_OBJECT
public:
    explicit CarScreen(QObject *parent = nullptr);

    void setGeometry(int width, int height, int marginWidth, int marginHeight);
    void setNight(bool night);
    void setInfo(const QString &headUnit, const QString &encoder);
    void setStats(double fps, double kbps);
    void setVersion(const QString &v) { m_version = v; }
    void setPhoneState(const PhoneState &s) { m_phone = s; }
    struct CarApp { QString id, name; QImage icon; bool media; };
    void setApps(const QVector<CarApp> &apps);
    void showMusic() { m_page = Music; }
    // Banner for a phone notification (8 s, tap closes). Text only when parked.
    void showNotification(const QString &app, const QString &title, const QString &body);
    void setParked(bool parked) { m_parked = parked; }
    // Pure Maps navigation; icon may be null (own arrow is drawn then).
    void setNav(const NavState &s, const QImage &icon);
    // Phone call: ringing = full screen with 4 buttons; silenced/active = strip at the bottom.
    void setCall(const CallInfo &c, bool silenced);

    QSize size() const { return m_image.size(); }
    // Draws the next frame and returns it (RGB32).
    const QImage &render();

    // What the phone's music player is doing (MPRIS).
    void setNowPlaying(bool hasPlayer, const QString &player, const QString &title, const QString &artist,
                       const QString &album, const QImage &art, bool playing, qint64 posMs, qint64 lenMs);

    // action: 0 press, 1 release, 2 drag (AAP TouchAction)
    void touch(int action, int x, int y);

signals:
    void tapped(const QString &what);
    // "playpause", "next", "previous" from the music page buttons
    void mediaCommand(const QString &cmd);
    // Tap on the progress bar: jump to that point (0..1).
    void mediaSeek(double fraction);
    // Apps page: start this phone app (desktop id)
    void launchApp(const QString &id);
    // "answer", "decline", "silence", "silence_music", "hangup"
    void callAction(const QString &action);

private:
    enum Page { Home, Navigation, Music, Apps, Phone, Info };
    struct Tile { QRect rect; Page page; QString title; QString glyph; };

    void layout();
    void drawHome(QPainter &p);
    void drawPage(QPainter &p);
    void drawTopBar(QPainter &p);
    // Draws the phone status icons right-aligned, ending at x = right. Returns left edge.
    int drawStatusIcons(QPainter &p, int right, int top, int height);

    QImage m_image;
    QRect m_safe;
    bool m_night = false;
    Page m_page = Home;
    QVector<Tile> m_tiles;
    QRect m_backButton;
    QRect m_playButton, m_prevButton, m_nextButton;
    QRect m_progressRect; // set while drawing the music page
    // Apps page: grid of 4x2, paged
    QVector<CarApp> m_apps;
    QVector<QRect> m_appRects;  // rects of the visible page, same order
    int m_appPage = 0;
    QRect m_appPrev, m_appNext;
    int m_pressedApp = -1;
    void layoutApps();
    // Notification banner
    struct Banner { QString app, title, body; };
    QVector<Banner> m_banners;    // queue, front is shown
    QElapsedTimer m_bannerClock;
    QRect m_bannerRect;
    bool m_parked = false;
    void drawBanner(QPainter &p);
    // Navigation
    NavState m_nav;
    QImage m_navIcon;
    QRect m_navStartButton;
    void drawNavPage(QPainter &p, const QRect &body);
    void drawManeuver(QPainter &p, const QRect &r, bool small);
    // Call
    CallInfo m_call;
    bool m_callSilenced = false;
    struct CallButton { QRect rect; QString action, label; QColor color; };
    QVector<CallButton> m_callButtons;
    void drawCall(QPainter &p);
    bool callFullScreen() const { return m_call.state == CallInfo::Incoming && !m_callSilenced; }
    bool m_hasPlayer = false, m_mediaPlaying = false;
    QString m_player, m_title, m_artist, m_album;
    QImage m_art;
    qint64 m_posMs = 0, m_lenMs = 0;
    QElapsedTimer m_posClock;
    bool m_playing = false;
    int m_pressedTile = -1;
    bool m_pressed = false;
    QPoint m_touch;
    QVector<QPoint> m_trail;
    QString m_headUnit, m_encoder, m_version;
    double m_fps = 0, m_kbps = 0;
    PhoneState m_phone;
    quint64 m_frame = 0;
};
