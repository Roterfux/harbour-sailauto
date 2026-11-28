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
#include "../nav/mapview.h"
#include "../nav/geosearch.h"
#include "../weather/weather.h"
#include "../phone/callmonitor.h"
#include "../phone/phonebook.h"

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
    // Live map behind the navigation page (nullptr = off)
    void setMap(MapView *map) { m_map = map; m_navChoosing = false; }
    // Weather (top bar + weather page)
    void setWeather(const WeatherData &w) { m_weather = w; }
    // Destination search results (searching = still waiting; note = shown when empty)
    void setSearchResults(const QVector<GeoResult> &r, bool searching, const QString &note)
    { m_results = r; m_searching = searching; m_resultNote = note; m_resultPage = 0; }
    // Loading screen while a phone app starts (hidden by hideLoading or after 15 s)
    void showLoading(const QString &name, const QImage &icon);
    void hideLoading() { m_loadingName.clear(); }
    // Short volume bar (percent), ~2 s
    void showVolume(int percent) { m_volume = percent; m_volumeClock.start(); }
    // Destinations for the navigation page (home/work from the phone settings).
    // Music page list (playlists/tracks of the player), shown after "Liste".
    struct ListItem { QString title, sub; bool playlist; QString icon; };
    void setMusicList(const QVector<ListItem> &items) { m_musicList = items; m_musicListOn = true; m_musicListPage = 0; m_musicKb = false; }
    void closeMusicList() { m_musicListOn = false; m_musicKb = false; }
    int currentPage() const { return m_page; }
    // Keyboard on the music page (radio search), parked only
    void openMusicKeyboard() { m_musicKb = true; m_navInput.clear(); }
    void setDestinations(const QString &home, const QString &work, const QStringList &recent)
    { m_home = home; m_work = work; m_recentDest = recent; }
    // Phone call: ringing = full screen with 4 buttons; silenced/active = strip at the bottom.
    void setCall(const CallInfo &c, bool silenced);
    // Address book for the phone page.
    void setContacts(const QVector<PhoneContact> &contacts);
    void setRecentCalls(const QVector<RecentCall> &calls) { m_recent = calls; }

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
    // Phone page: call this number
    void dial(const QString &number, const QString &name, const QImage &photo);
    // Navigation page: search this destination in Pure Maps / start the route
    void navSearch(const QString &text);
    void navStart();
    void navPick(int index); // a search result
    void navStop();
    // Music page: "Liste" tapped / an entry picked
    void musicListRequested();
    void musicListPicked(int index);
    void musicListBack();                 // "Zurück" inside the list
    void radioSearch(const QString &text);

private:
    enum Page { Home, Navigation, Music, Apps, Phone, Info, WeatherPage };
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
    // Destination entry (navigation page without a running route)
    struct NavButton { QRect rect; QString kind, value; };
    QVector<NavButton> m_navButtons;
    int m_navView = 0;           // 0 choose, 1 keyboard, 2 contacts with address, 3 search results
    QVector<GeoResult> m_results;
    bool m_searching = false;
    QString m_resultNote;
    int m_resultPage = 0;
    QString m_navInput;
    int m_navContactPage = 0;
    QString m_home, m_work;
    QStringList m_recentDest;
    bool m_navSearchSent = false;
    MapView *m_map = nullptr;
    bool m_navChoosing = false;  // map on: destination choice shown instead of the map
    void drawNavOnMap(QPainter &p, const QRect &body);
    void drawNavChoose(QPainter &p, const QRect &body);
    QVector<ListItem> m_musicList;
    bool m_musicListOn = false;
    bool m_musicKb = false;
    int m_musicListPage = 0;
    QRect m_listButton, m_mlUp, m_mlDown;
    QVector<QPair<QRect, int>> m_musicRows;
    void drawMusicList(QPainter &p, const QRect &body);
    WeatherData m_weather;
    QRect m_weatherRect;
    QRect m_infoRect, m_musicBarRect;
    void drawWeatherIcon(QPainter &p, const QRectF &r, int code, bool day) const;
    void drawWeatherPage(QPainter &p, const QRect &body);
    QString m_loadingName;
    QImage m_loadingIcon;
    QElapsedTimer m_loadingClock;
    void drawLoading(QPainter &p);
    int m_volume = -1;
    QElapsedTimer m_volumeClock;
    void drawVolume(QPainter &p);
    QVector<QPair<QRect, QString>> m_musicAppButtons; // empty music page: start an app
    void navButtonTapped(const NavButton &b);
    void drawManeuver(QPainter &p, const QRect &r, bool small);
    // Call
    CallInfo m_call;
    bool m_callSilenced = false;
    struct CallButton { QRect rect; QString action, label; QColor color; };
    QVector<CallButton> m_callButtons;
    void drawCall(QPainter &p);
    bool callFullScreen() const { return m_call.state == CallInfo::Incoming && !m_callSilenced; }
    // Phone page (address book)
    QVector<PhoneContact> m_contacts;
    int m_phoneTab = 0;          // 0 favorites, 1 all, 2 recent calls
    QVector<RecentCall> m_recent;
    int contactForNumber(const QString &number) const;
    int m_phoneScroll = 0;       // pixels
    int m_phoneDetail = -1;      // index into m_contacts, -1 = list
    struct Row { QRect rect; int contact; };
    QVector<Row> m_phoneRows;
    QRect m_phoneTabs[3], m_phoneUp, m_phoneDown, m_phoneList;
    struct DialButton { QRect rect; QString number; };
    QVector<DialButton> m_dialButtons;
    int m_dragStartY = 0, m_dragStartScroll = 0;
    bool m_dragMoved = false;
    QVector<int> phoneVisible() const;
    int phoneRowHeight() const { return qMax(48, m_phoneList.height() / 5); }
    int m_favPage = 0;           // favorites: big tiles, 6 per page
    void drawPhonePage(QPainter &p, const QRect &body);
    void phoneTap(const QPoint &pt);
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
