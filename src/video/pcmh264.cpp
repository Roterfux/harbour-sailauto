#include "pcmh264.h"

#include <cstring>

class BitWriter
{
public:
    void bits(quint32 value, int n)
    {
        for (int i = n - 1; i >= 0; --i) bit((value >> i) & 1);
    }
    void bit(int b)
    {
        m_cur = static_cast<quint8>((m_cur << 1) | (b & 1));
        if (++m_n == 8) flushByte();
    }
    void ue(quint32 v)
    {
        const quint32 x = v + 1;
        int len = 0;
        while ((x >> len) > 1) ++len;
        bits(0, len);
        bits(x, len + 1);
    }
    void se(qint32 v) { ue(v <= 0 ? static_cast<quint32>(-2 * v) : static_cast<quint32>(2 * v - 1)); }
    void alignZero() { while (m_n) bit(0); }
    void bytes(const quint8 *p, int n)
    {
        // Only called when byte aligned.
        m_out.append(reinterpret_cast<const char *>(p), n);
    }
    void trailing()
    {
        bit(1);
        alignZero();
    }
    const QByteArray &data() const { return m_out; }

private:
    void flushByte()
    {
        m_out.append(static_cast<char>(m_cur));
        m_cur = 0;
        m_n = 0;
    }
    QByteArray m_out;
    quint8 m_cur = 0;
    int m_n = 0;
};

namespace {

// RBSP -> NAL with start code and emulation prevention bytes.
QByteArray nal(int refIdc, int type, const QByteArray &rbsp)
{
    QByteArray out;
    out.reserve(rbsp.size() + rbsp.size() / 64 + 8);
    out.append("\x00\x00\x00\x01", 4);
    out.append(static_cast<char>((refIdc << 5) | type));
    int zeros = 0;
    for (char c : rbsp) {
        const quint8 b = static_cast<quint8>(c);
        if (zeros >= 2 && b <= 3) {
            out.append('\x03');
            zeros = 0;
        }
        out.append(c);
        zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
}

inline quint8 clampPcm(int v)
{
    // Older decoders reject pcm_sample == 0.
    return static_cast<quint8>(v < 1 ? 1 : (v > 255 ? 255 : v));
}

} // namespace

void PcmH264Encoder::reset(int width, int height)
{
    m_width = width;
    m_height = height;
    m_mbw = (width + 15) / 16;
    m_mbh = (height + 15) / 16;
    const int lw = m_mbw * 16, lh = m_mbh * 16;
    m_y.fill(16, lw * lh);
    m_u.fill(128, lw * lh / 4);
    m_v.fill(128, lw * lh / 4);
    m_refY = m_y;
    m_refU = m_u;
    m_refV = m_v;
    m_frameNum = 0;
    m_haveRef = false;
}

QByteArray PcmH264Encoder::codecConfig() const
{
    BitWriter sps;
    sps.bits(66, 8);   // profile_idc: Baseline
    sps.bits(0xC0, 8); // constraint_set0 + set1 -> Constrained Baseline
    sps.bits(40, 8);   // level 4.0
    sps.ue(0);         // seq_parameter_set_id
    sps.ue(0);         // log2_max_frame_num_minus4 -> 4 bits
    sps.ue(2);         // pic_order_cnt_type 2 (output order = decode order)
    sps.ue(1);         // max_num_ref_frames
    sps.bit(0);        // gaps_in_frame_num_value_allowed_flag
    sps.ue(m_mbw - 1);
    sps.ue(m_mbh - 1);
    sps.bit(1);        // frame_mbs_only_flag
    sps.bit(1);        // direct_8x8_inference_flag
    const int cropR = (m_mbw * 16 - m_width) / 2, cropB = (m_mbh * 16 - m_height) / 2;
    if (cropR || cropB) {
        sps.bit(1);
        sps.ue(0);
        sps.ue(static_cast<quint32>(cropR));
        sps.ue(0);
        sps.ue(static_cast<quint32>(cropB));
    } else {
        sps.bit(0);
    }
    // VUI with bitstream_restriction: tell decoders there is no reordering,
    // so they output every frame immediately instead of buffering.
    sps.bit(1);        // vui_parameters_present_flag
    sps.bit(0);        // aspect_ratio_info_present_flag
    sps.bit(0);        // overscan_info_present_flag
    sps.bit(0);        // video_signal_type_present_flag
    sps.bit(0);        // chroma_loc_info_present_flag
    sps.bit(0);        // timing_info_present_flag
    sps.bit(0);        // nal_hrd_parameters_present_flag
    sps.bit(0);        // vcl_hrd_parameters_present_flag
    sps.bit(0);        // pic_struct_present_flag
    sps.bit(1);        // bitstream_restriction_flag
    sps.bit(1);        // motion_vectors_over_pic_boundaries_flag
    sps.ue(0);         // max_bytes_per_pic_denom
    sps.ue(0);         // max_bits_per_mb_denom
    sps.ue(0);         // log2_max_mv_length_horizontal
    sps.ue(0);         // log2_max_mv_length_vertical
    sps.ue(0);         // max_num_reorder_frames
    sps.ue(1);         // max_dec_frame_buffering
    sps.trailing();

    BitWriter pps;
    pps.ue(0);   // pic_parameter_set_id
    pps.ue(0);   // seq_parameter_set_id
    pps.bit(0);  // entropy_coding_mode_flag: CAVLC
    pps.bit(0);  // bottom_field_pic_order_in_frame_present_flag
    pps.ue(0);   // num_slice_groups_minus1
    pps.ue(0);   // num_ref_idx_l0_default_active_minus1
    pps.ue(0);   // num_ref_idx_l1_default_active_minus1
    pps.bit(0);  // weighted_pred_flag
    pps.bits(0, 2); // weighted_bipred_idc
    pps.se(0);   // pic_init_qp_minus26
    pps.se(0);   // pic_init_qs_minus26
    pps.se(0);   // chroma_qp_index_offset
    pps.bit(1);  // deblocking_filter_control_present_flag
    pps.bit(0);  // constrained_intra_pred_flag
    pps.bit(0);  // redundant_pic_cnt_present_flag
    pps.trailing();

    return nal(3, 7, sps.data()) + nal(3, 8, pps.data());
}

void PcmH264Encoder::toYuv(const QImage &src)
{
    const QImage img = src.format() == QImage::Format_RGB32 || src.format() == QImage::Format_ARGB32
                           ? src : src.convertToFormat(QImage::Format_RGB32);
    const int lw = m_mbw * 16;
    const int cw = lw / 2;
    const int h = qMin(img.height(), m_height), w = qMin(img.width(), m_width);
    // BT.601 limited range, integer math.
    for (int y = 0; y < h; y += 2) {
        const QRgb *r0 = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        const QRgb *r1 = reinterpret_cast<const QRgb *>(img.constScanLine(qMin(y + 1, h - 1)));
        quint8 *y0 = m_y.data() + y * lw;
        quint8 *y1 = y0 + lw;
        for (int x = 0; x < w; x += 2) {
            int su = 0, sv = 0;
            const QRgb px[4] = {r0[x], r0[qMin(x + 1, w - 1)], r1[x], r1[qMin(x + 1, w - 1)]};
            quint8 *dst[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int i = 0; i < 4; ++i) {
                const int R = qRed(px[i]), G = qGreen(px[i]), B = qBlue(px[i]);
                *dst[i] = static_cast<quint8>(((66 * R + 129 * G + 25 * B + 128) >> 8) + 16);
                su += ((-38 * R - 74 * G + 112 * B + 128) >> 8) + 128;
                sv += ((112 * R - 94 * G - 18 * B + 128) >> 8) + 128;
            }
            m_u[(y / 2) * cw + x / 2] = static_cast<quint8>(su / 4);
            m_v[(y / 2) * cw + x / 2] = static_cast<quint8>(sv / 4);
        }
    }
}

bool PcmH264Encoder::mbChanged(int mbx, int mby) const
{
    const int lw = m_mbw * 16, cw = lw / 2;
    for (int r = 0; r < 16; ++r) {
        const int off = (mby * 16 + r) * lw + mbx * 16;
        if (std::memcmp(m_y.constData() + off, m_refY.constData() + off, 16)) return true;
    }
    for (int r = 0; r < 8; ++r) {
        const int off = (mby * 8 + r) * cw + mbx * 8;
        if (std::memcmp(m_u.constData() + off, m_refU.constData() + off, 8)
            || std::memcmp(m_v.constData() + off, m_refV.constData() + off, 8))
            return true;
    }
    return false;
}

void PcmH264Encoder::writeMb(BitWriter &bw, int mbx, int mby) const
{
    bw.alignZero(); // pcm_alignment_zero_bit
    const int lw = m_mbw * 16, cw = lw / 2;
    quint8 buf[384];
    int n = 0;
    for (int r = 0; r < 16; ++r)
        for (int c = 0; c < 16; ++c)
            buf[n++] = clampPcm(m_y[(mby * 16 + r) * lw + mbx * 16 + c]);
    for (const QVector<quint8> *plane : {&m_u, &m_v})
        for (int r = 0; r < 8; ++r)
            for (int c = 0; c < 8; ++c)
                buf[n++] = clampPcm((*plane)[(mby * 8 + r) * cw + mbx * 8 + c]);
    bw.bytes(buf, 384);
}

QByteArray PcmH264Encoder::encode(const QImage &frame, bool forceKeyframe)
{
    toYuv(frame);
    const bool idr = forceKeyframe || !m_haveRef;

    BitWriter bw;
    if (idr) {
        m_frameNum = 0;
        bw.ue(0);           // first_mb_in_slice
        bw.ue(7);           // slice_type: I (all slices)
        bw.ue(0);           // pic_parameter_set_id
        bw.bits(0, 4);      // frame_num
        bw.ue(0);           // idr_pic_id
        bw.bit(0);          // no_output_of_prior_pics_flag
        bw.bit(0);          // long_term_reference_flag
        bw.se(0);           // slice_qp_delta
        bw.ue(1);           // disable_deblocking_filter_idc: off
        for (int my = 0; my < m_mbh; ++my)
            for (int mx = 0; mx < m_mbw; ++mx) {
                bw.ue(25);  // mb_type I_PCM
                writeMb(bw, mx, my);
            }
        bw.trailing();
    } else {
        // Unchanged frames become an all-skip P frame (a few bytes). Sending
        // them keeps decoders that need a steady stream from stalling.
        QVector<bool> changed(m_mbw * m_mbh);
        for (int my = 0; my < m_mbh; ++my)
            for (int mx = 0; mx < m_mbw; ++mx)
                changed[my * m_mbw + mx] = mbChanged(mx, my);

        m_frameNum = (m_frameNum + 1) & 15;
        bw.ue(0);           // first_mb_in_slice
        bw.ue(5);           // slice_type: P (all slices)
        bw.ue(0);           // pic_parameter_set_id
        bw.bits(static_cast<quint32>(m_frameNum), 4);
        bw.bit(0);          // num_ref_idx_active_override_flag
        bw.bit(0);          // ref_pic_list_modification_flag_l0
        bw.bit(0);          // adaptive_ref_pic_marking_mode_flag
        bw.se(0);           // slice_qp_delta
        bw.ue(1);           // disable_deblocking_filter_idc: off
        // All motion vectors are zero, so P_Skip copies the reference MB 1:1.
        quint32 skip = 0;
        for (int i = 0; i < m_mbw * m_mbh; ++i) {
            if (!changed[i]) {
                ++skip;
                continue;
            }
            bw.ue(skip);    // mb_skip_run
            skip = 0;
            bw.ue(5 + 25);  // mb_type I_PCM inside a P slice
            writeMb(bw, i % m_mbw, i / m_mbw);
        }
        if (skip) bw.ue(skip);
        bw.trailing();
    }

    m_refY = m_y;
    m_refU = m_u;
    m_refV = m_v;
    m_haveRef = true;
    return idr ? nal(3, 5, bw.data()) : nal(2, 1, bw.data());
}
