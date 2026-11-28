// Self-drawn map from vector tiles (OpenMapTiles schema), in the spirit of
// OsmAnd: the map itself has no text; names are placed afterwards on the
// screen, always upright and readable, by priority, without overlaps:
//  - towns / villages / suburbs as halo text at their point,
//  - street names written along the street (glyph by glyph, never upside
//    down, only where the street is straight enough and long enough),
//  - road numbers (A1, B17, ...) as shields,
//  - lake / river names.
// One label per name nearby; fewer, bigger labels near the car.
#pragma once

#include <QColor>
#include <QHash>
#include <QPainterPath>
#include <QPoint>
#include <QPolygonF>
#include <QRectF>
#include <QSharedPointer>
#include <QString>
#include <QTransform>
#include <QVector>

#include "vectortile.h"

class QPainter;

class VectorMap
{
public:
    static const int kMaxDataZoom = 14; // OpenMapTiles tiles stop at z14, higher zooms scale them

    void setNight(bool night) { m_night = night; }
    bool night() const { return m_night; }
    // Data tiles (zoom kMaxDataZoom or lower)
    void insert(int z, int x, int y, const QSharedPointer<VectorTile> &tile);
    bool has(int z, int x, int y) const { return m_tiles.contains(key(z, x, y)); }
    int count() const { return m_tiles.size(); }
    void prune(int z, int x, int y, int keep); // drop tiles far away from (x, y)

    struct LabelLine { QString name; QPolygonF pts; int prio; bool shield; QString cls; };
    struct LabelPoint { QString name; QPointF pt; int prio; int size; bool water; };
    struct Labels { QVector<LabelLine> lines; QVector<LabelPoint> points; };
    struct Src { QSharedPointer<VectorTile> tile; QTransform tr; };
    // Everything needed to draw one canvas; draw() may run in another thread.
    struct Job { QVector<Src> srcs; int z = 0, cx0 = 0, cy0 = 0, grid = 0; bool night = false; };

    // Canvas = grid x grid tiles of 256 px at display zoom z, top-left tile
    // cx0/cy0. Data tiles still missing go to *missing (z, x, y).
    Job prepare(int z, int cx0, int cy0, int grid, QVector<QVector<int>> *missing) const;
    // Draws the canvas without text, returns the label sources (canvas pixels).
    static Labels draw(QPainter &p, const Job &job);
    void setLabels(const Labels &l) { m_lines = l.lines; m_points = l.points; }
    // prepare + draw + setLabels in one go
    void renderCanvas(QPainter &p, int z, int cx0, int cy0, int grid, QVector<QVector<int>> *missing);
    // Labels for the last canvas, canvasToScreen maps canvas pixels to the screen.
    // reserved: screen areas to keep free (car marker, buttons).
    void drawLabels(QPainter &p, const QTransform &canvasToScreen, const QRectF &view, double horizon,
                    const QVector<QRectF> &reserved);

    QColor background() const { return backgroundFor(m_night); }
    static QColor backgroundFor(bool night);

private:
    static QString key(int z, int x, int y) { return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y); }
    QHash<QString, QSharedPointer<VectorTile>> m_tiles;
    bool m_night = false;
    QVector<LabelLine> m_lines;
    QVector<LabelPoint> m_points;
};
