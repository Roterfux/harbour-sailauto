#include "openh264enc.h"

#include <codec_api.h>
#include <cstring>

OpenH264Encoder::~OpenH264Encoder()
{
    stop();
}

bool OpenH264Encoder::start(int width, int height, int fps, int bitrateKbps, QString *error)
{
    stop();
    // Encoder input must have even dimensions (4:2:0).
    m_width = width & ~1;
    m_height = height & ~1;

    if (WelsCreateSVCEncoder(&m_enc) != 0 || !m_enc) {
        if (error) *error = QStringLiteral("openh264: WelsCreateSVCEncoder fehlgeschlagen");
        m_enc = nullptr;
        return false;
    }

    SEncParamExt p;
    m_enc->GetDefaultParams(&p);
    p.iUsageType = SCREEN_CONTENT_REAL_TIME;
    p.iPicWidth = m_width;
    p.iPicHeight = m_height;
    p.iTargetBitrate = bitrateKbps * 1000;
    p.iMaxBitrate = bitrateKbps * 2000;
    p.iRCMode = RC_BITRATE_MODE;
    p.fMaxFrameRate = static_cast<float>(fps);
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.bEnableFrameSkip = false;      // pacing is done by the HU's acks
    p.uiIntraPeriod = static_cast<unsigned int>(fps * 2);
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.bPrefixNalAddingCtrl = false;
    p.iEntropyCodingModeFlag = 0;    // CAVLC, Baseline
    p.iMultipleThreadIdc = 2;
    p.iComplexityMode = LOW_COMPLEXITY;
    p.bEnableDenoise = false;
    p.bEnableBackgroundDetection = true;
    p.bEnableAdaptiveQuant = false;
    p.bEnableLongTermReference = false;
    p.iLoopFilterDisableIdc = 0;

    SSpatialLayerConfig &l = p.sSpatialLayers[0];
    l.iVideoWidth = m_width;
    l.iVideoHeight = m_height;
    l.fFrameRate = static_cast<float>(fps);
    l.iSpatialBitrate = p.iTargetBitrate;
    l.iMaxSpatialBitrate = p.iMaxBitrate;
    l.uiProfileIdc = PRO_BASELINE;
    l.uiLevelIdc = LEVEL_4_0;
    l.sSliceArgument.uiSliceMode = SM_SINGLE_SLICE;

    const int rc = m_enc->InitializeExt(&p);
    if (rc != 0) {
        if (error) *error = QStringLiteral("openh264: InitializeExt Fehler %1").arg(rc);
        stop();
        return false;
    }
    int fmt = videoFormatI420;
    m_enc->SetOption(ENCODER_OPTION_DATAFORMAT, &fmt);

    m_y.resize(m_width * m_height);
    m_u.resize(m_width * m_height / 4);
    m_v.resize(m_width * m_height / 4);
    return true;
}

void OpenH264Encoder::stop()
{
    if (!m_enc) return;
    m_enc->Uninitialize();
    WelsDestroySVCEncoder(m_enc);
    m_enc = nullptr;
}

void OpenH264Encoder::toI420(const QImage &src)
{
    const QImage img = src.format() == QImage::Format_RGB32 || src.format() == QImage::Format_ARGB32
                           ? src : src.convertToFormat(QImage::Format_RGB32);
    const int w = qMin(m_width, img.width() & ~1), h = qMin(m_height, img.height() & ~1);
    const int cw = m_width / 2;
    // BT.601 limited range, 2x2 chroma average.
    for (int y = 0; y < h; y += 2) {
        const QRgb *r0 = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        const QRgb *r1 = reinterpret_cast<const QRgb *>(img.constScanLine(y + 1));
        quint8 *y0 = m_y.data() + y * m_width;
        quint8 *y1 = y0 + m_width;
        quint8 *u = m_u.data() + (y / 2) * cw;
        quint8 *v = m_v.data() + (y / 2) * cw;
        for (int x = 0; x < w; x += 2) {
            const QRgb px[4] = {r0[x], r0[x + 1], r1[x], r1[x + 1]};
            quint8 *dst[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            int su = 0, sv = 0;
            for (int i = 0; i < 4; ++i) {
                const int R = qRed(px[i]), G = qGreen(px[i]), B = qBlue(px[i]);
                *dst[i] = static_cast<quint8>(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
                su += ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                sv += ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
            }
            u[x / 2] = static_cast<quint8>(su / 4);
            v[x / 2] = static_cast<quint8>(sv / 4);
        }
    }
}

QByteArray OpenH264Encoder::encode(const QImage &frame, quint64 timestampUs, bool forceKeyframe)
{
    if (!m_enc) return QByteArray();
    toI420(frame);

    SSourcePicture pic;
    std::memset(&pic, 0, sizeof(pic));
    pic.iColorFormat = videoFormatI420;
    pic.iPicWidth = m_width;
    pic.iPicHeight = m_height;
    pic.iStride[0] = m_width;
    pic.iStride[1] = pic.iStride[2] = m_width / 2;
    pic.pData[0] = m_y.data();
    pic.pData[1] = m_u.data();
    pic.pData[2] = m_v.data();
    pic.uiTimeStamp = static_cast<long long>(timestampUs / 1000);

    if (forceKeyframe) m_enc->ForceIntraFrame(true);

    SFrameBSInfo info;
    std::memset(&info, 0, sizeof(info));
    if (m_enc->EncodeFrame(&pic, &info) != cmResultSuccess || info.eFrameType == videoFrameTypeSkip)
        return QByteArray();

    QByteArray out;
    for (int i = 0; i < info.iLayerNum; ++i) {
        const SLayerBSInfo &layer = info.sLayerInfo[i];
        int len = 0;
        for (int n = 0; n < layer.iNalCount; ++n) len += layer.pNalLengthInByte[n];
        out.append(reinterpret_cast<const char *>(layer.pBsBuf), len);
    }
    return out;
}
