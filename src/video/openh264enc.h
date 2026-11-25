// Software H.264 encoder: openh264 (Cisco, BSD-2-Clause), compiled into the
// app from third_party/openh264. Uses the "screen content" mode, which is
// made for UI pictures. Baseline profile, CAVLC, Annex-B output.
#pragma once

#include <QByteArray>
#include <QImage>
#include <QVector>

class ISVCEncoder;

class OpenH264Encoder
{
public:
    OpenH264Encoder() = default;
    ~OpenH264Encoder();

    bool start(int width, int height, int fps, int bitrateKbps, QString *error);
    void stop();
    bool running() const { return m_enc != nullptr; }

    // Annex-B access unit (the first one starts with SPS/PPS).
    // Empty when the encoder decided to skip the frame.
    QByteArray encode(const QImage &frame, quint64 timestampUs, bool forceKeyframe = false);

private:
    void toI420(const QImage &img);

    ISVCEncoder *m_enc = nullptr;
    int m_width = 0, m_height = 0;
    QVector<quint8> m_y, m_u, m_v;
};
