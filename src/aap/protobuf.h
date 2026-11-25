// Minimal protobuf wire-format encoder/decoder.
// Enough for the Android Auto Projection (AAP) messages we need, without
// pulling protoc/libprotobuf onto the device.
#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <cstdint>

namespace pb {

enum WireType { Varint = 0, Fixed64 = 1, LengthDelimited = 2, Fixed32 = 5 };

class Writer
{
public:
    Writer &varint(int field, uint64_t v) { key(field, Varint); raw(v); return *this; }
    Writer &sint(int field, int64_t v) { return varint(field, static_cast<uint64_t>(v)); }
    Writer &boolean(int field, bool v) { return varint(field, v ? 1 : 0); }
    Writer &bytes(int field, const QByteArray &v)
    {
        key(field, LengthDelimited);
        raw(static_cast<uint64_t>(v.size()));
        m_buf.append(v);
        return *this;
    }
    Writer &string(int field, const QString &v) { return bytes(field, v.toUtf8()); }
    Writer &message(int field, const Writer &w) { return bytes(field, w.data()); }
    const QByteArray &data() const { return m_buf; }

private:
    void key(int field, WireType t) { raw((static_cast<uint64_t>(field) << 3) | t); }
    void raw(uint64_t v)
    {
        do {
            uint8_t b = v & 0x7f;
            v >>= 7;
            if (v) b |= 0x80;
            m_buf.append(static_cast<char>(b));
        } while (v);
    }
    QByteArray m_buf;
};

// A parsed field value: either a varint/fixed number or a byte blob.
struct Value
{
    WireType type = Varint;
    uint64_t number = 0;
    QByteArray bytes;
};

// Parses one message level. Repeated fields keep their order.
class Reader
{
public:
    explicit Reader(const QByteArray &data) { m_ok = parse(data); }
    bool ok() const { return m_ok; }

    bool has(int field) const { return m_fields.contains(field); }
    uint64_t number(int field, uint64_t def = 0) const
    {
        auto it = m_fields.constFind(field);
        return it == m_fields.constEnd() || it->isEmpty() ? def : it->first().number;
    }
    bool boolean(int field, bool def = false) const { return number(field, def) != 0; }
    QByteArray bytes(int field) const
    {
        auto it = m_fields.constFind(field);
        return it == m_fields.constEnd() || it->isEmpty() ? QByteArray() : it->first().bytes;
    }
    QString string(int field) const { return QString::fromUtf8(bytes(field)); }
    QList<Value> all(int field) const { return m_fields.value(field); }

private:
    static bool readVarint(const QByteArray &d, int &pos, uint64_t &out)
    {
        out = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (pos >= d.size()) return false;
            uint8_t b = static_cast<uint8_t>(d[pos++]);
            out |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
        }
        return false;
    }
    bool parse(const QByteArray &d)
    {
        int pos = 0;
        while (pos < d.size()) {
            uint64_t key;
            if (!readVarint(d, pos, key)) return false;
            Value v;
            v.type = static_cast<WireType>(key & 7);
            int field = static_cast<int>(key >> 3);
            switch (v.type) {
            case Varint:
                if (!readVarint(d, pos, v.number)) return false;
                break;
            case Fixed64:
                if (pos + 8 > d.size()) return false;
                for (int i = 7; i >= 0; --i) v.number = (v.number << 8) | static_cast<uint8_t>(d[pos + i]);
                pos += 8;
                break;
            case Fixed32:
                if (pos + 4 > d.size()) return false;
                for (int i = 3; i >= 0; --i) v.number = (v.number << 8) | static_cast<uint8_t>(d[pos + i]);
                pos += 4;
                break;
            case LengthDelimited: {
                uint64_t len;
                if (!readVarint(d, pos, len) || pos + static_cast<int64_t>(len) > d.size()) return false;
                v.bytes = d.mid(pos, static_cast<int>(len));
                pos += static_cast<int>(len);
                break;
            }
            default:
                return false;
            }
            m_fields[field].append(v);
        }
        return true;
    }

    QMap<int, QList<Value>> m_fields;
    bool m_ok = false;
};

} // namespace pb
