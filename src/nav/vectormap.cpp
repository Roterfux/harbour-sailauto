#include "vectormap.h"

#include <QFontMetricsF>
#include <QLineF>
#include <QPainter>
#include <QSet>
#include <QtMath>

#include <algorithm>

namespace {
const int kTile = 256;

struct RoadStyle { QColor fill, casing; double width; bool dashed; int order; };

QString pickName(const VtFeature &f)
{
    // German names where the tiles have them, else the local one
    for (const char *k : {"name:de", "name_de", "name", "name:latin", "name_int"}) {
        const QString v = f.str(k);
        if (!v.isEmpty()) return v;
    }
    return QString();
}

int roadRank(const QString &cls)
{
    static const QHash<QString, int> r = {
        {QStringLiteral("motorway"), 9}, {QStringLiteral("trunk"), 8}, {QStringLiteral("primary"), 7},
        {QStringLiteral("secondary"), 6}, {QStringLiteral("tertiary"), 5}, {QStringLiteral("minor"), 4},
        {QStringLiteral("busway"), 3}, {QStringLiteral("service"), 3}, {QStringLiteral("raceway"), 3},
        {QStringLiteral("track"), 2}, {QStringLiteral("path"), 1}, {QStringLiteral("rail"), 0},
        {QStringLiteral("transit"), 0}};
    return r.value(cls, -1);
}

RoadStyle roadStyle(const QString &cls, bool night)
{
    // OsmAnd-like colours: motorway orange, trunk light orange, primary yellow,
    // secondary pale yellow, the rest white (day) / slate (night).
    struct S { const char *cls; QRgb dayFill, dayCase, nightFill, nightCase; double w; bool dash; };
    static const S styles[] = {
        {"motorway", 0xf3a04a, 0xc8762c, 0xa8692e, 0x5e3d1c, 11, false},
        {"trunk", 0xf7bd68, 0xc99446, 0x93703c, 0x544024, 10, false},
        {"primary", 0xfcd88a, 0xc8a85c, 0x7b6c47, 0x463e2a, 9.5, false},
        {"secondary", 0xfff0a4, 0xcbbb72, 0x625e47, 0x3a382c, 8.5, false},
        {"tertiary", 0xffffff, 0xc5bdb1, 0x4f5464, 0x30343f, 7.5, false},
        {"minor", 0xffffff, 0xcdc6ba, 0x464b5a, 0x2c303b, 6, false},
        {"busway", 0xffffff, 0xcdc6ba, 0x464b5a, 0x2c303b, 5, false},
        {"service", 0xffffff, 0xd3ccc1, 0x3e4352, 0x2a2e38, 3.5, false},
        {"raceway", 0xffc0cb, 0xc89aa2, 0x6b4a50, 0x3a2a2e, 3.5, false},
        {"track", 0xb39466, 0, 0x6e5c42, 0, 2, true},
        {"path", 0xa58c70, 0, 0x6d6050, 0, 1.4, true},
        {"rail", 0x9a9a9a, 0, 0x5a5e68, 0, 2, true},
        {"transit", 0xa0a0a0, 0, 0x5a5e68, 0, 1.6, true},
    };
    for (const S &s : styles)
        if (cls == QLatin1String(s.cls)) {
            RoadStyle r;
            r.fill = QColor(night ? s.nightFill : s.dayFill);
            r.casing = (night ? s.nightCase : s.dayCase) ? QColor(night ? s.nightCase : s.dayCase) : QColor();
            r.width = s.w;
            r.dashed = s.dash;
            r.order = roadRank(cls);
            return r;
        }
    return RoadStyle{QColor(), QColor(), 0, false, -1};
}

QColor areaColor(const QString &layer, const QString &cls, bool night)
{
    struct A { const char *cls; QRgb day, night; };
    static const A land[] = {
        {"wood", 0xc3dba9, 0x1d2a21}, {"forest", 0xc3dba9, 0x1d2a21}, {"grass", 0xd5e9bf, 0x1f2b24},
        {"meadow", 0xd5e9bf, 0x1f2b24}, {"park", 0xcfe7b7, 0x1e2b23}, {"garden", 0xd5e9bf, 0x1f2b24},
        {"farmland", 0xeef0d7, 0x1f241f}, {"wetland", 0xd4e6dc, 0x1d2828}, {"sand", 0xf3e9c4, 0x2a2822},
        {"beach", 0xf3e9c4, 0x2a2822}, {"rock", 0xdcd8d2, 0x26272b}, {"ice", 0xf4f8fb, 0x2a3038},
        {"glacier", 0xf4f8fb, 0x2a3038}, {"cemetery", 0xd4dfcc, 0x1f2722}, {"residential", 0xe9e5de, 0x20242e},
        {"suburb", 0xe9e5de, 0x20242e}, {"neighbourhood", 0xe9e5de, 0x20242e}, {"commercial", 0xf1e4e1, 0x262330},
        {"retail", 0xf1e4e1, 0x262330}, {"industrial", 0xe8e0ea, 0x232230}, {"railway", 0xe5e1dd, 0x22242c},
        {"hospital", 0xf5e2e2, 0x2a2328}, {"school", 0xf3ecd8, 0x272622}, {"university", 0xf3ecd8, 0x272622},
        {"college", 0xf3ecd8, 0x272622}, {"kindergarten", 0xf3ecd8, 0x272622}, {"stadium", 0xd9ecd0, 0x1f2b24},
        {"pitch", 0xcfe8c6, 0x1f2b24}, {"playground", 0xd9ecd0, 0x1f2b24}, {"military", 0xf0dcd8, 0x2a2224},
        {"quarry", 0xdcd8d2, 0x26272b}, {"parking", 0xeeeae6, 0x262a34},
    };
    if (layer == QLatin1String("water")) return QColor(night ? 0x1b3048 : 0xa8d2e8);
    if (layer == QLatin1String("park")) return QColor(night ? 0x1e2b23 : 0xcfe7b7);
    if (layer == QLatin1String("building")) return QColor(night ? 0x2c313e : 0xdcd3ca);
    if (layer == QLatin1String("aeroway")) return QColor(night ? 0x2a2d38 : 0xe2dfe6);
    for (const A &a : land)
        if (cls == QLatin1String(a.cls)) return QColor(night ? a.night : a.day);
    return QColor();
}

double angleDeg(const QPointF &a, const QPointF &b) { return qRadiansToDegrees(qAtan2(b.y() - a.y(), b.x() - a.x())); }

// Point and tangent at distance s along a polyline with cumulative lengths cum
bool along(const QPolygonF &pl, const QVector<double> &cum, double s, QPointF *pt, double *ang)
{
    if (pl.size() < 2 || s < 0 || s > cum.last()) return false;
    int i = int(std::upper_bound(cum.begin(), cum.end(), s) - cum.begin()) - 1;
    i = qBound(0, i, pl.size() - 2);
    const double seg = cum[i + 1] - cum[i];
    const double t = seg > 0 ? (s - cum[i]) / seg : 0;
    *pt = pl[i] + (pl[i + 1] - pl[i]) * t;
    *ang = angleDeg(pl[i], pl[i + 1]);
    return true;
}

void drawHaloPath(QPainter &p, const QPainterPath &path, const QColor &text, const QColor &halo, double haloW)
{
    p.setPen(QPen(halo, haloW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(text);
    p.drawPath(path);
}
} // namespace

QColor VectorMap::backgroundFor(bool night) { return night ? QColor(0x1b, 0x1f, 0x2a) : QColor(0xf1, 0xee, 0xe8); }

void VectorMap::insert(int z, int x, int y, const QSharedPointer<VectorTile> &tile) { m_tiles.insert(key(z, x, y), tile); }

void VectorMap::prune(int z, int x, int y, int keep)
{
    if (m_tiles.size() <= 40) return;
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        const QStringList k = it.key().split(QLatin1Char('/'));
        const bool far = k.value(0).toInt() != z || qAbs(k.value(1).toInt() - x) > keep || qAbs(k.value(2).toInt() - y) > keep;
        if (far) it = m_tiles.erase(it);
        else ++it;
    }
}

VectorMap::Job VectorMap::prepare(int z, int cx0, int cy0, int grid, QVector<QVector<int>> *missing) const
{
    Job job;
    job.z = z;
    job.cx0 = cx0;
    job.cy0 = cy0;
    job.grid = grid;
    job.night = m_night;
    const int dz = qMin(z, kMaxDataZoom);
    const int f = 1 << (z - dz); // display tiles per data tile (per axis)
    const int n = 1 << dz;
    const int dx0 = int(std::floor(double(cx0) / f)), dy0 = int(std::floor(double(cy0) / f));
    const int dx1 = int(std::floor(double(cx0 + grid - 1) / f)), dy1 = int(std::floor(double(cy0 + grid - 1) / f));
    for (int dy = dy0; dy <= dy1; ++dy)
        for (int dx = dx0; dx <= dx1; ++dx) {
            if (dy < 0 || dy >= n) continue;
            const int wx = ((dx % n) + n) % n;
            auto it = m_tiles.constFind(key(dz, wx, dy));
            if (it == m_tiles.constEnd()) {
                if (missing) missing->append({dz, wx, dy});
                continue;
            }
            const VtLayer *any = (*it)->layers.isEmpty() ? nullptr : &*(*it)->layers.constBegin();
            const int extent = any ? any->extent : 4096;
            const double scale = double(kTile) * f / extent;
            QTransform tr;
            tr.translate((dx * f - cx0) * kTile, (dy * f - cy0) * kTile);
            tr.scale(scale, scale);
            job.srcs.append({*it, tr});
        }
    return job;
}

void VectorMap::renderCanvas(QPainter &p, int z, int cx0, int cy0, int grid, QVector<QVector<int>> *missing)
{
    const Job job = prepare(z, cx0, cy0, grid, missing);
    const Labels l = draw(p, job);
    m_lines = l.lines;
    m_points = l.points;
}

VectorMap::Labels VectorMap::draw(QPainter &p, const Job &job)
{
    Labels labels;
    QVector<LabelLine> &lines = labels.lines;
    QVector<LabelPoint> &points = labels.points;
    const int z = job.z, grid = job.grid;
    const bool night = job.night;
    const QVector<Src> &srcs = job.srcs;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const double zoomScale = std::pow(1.55, z - 16); // line widths grow with zoom, but not 2x per level
    // Only what lies on the canvas (data tiles are bigger than the canvas at z15+)
    const QRectF canvasRect(-32, -32, grid * kTile + 64, grid * kTile + 64);
    auto mapPart = [&](const Src &s, const QVector<QPoint> &part, QPolygonF *out) {
        out->clear();
        out->reserve(part.size());
        for (const QPoint &pt : part) *out << s.tr.map(QPointF(pt));
        return out->boundingRect().intersects(canvasRect);
    };

    // Areas: landcover, landuse, park, water, aeroway, then buildings
    auto fillLayer = [&](const char *layerName, double minZoom) {
        if (z < minZoom) return;
        const QString ln = QLatin1String(layerName);
        for (const Src &s : srcs) {
            const VtLayer *l = s.tile->layer(ln);
            if (!l) continue;
            const bool building = ln == QLatin1String("building");
            p.setRenderHint(QPainter::Antialiasing, !building); // many small shapes: no edge smoothing needed
            QPolygonF poly;
            for (const VtFeature &ft : l->features) {
                if (ft.type != VtFeature::Polygon) continue;
                const QColor c = areaColor(ln, ft.str("class").isEmpty() ? ft.str("subclass") : ft.str("class"), night);
                if (!c.isValid()) continue;
                p.setPen(Qt::NoPen);
                p.setBrush(c);
                if (ft.parts.size() == 1) {
                    if (mapPart(s, ft.parts.first(), &poly)) p.drawPolygon(poly);
                    continue;
                }
                QPainterPath path; // with holes
                path.setFillRule(Qt::OddEvenFill);
                bool any = false;
                for (const QVector<QPoint> &ring : ft.parts) {
                    any = mapPart(s, ring, &poly) || any;
                    path.addPolygon(poly);
                }
                if (any) p.drawPath(path);
            }
        }
    };
    fillLayer("landcover", 0);
    fillLayer("landuse", 0);
    fillLayer("park", 0);
    fillLayer("water", 0);
    fillLayer("aeroway", 0);

    // Rivers and streams
    {
        QVector<QPolygonF> big, small;
        for (const Src &s : srcs) {
            const VtLayer *l = s.tile->layer(QStringLiteral("waterway"));
            if (!l) continue;
            for (const VtFeature &ft : l->features) {
                if (ft.type != VtFeature::Line) continue;
                const bool isBig = ft.str("class") == QLatin1String("river") || ft.str("class") == QLatin1String("canal");
                QPolygonF poly;
                for (const QVector<QPoint> &line : ft.parts)
                    if (mapPart(s, line, &poly)) (isBig ? big : small) << poly;
            }
        }
        const QColor w(night ? 0x1b3048 : 0xa8d2e8);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(w, 5 * zoomScale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF &pl : big) p.drawPolyline(pl);
        p.setPen(QPen(w, 1.8 * zoomScale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const QPolygonF &pl : small) p.drawPolyline(pl);
    }
    fillLayer("building", 15); p.setRenderHint(QPainter::Antialiasing);

    // Roads: tunnels faint, then normal, then bridges; per level casings first,
    // then fills, lower classes below higher ones.
    struct Bucket { QVector<QPolygonF> lines; RoadStyle st; bool ramp; };
    for (int level = 0; level < 3; ++level) { // 0 tunnel, 1 ground, 2 bridge
        QHash<QString, Bucket> buckets;
        for (const Src &s : srcs) {
            const VtLayer *l = s.tile->layer(QStringLiteral("transportation"));
            if (!l) continue;
            for (const VtFeature &ft : l->features) {
                if (ft.type != VtFeature::Line) continue;
                const QString br = ft.str("brunnel");
                const int lv = br == QLatin1String("tunnel") ? 0 : br == QLatin1String("bridge") ? 2 : 1;
                if (lv != level) continue;
                const QString cls = ft.str("class");
                const RoadStyle st = roadStyle(cls, night);
                if (st.order < 0) continue;
                const bool ramp = ft.tags.value(QStringLiteral("ramp")).toInt() == 1;
                const QString bk = cls + (ramp ? QStringLiteral("/r") : QString());
                Bucket &b = buckets[bk];
                b.st = st;
                b.ramp = ramp;
                QPolygonF poly;
                for (const QVector<QPoint> &line : ft.parts)
                    if (mapPart(s, line, &poly)) b.lines << poly;
            }
        }
        QVector<const Bucket *> order;
        for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) order << &it.value();
        std::sort(order.begin(), order.end(), [](const Bucket *a, const Bucket *b) { return a->st.order < b->st.order; });
        p.setBrush(Qt::NoBrush);
        for (int pass = 0; pass < 2; ++pass) {
            for (const Bucket *b : order) {
                const double w = b->st.width * zoomScale * (b->ramp ? 0.7 : 1.0);
                if (b->st.dashed) {
                    if (pass == 0) continue;
                    QPen pen(b->st.fill, qMax(1.0, w), Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
                    pen.setDashPattern({3, 2.5});
                    p.setPen(pen);
                    p.setRenderHint(QPainter::Antialiasing, false);
                    for (const QPolygonF &pl : b->lines) p.drawPolyline(pl);
                    p.setRenderHint(QPainter::Antialiasing, true);
                    continue;
                }
                QColor col = pass == 0 ? b->st.casing : b->st.fill;
                if (level == 0) col.setAlpha(pass == 0 ? 90 : 150); // tunnel: faint
                const double width = pass == 0 ? w + qMax(2.0, w * 0.25) + (level == 2 ? 2 : 0) : w;
                if (level == 2 && pass == 0) col = night ? QColor(10, 10, 14) : QColor(110, 100, 90);
                p.setPen(QPen(col, width, Qt::SolidLine, level == 2 ? Qt::FlatCap : Qt::RoundCap, Qt::RoundJoin));
                for (const QPolygonF &pl : b->lines) p.drawPolyline(pl);
            }
        }
    }
   
    p.restore();

    // Label sources in canvas pixels (drawn on screen later, upright)
    QSet<QString> seenPoint;
    for (const Src &s : srcs) {
        if (const VtLayer *l = s.tile->layer(QStringLiteral("transportation_name"))) {
            for (const VtFeature &ft : l->features) {
                if (ft.type != VtFeature::Line) continue;
                const QString cls = ft.str("class");
                const int rank = roadRank(cls);
                if (rank < 2 && cls != QLatin1String("path")) continue;
                const QString name = pickName(ft), ref = ft.str("ref");
                for (const QVector<QPoint> &line : ft.parts) {
                    QPolygonF poly;
                    for (const QPoint &pt : line) poly << s.tr.map(QPointF(pt));
                    if (!name.isEmpty() && rank >= 2) lines.append({name, poly, 40 + rank * 5, false, cls});
                    if (!ref.isEmpty() && rank >= 6 && ref.size() <= 8) lines.append({ref, poly, 50 + rank * 4, true, cls});
                }
            }
        }
        if (const VtLayer *l = s.tile->layer(QStringLiteral("place"))) {
            for (const VtFeature &ft : l->features) {
                if (ft.type != VtFeature::Point) continue;
                const QString cls = ft.str("class"), name = pickName(ft);
                if (name.isEmpty() || seenPoint.contains(name)) continue;
                static const QHash<QString, int> pr = {
                    {QStringLiteral("city"), 100}, {QStringLiteral("town"), 95}, {QStringLiteral("village"), 70},
                    {QStringLiteral("suburb"), 62}, {QStringLiteral("hamlet"), 45}, {QStringLiteral("quarter"), 40},
                    {QStringLiteral("neighbourhood"), 35}, {QStringLiteral("isolated_dwelling"), 20}, {QStringLiteral("locality"), 20}};
                if (!pr.contains(cls)) continue;
                seenPoint.insert(name);
                const int prio = pr.value(cls);
                points.append({name, s.tr.map(QPointF(ft.parts.first().first())), prio,
                                 prio >= 95 ? 3 : prio >= 60 ? 2 : 1, false});
            }
        }
        if (const VtLayer *l = s.tile->layer(QStringLiteral("water_name"))) {
            for (const VtFeature &ft : l->features) {
                const QString name = pickName(ft);
                if (name.isEmpty() || seenPoint.contains(name)) continue;
                seenPoint.insert(name);
                if (ft.type == VtFeature::Point) points.append({name, s.tr.map(QPointF(ft.parts.first().first())), 30, 1, true});
            }
        }
    }
    return labels;
}

void VectorMap::drawLabels(QPainter &p, const QTransform &toScreen, const QRectF &view, double horizon,
                           const QVector<QRectF> &reserved)
{
    const double h = view.height();
    const QRectF usable(view.left(), horizon + h * 0.05, view.width(), view.bottom() - horizon - h * 0.05);
    QVector<QRectF> taken = reserved;
    QHash<QString, QVector<QPointF>> placed; // name -> screen positions (one label per name nearby)
    const QColor text = m_night ? QColor(226, 230, 238) : QColor(38, 42, 54);
    const QColor halo = m_night ? QColor(12, 14, 22, 230) : QColor(255, 255, 255, 235);
    const QColor water = m_night ? QColor(120, 170, 220) : QColor(40, 100, 160);
    auto free = [&](const QRectF &r) {
        if (!usable.contains(r.center())) return false;
        for (const QRectF &t : taken)
            if (t.intersects(r)) return false;
        return true;
    };
    auto nearSame = [&](const QString &name, const QPointF &pt, double dist) {
        for (const QPointF &q : placed.value(name))
            if (QLineF(q, pt).length() < dist) return true;
        return false;
    };
    // Nearer = lower on screen = bigger text
    auto depthScale = [&](double y) { return 0.72 + 0.45 * qBound(0.0, (y - horizon) / (view.bottom() - horizon), 1.0); };

    struct Cand { int prio; bool line; int idx; };
    QVector<Cand> cands;
    for (int i = 0; i < m_points.size(); ++i) cands.append({m_points[i].prio, false, i});
    for (int i = 0; i < m_lines.size(); ++i) cands.append({m_lines[i].prio, true, i});
    std::stable_sort(cands.begin(), cands.end(), [](const Cand &a, const Cand &b) { return a.prio > b.prio; });

    int nPlaces = 0, nRoads = 0, nShields = 0;
    QFont font = p.font();
    for (const Cand &c : cands) {
        if (!c.line) {
            const LabelPoint &lp = m_points[c.idx];
            if (nPlaces >= 10) continue;
            const QPointF sp = toScreen.map(lp.pt);
            if (!usable.contains(sp)) continue;
            font.setBold(!lp.water);
            font.setItalic(lp.water);
            font.setPixelSize(qMax(11, int(h / 25 * (0.75 + 0.18 * lp.size) * depthScale(sp.y()))));
            const QFontMetricsF fm(font);
            QRectF box(0, 0, fm.width(lp.name) + 8, fm.height() + 2);
            box.moveCenter(sp);
            if (!free(box.adjusted(-6, -4, 6, 4)) || nearSame(lp.name, sp, 400)) continue;
            taken.append(box);
            placed[lp.name].append(sp);
            QPainterPath path;
            path.addText(box.left() + 4, box.top() + 1 + fm.ascent(), font, lp.name);
            drawHaloPath(p, path, lp.water ? water : text, halo, qMax(3.0, font.pixelSize() / 5.0));
            ++nPlaces;
            continue;
        }
        const LabelLine &ll = m_lines[c.idx];
        if (ll.shield ? nShields >= 6 : nRoads >= 10) continue;
        // Street on screen; keep the part below the horizon
        QPolygonF sl;
        for (const QPointF &pt : ll.pts) sl << toScreen.map(pt);
        // Split into runs inside the usable area
        QVector<QPolygonF> runs(1);
        for (const QPointF &pt : sl) {
            if (usable.adjusted(-40, 0, 40, 0).contains(pt)) runs.last() << pt;
            else if (!runs.last().isEmpty()) runs.append(QPolygonF());
        }
        bool done = false;
        for (QPolygonF run : runs) {
            if (done || run.size() < 2) continue;
            QVector<double> cum(1, 0.0);
            for (int i = 1; i < run.size(); ++i) cum << cum.last() + QLineF(run[i - 1], run[i]).length();
            const double len = cum.last();
            if (ll.shield) {
                // Road number shield at the middle of the visible part
                QPointF mid;
                double ang;
                if (len < 60 || !along(run, cum, len / 2, &mid, &ang)) continue;
                font.setBold(true);
                font.setItalic(false);
                font.setPixelSize(qMax(12, int(h / 27 * depthScale(mid.y()))));
                const QFontMetricsF fm(font);
                QRectF box(0, 0, fm.width(ll.name) + font.pixelSize() * 0.9, fm.height() + 4);
                box.moveCenter(mid);
                if (!free(box.adjusted(-4, -4, 4, 4)) || nearSame(QStringLiteral("#") + ll.name, mid, 350)) continue;
                taken.append(box);
                placed[QStringLiteral("#") + ll.name].append(mid);
                const bool motorway = ll.cls == QLatin1String("motorway") || ll.cls == QLatin1String("trunk");
                p.setPen(QPen(Qt::white, 1.5));
                p.setBrush(motorway ? QColor(38, 98, 178) : QColor(244, 206, 70));
                p.drawRoundedRect(box, 5, 5);
                p.setPen(motorway ? Qt::white : QColor(30, 30, 30));
                p.setFont(font);
                p.drawText(box, Qt::AlignCenter, ll.name);
                ++nShields;
                done = true;
                continue;
            }
            // Street name along the street
            font.setBold(false);
            font.setItalic(false);
            const double yMid = run[run.size() / 2].y();
            font.setPixelSize(qMax(12, int(h / 26 * depthScale(yMid))));
            const QFontMetricsF fm(font);
            const double tw = fm.width(ll.name);
            if (len < tw + 30) continue;
            // Try the middle first, then positions around it; the text part
            // must be fairly straight (OsmAnd: max bend between glyphs).
            const double step = qMax(20.0, tw / 2);
            QVector<double> starts;
            const double centre = (len - tw) / 2;
            starts << centre;
            for (double d = step; centre - d >= 10 || centre + d <= len - tw - 10; d += step) {
                if (centre - d >= 10) starts << centre - d;
                if (centre + d <= len - tw - 10) starts << centre + d;
            }
            for (double s0 : starts) {
                QPointF a, b;
                double angA, angB;
                if (!along(run, cum, s0, &a, &angA) || !along(run, cum, s0 + tw, &b, &angB)) continue;
                // straightness: all segment angles within the window close to the chord
                const double chord = angleDeg(a, b);
                bool straight = true;
                for (int i = 0; i + 1 < run.size(); ++i) {
                    if (cum[i + 1] < s0 || cum[i] > s0 + tw) continue;
                    if (qAbs(std::remainder(angleDeg(run[i], run[i + 1]) - chord, 360.0)) > 28) { straight = false; break; }
                }
                if (!straight) continue;
                // Read left to right: reverse when the street runs right to left
                const bool reverse = qAbs(std::remainder(chord, 360.0)) > 90;
                // Glyph positions and collision boxes
                struct G { QPointF pos; double ang; QString ch; double w; };
                QVector<G> glyphs;
                double sPos = 0;
                bool ok = true;
                QVector<QRectF> boxes;
                for (int k = 0; k < ll.name.size(); ++k) {
                    const QString ch = ll.name.mid(k, 1);
                    const double gw = fm.width(ch);
                    const double sCenter = reverse ? s0 + tw - (sPos + gw / 2) : s0 + sPos + gw / 2;
                    QPointF gp;
                    double ga;
                    if (!along(run, cum, sCenter, &gp, &ga)) { ok = false; break; }
                    if (reverse) ga += 180;
                    glyphs.append({gp, ga, ch, gw});
                    const double bs = qMax(gw, fm.height() * 0.8);
                    boxes << QRectF(gp.x() - bs / 2, gp.y() - bs / 2, bs, bs);
                    sPos += gw;
                }
                if (!ok) continue;
                bool clash = false;
                for (const QRectF &bx : boxes)
                    if (!free(bx)) { clash = true; break; }
                if (clash || nearSame(ll.name, (a + b) / 2, 420)) continue;
                taken += boxes;
                placed[ll.name].append((a + b) / 2);
                const double haloW = qMax(3.0, font.pixelSize() / 4.5);
                for (int pass = 0; pass < 2; ++pass)
                    for (const G &g : glyphs) {
                        QPainterPath gpth;
                        gpth.addText(-g.w / 2, fm.ascent() / 2 - fm.descent() / 2, font, g.ch);
                        p.save();
                        p.translate(g.pos);
                        p.rotate(g.ang);
                        if (pass == 0) {
                            p.setPen(QPen(halo, haloW, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                            p.setBrush(Qt::NoBrush);
                        } else {
                            p.setPen(Qt::NoPen);
                            p.setBrush(text);
                        }
                        p.drawPath(gpth);
                        p.restore();
                    }
                ++nRoads;
                done = true;
                break;
            }
        }
    }
}
