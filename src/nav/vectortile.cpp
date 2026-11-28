#include "vectortile.h"

#include <cstring>
#include <zlib.h>

namespace {
// Minimal protobuf reader
struct Pb {
    const quint8 *p, *end;
    bool ok = true;
    Pb() : p(nullptr), end(nullptr) {} // QVector (Qt 5.6) needs it
    Pb(const char *d, int n) : p(reinterpret_cast<const quint8 *>(d)), end(reinterpret_cast<const quint8 *>(d) + n) {}
    bool atEnd() const { return p >= end || !ok; }
    quint64 varint()
    {
        quint64 v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (p >= end) { ok = false; return 0; }
            const quint8 b = *p++;
            v |= quint64(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    // Reads a key; returns field number, wire type in *wt
    int key(int *wt)
    {
        const quint64 k = varint();
        *wt = int(k & 7);
        return int(k >> 3);
    }
    Pb sub()
    {
        const quint64 n = varint();
        if (!ok || n > quint64(end - p)) { ok = false; return Pb(nullptr, 0); }
        Pb s(reinterpret_cast<const char *>(p), int(n));
        p += n;
        return s;
    }
    QByteArray bytes()
    {
        const quint64 n = varint();
        if (!ok || n > quint64(end - p)) { ok = false; return QByteArray(); }
        QByteArray b(reinterpret_cast<const char *>(p), int(n));
        p += n;
        return b;
    }
    quint32 fixed32()
    {
        if (end - p < 4) { ok = false; return 0; }
        quint32 v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    quint64 fixed64()
    {
        if (end - p < 8) { ok = false; return 0; }
        quint64 v;
        std::memcpy(&v, p, 8);
        p += 8;
        return v;
    }
    void skip(int wt)
    {
        switch (wt) {
        case 0: varint(); break;
        case 1: if (end - p < 8) ok = false; else p += 8; break;
        case 2: { const quint64 n = varint(); if (!ok || n > quint64(end - p)) ok = false; else p += n; break; }
        case 5: if (end - p < 4) ok = false; else p += 4; break;
        default: ok = false;
        }
    }
};

inline qint64 zigzag(quint64 v) { return qint64(v >> 1) ^ -qint64(v & 1); }

QVariant readValue(Pb v)
{
    QVariant out;
    while (!v.atEnd()) {
        int wt;
        switch (v.key(&wt)) {
        case 1: out = QString::fromUtf8(v.bytes()); break;
        case 2: { const quint32 b = v.fixed32(); float f; std::memcpy(&f, &b, 4); out = double(f); break; }
        case 3: { const quint64 b = v.fixed64(); double d; std::memcpy(&d, &b, 8); out = d; break; }
        case 4: out = qint64(v.varint()); break;
        case 5: out = quint64(v.varint()); break;
        case 6: out = zigzag(v.varint()); break;
        case 7: out = v.varint() != 0; break;
        default: v.skip(wt);
        }
    }
    return out;
}

void decodeGeometry(Pb g, VtFeature &f)
{
    qint32 x = 0, y = 0;
    QVector<QPoint> cur;
    while (!g.atEnd()) {
        const quint32 cmdInt = quint32(g.varint());
        const int cmd = cmdInt & 7, count = int(cmdInt >> 3);
        if (cmd == 1 || cmd == 2) { // MoveTo / LineTo
            for (int i = 0; i < count && !g.atEnd(); ++i) {
                x += qint32(zigzag(g.varint()));
                y += qint32(zigzag(g.varint()));
                if (cmd == 1) {
                    if (f.type == VtFeature::Point) { f.parts.append({QPoint(x, y)}); continue; }
                    if (cur.size() > 1) f.parts.append(cur);
                    cur.clear();
                }
                cur.append(QPoint(x, y));
            }
        } else if (cmd == 7) { // ClosePath
            if (!cur.isEmpty()) cur.append(cur.first());
        } else {
            return;
        }
    }
    if (cur.size() > 1) f.parts.append(cur);
}
} // namespace

QByteArray VectorTile::gunzip(const QByteArray &data)
{
    z_stream zs;
    std::memset(&zs, 0, sizeof zs);
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return QByteArray(); // gzip or zlib header
    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    zs.avail_in = uInt(data.size());
    QByteArray out;
    char buf[65536];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef *>(buf);
        zs.avail_out = sizeof buf;
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return QByteArray(); }
        out.append(buf, int(sizeof buf - zs.avail_out));
    } while (ret != Z_STREAM_END && out.size() < 64 * 1024 * 1024);
    inflateEnd(&zs);
    return out;
}

bool VectorTile::parse(const QByteArray &raw)
{
    layers.clear();
    QByteArray data = raw;
    if (data.size() > 2 && quint8(data[0]) == 0x1f && quint8(data[1]) == 0x8b) data = gunzip(data);
    if (data.isEmpty()) return false;
    Pb tile(data.constData(), data.size());
    while (!tile.atEnd()) {
        int wt;
        const int field = tile.key(&wt);
        if (field != 3 || wt != 2) { tile.skip(wt); continue; }
        Pb lp = tile.sub();
        VtLayer layer;
        QStringList keys;
        QVector<QVariant> values;
        QVector<Pb> featureBufs;
        while (!lp.atEnd()) {
            int lwt;
            switch (lp.key(&lwt)) {
            case 1: layer.name = QString::fromUtf8(lp.bytes()); break;
            case 2: featureBufs.append(lp.sub()); break;
            case 3: keys.append(QString::fromUtf8(lp.bytes())); break;
            case 4: values.append(readValue(lp.sub())); break;
            case 5: layer.extent = int(lp.varint()); break;
            default: lp.skip(lwt);
            }
        }
        if (!lp.ok) return false;
        layer.features.reserve(featureBufs.size());
        for (Pb fb : featureBufs) {
            VtFeature f;
            Pb geom(nullptr, 0);
            bool haveGeom = false;
            while (!fb.atEnd()) {
                int fwt;
                switch (fb.key(&fwt)) {
                case 2: {
                    Pb t = fb.sub();
                    while (!t.atEnd()) {
                        const quint32 k = quint32(t.varint()), v = quint32(t.varint());
                        if (int(k) < keys.size() && int(v) < values.size()) f.tags.insert(keys[int(k)], values[int(v)]);
                    }
                    break;
                }
                case 3: f.type = int(fb.varint()); break;
                case 4: geom = fb.sub(); haveGeom = true; break;
                default: fb.skip(fwt);
                }
            }
            if (haveGeom) decodeGeometry(geom, f);
            if (!f.parts.isEmpty()) layer.features.append(f);
        }
        if (layer.extent <= 0) layer.extent = 4096;
        layers.insert(layer.name, layer);
    }
    return tile.ok;
}

const VtLayer *VectorTile::layer(const QString &name) const
{
    auto it = layers.constFind(name);
    return it == layers.constEnd() ? nullptr : &*it;
}
