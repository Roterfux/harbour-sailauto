# openh264 v2.7.0 encoder (BSD-2-Clause, see LICENSE), built statically into the app.
# Generated from codec/*/targets.mk. Only encoder + processing + common.
INCLUDEPATH += \
    $$PWD/codec/api/wels \
    $$PWD/codec/common/inc \
    $$PWD/codec/encoder/core/inc \
    $$PWD/codec/encoder/plus/inc \
    $$PWD/codec/processing/interface \
    $$PWD/codec/processing/src/common \
    $$PWD/codec/processing/src/adaptivequantization \
    $$PWD/codec/processing/src/downsample \
    $$PWD/codec/processing/src/scrolldetection \
    $$PWD/codec/processing/src/vaacalc

# Same file names in different folders -> keep objects apart.
CONFIG += object_parallel_to_source

SOURCES += \
    $$PWD/codec/common/src/common_tables.cpp \
    $$PWD/codec/common/src/copy_mb.cpp \
    $$PWD/codec/common/src/cpu.cpp \
    $$PWD/codec/common/src/crt_util_safe_x.cpp \
    $$PWD/codec/common/src/deblocking_common.cpp \
    $$PWD/codec/common/src/expand_pic.cpp \
    $$PWD/codec/common/src/intra_pred_common.cpp \
    $$PWD/codec/common/src/mc.cpp \
    $$PWD/codec/common/src/memory_align.cpp \
    $$PWD/codec/common/src/sad_common.cpp \
    $$PWD/codec/common/src/utils.cpp \
    $$PWD/codec/common/src/welsCodecTrace.cpp \
    $$PWD/codec/common/src/WelsTaskThread.cpp \
    $$PWD/codec/common/src/WelsThread.cpp \
    $$PWD/codec/common/src/WelsThreadLib.cpp \
    $$PWD/codec/common/src/WelsThreadPool.cpp \
    $$PWD/codec/encoder/core/src/au_set.cpp \
    $$PWD/codec/encoder/core/src/deblocking.cpp \
    $$PWD/codec/encoder/core/src/decode_mb_aux.cpp \
    $$PWD/codec/encoder/core/src/encode_mb_aux.cpp \
    $$PWD/codec/encoder/core/src/encoder.cpp \
    $$PWD/codec/encoder/core/src/encoder_data_tables.cpp \
    $$PWD/codec/encoder/core/src/encoder_ext.cpp \
    $$PWD/codec/encoder/core/src/get_intra_predictor.cpp \
    $$PWD/codec/encoder/core/src/md.cpp \
    $$PWD/codec/encoder/core/src/mv_pred.cpp \
    $$PWD/codec/encoder/core/src/nal_encap.cpp \
    $$PWD/codec/encoder/core/src/paraset_strategy.cpp \
    $$PWD/codec/encoder/core/src/picture_handle.cpp \
    $$PWD/codec/encoder/core/src/ratectl.cpp \
    $$PWD/codec/encoder/core/src/ref_list_mgr_svc.cpp \
    $$PWD/codec/encoder/core/src/sample.cpp \
    $$PWD/codec/encoder/core/src/set_mb_syn_cabac.cpp \
    $$PWD/codec/encoder/core/src/set_mb_syn_cavlc.cpp \
    $$PWD/codec/encoder/core/src/slice_multi_threading.cpp \
    $$PWD/codec/encoder/core/src/svc_base_layer_md.cpp \
    $$PWD/codec/encoder/core/src/svc_enc_slice_segment.cpp \
    $$PWD/codec/encoder/core/src/svc_encode_mb.cpp \
    $$PWD/codec/encoder/core/src/svc_encode_slice.cpp \
    $$PWD/codec/encoder/core/src/svc_mode_decision.cpp \
    $$PWD/codec/encoder/core/src/svc_motion_estimate.cpp \
    $$PWD/codec/encoder/core/src/svc_set_mb_syn_cabac.cpp \
    $$PWD/codec/encoder/core/src/svc_set_mb_syn_cavlc.cpp \
    $$PWD/codec/encoder/core/src/wels_preprocess.cpp \
    $$PWD/codec/encoder/core/src/wels_task_base.cpp \
    $$PWD/codec/encoder/core/src/wels_task_encoder.cpp \
    $$PWD/codec/encoder/core/src/wels_task_management.cpp \
    $$PWD/codec/encoder/plus/src/welsEncoderExt.cpp \
    $$PWD/codec/processing/src/adaptivequantization/AdaptiveQuantization.cpp \
    $$PWD/codec/processing/src/backgrounddetection/BackgroundDetection.cpp \
    $$PWD/codec/processing/src/common/memory.cpp \
    $$PWD/codec/processing/src/common/WelsFrameWork.cpp \
    $$PWD/codec/processing/src/common/WelsFrameWorkEx.cpp \
    $$PWD/codec/processing/src/complexityanalysis/ComplexityAnalysis.cpp \
    $$PWD/codec/processing/src/denoise/denoise.cpp \
    $$PWD/codec/processing/src/denoise/denoise_filter.cpp \
    $$PWD/codec/processing/src/downsample/downsample.cpp \
    $$PWD/codec/processing/src/downsample/downsamplefuncs.cpp \
    $$PWD/codec/processing/src/imagerotate/imagerotate.cpp \
    $$PWD/codec/processing/src/imagerotate/imagerotatefuncs.cpp \
    $$PWD/codec/processing/src/scenechangedetection/SceneChangeDetection.cpp \
    $$PWD/codec/processing/src/scrolldetection/ScrollDetection.cpp \
    $$PWD/codec/processing/src/scrolldetection/ScrollDetectionFuncs.cpp \
    $$PWD/codec/processing/src/vaacalc/vaacalcfuncs.cpp \
    $$PWD/codec/processing/src/vaacalc/vaacalculation.cpp

contains(QT_ARCH, arm64)|contains(QT_ARCH, aarch64) {
    DEFINES += HAVE_NEON_AARCH64
    INCLUDEPATH += $$PWD/codec/common/arm64
    OPENH264_ASM_INC = -I$$PWD/codec/common/arm64
    OPENH264_ASM += \
    $$PWD/codec/common/arm64/copy_mb_aarch64_neon.S \
    $$PWD/codec/common/arm64/deblocking_aarch64_neon.S \
    $$PWD/codec/common/arm64/expand_picture_aarch64_neon.S \
    $$PWD/codec/common/arm64/intra_pred_common_aarch64_neon.S \
    $$PWD/codec/common/arm64/mc_aarch64_neon.S \
    $$PWD/codec/encoder/core/arm64/intra_pred_aarch64_neon.S \
    $$PWD/codec/encoder/core/arm64/intra_pred_sad_3_opt_aarch64_neon.S \
    $$PWD/codec/encoder/core/arm64/memory_aarch64_neon.S \
    $$PWD/codec/encoder/core/arm64/pixel_aarch64_neon.S \
    $$PWD/codec/encoder/core/arm64/reconstruct_aarch64_neon.S \
    $$PWD/codec/encoder/core/arm64/svc_motion_estimation_aarch64_neon.S \
    $$PWD/codec/processing/src/arm64/adaptive_quantization_aarch64_neon.S \
    $$PWD/codec/processing/src/arm64/down_sample_aarch64_neon.S \
    $$PWD/codec/processing/src/arm64/pixel_sad_aarch64_neon.S \
    $$PWD/codec/processing/src/arm64/vaa_calc_aarch64_neon.S
} else:contains(QT_ARCH, arm) {
    DEFINES += HAVE_NEON
    INCLUDEPATH += $$PWD/codec/common/arm
    QMAKE_CFLAGS += -mfpu=neon
    QMAKE_CXXFLAGS += -mfpu=neon
    OPENH264_ASM_INC = -I$$PWD/codec/common/arm
    OPENH264_ASM += \
    $$PWD/codec/common/arm/copy_mb_neon.S \
    $$PWD/codec/common/arm/deblocking_neon.S \
    $$PWD/codec/common/arm/expand_picture_neon.S \
    $$PWD/codec/common/arm/intra_pred_common_neon.S \
    $$PWD/codec/common/arm/mc_neon.S \
    $$PWD/codec/encoder/core/arm/intra_pred_neon.S \
    $$PWD/codec/encoder/core/arm/intra_pred_sad_3_opt_neon.S \
    $$PWD/codec/encoder/core/arm/memory_neon.S \
    $$PWD/codec/encoder/core/arm/pixel_neon.S \
    $$PWD/codec/encoder/core/arm/reconstruct_neon.S \
    $$PWD/codec/encoder/core/arm/svc_motion_estimation.S \
    $$PWD/codec/processing/src/arm/adaptive_quantization.S \
    $$PWD/codec/processing/src/arm/down_sample_neon.S \
    $$PWD/codec/processing/src/arm/pixel_sad_neon.S \
    $$PWD/codec/processing/src/arm/vaa_calc_neon.S
}

# NEON assembly (.S, needs the C preprocessor) via the C compiler.
openh264_asm.input = OPENH264_ASM
openh264_asm.output = ${QMAKE_VAR_OBJECTS_DIR}${QMAKE_FILE_BASE}_asm.o
openh264_asm.commands = $$QMAKE_CC -c $(CFLAGS) $$OPENH264_ASM_INC $(DEFINES) ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
openh264_asm.variable_out = OBJECTS
QMAKE_EXTRA_COMPILERS += openh264_asm
