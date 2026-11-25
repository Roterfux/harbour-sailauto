// The UI shown on the car display. Rendered with QPainter into a QImage,
// which then goes to the H.264 encoder. Deliberately simple for the MVP;
// later this becomes an offscreen QML scene (QQuickRenderControl).
#pragma once

#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QVector>

class CarScreen : public QObject
{
    Q_OBJECT
public:
    explicit CarScreen(QObject *parent = nullptr);

    void setGeometry(int width, int height, int marginWidth, int marginHeight);
    void setNight(bool night);
    void setInfo(const QString &headUnit, const QString &encoder);
    void setStats(double fps, double kbps);

    QSize size() const { return m_image.size(); }
    // Draws the next frame and returns it (RGB32).
    const QImage &render();

    // action: 0 press, 1 release, 2 drag (AAP TouchAction)
    void touch(int action, int x, int y);

signals:
    void tapped(const QString &what);

private:
    enum Page { Home, Navigation, Music, Phone, Info };
    struct Tile { QRect rect; Page page; QString title; QString glyph; };

    void layout();
    void drawHome(QPainter &p);
    void drawPage(QPainter &p);
    void drawTopBar(QPainter &p);

    QImage m_image;
    QRect m_safe;
    bool m_night = false;
    Page m_page = Home;
    QVector<Tile> m_tiles;
    QRect m_backButton;
    QRect m_playButton;
    bool m_playing = false;
    int m_pressedTile = -1;
    bool m_pressed = false;
    QPoint m_touch;
    QVector<QPoint> m_trail;
    QString m_headUnit, m_encoder;
    double m_fps = 0, m_kbps = 0;
    quint64 m_frame = 0;
};
