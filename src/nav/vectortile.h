// Mapbox Vector Tile (MVT, protobuf) reader for the self-drawn map.
// Schema: OpenMapTiles (OSM Scout Server "mbgl" tiles offline, OpenFreeMap
// online). Only what the car map needs: layers, feature type, tags, geometry.
#pragma once

#include <QByteArray>
#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct VtFeature {
    enum Type { Unknown = 0, Point = 1, Line = 2, Polygon = 3 };
    int type = Unknown;
    QHash<QString, QVariant> tags;
    QVector<QVector<QPoint>> parts; // points / lines / rings, in tile units (0..extent)
    QString str(const char *key) const { return tags.value(QLatin1String(key)).toString(); }
};

struct VtLayer {
    QString name;
    int extent = 4096;
    QVector<VtFeature> features;
};

class VectorTile
{
public:
    // data: raw or gzip-compressed protobuf. False if it can not be read.
    bool parse(const QByteArray &data);
    const VtLayer *layer(const QString &name) const;
    QHash<QString, VtLayer> layers;

    static QByteArray gunzip(const QByteArray &data);
};
