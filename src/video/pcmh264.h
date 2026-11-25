// Tiny dependency-free H.264 encoder for UI content.
//
// Changed 16x16 macroblocks are sent uncompressed (I_PCM), unchanged ones
// as P_Skip, so a static car UI costs almost nothing and every frame is
// lossless. Baseline profile, CAVLC, deblocking off, one reference frame.
// Used when no GStreamer H.264 encoder works (e.g. droidvenc on phones
// that only accept camera buffers).
#pragma once

#include <QByteArray>
#include <QImage>
#include <QVector>

class PcmH264Encoder
{
public:
    void reset(int width, int height);

    // SPS + PPS (Annex-B, with start codes).
    QByteArray codecConfig() const;

    // Encodes one RGB32 frame into an Annex-B access unit.
    QByteArray encode(const QImage &frame, bool forceKeyframe = false);

    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    void toYuv(const QImage &img);
    bool mbChanged(int mbx, int mby) const;
    void writeMb(class BitWriter &bw, int mbx, int mby) const;

    int m_width = 0, m_height = 0;
    int m_mbw = 0, m_mbh = 0;
    int m_frameNum = 0;
    bool m_haveRef = false;
    QVector<quint8> m_y, m_u, m_v;          // current frame (padded to MB size)
    QVector<quint8> m_refY, m_refU, m_refV; // what the decoder has
};
