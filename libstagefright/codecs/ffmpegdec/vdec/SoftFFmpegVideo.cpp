/*
 * Copyright 2012 Michael Chen <omxcodec@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDEBUG 0
#define LOG_TAG "SoftFFmpegVideo"
#include <utils/Log.h>

#include "SoftFFmpegVideo.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaDefs.h>

#include "utils/ffmpeg_utils.h"

#define DEBUG_PKT 0
#define DEBUG_FRM 0

static int decoder_reorder_pts = -1;

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

void SoftFFmpegVideo::setMode(const char *name) {
    if (!strcmp(name, "OMX.ffmpeg.mpeg2v.decoder")) {
        mMode = MODE_MPEG2;
    } else if (!strcmp(name, "OMX.ffmpeg.h263.decoder")) {
        mMode = MODE_H263;
	} else if (!strcmp(name, "OMX.ffmpeg.mpeg4.decoder")) {
        mMode = MODE_MPEG4;
    } else if (!strcmp(name, "OMX.ffmpeg.wmv.decoder")) {
        mMode = MODE_WMV;
    } else if (!strcmp(name, "OMX.ffmpeg.rv.decoder")) {
        mMode = MODE_RV;
	} else if (!strcmp(name, "OMX.ffmpeg.h264.decoder")) {
        mMode = MODE_H264;
    } else if (!strcmp(name, "OMX.ffmpeg.vpx.decoder")) {
        mMode = MODE_VPX;
    } else if (!strcmp(name, "OMX.ffmpeg.vc1.decoder")) {
        mMode = MODE_VC1;
    } else if (!strcmp(name, "OMX.ffmpeg.flv1.decoder")) {
        mMode = MODE_FLV1;
    } else if (!strcmp(name, "OMX.ffmpeg.divx.decoder")) {
        mMode = MODE_DIVX;
    } else if (!strcmp(name, "OMX.ffmpeg.hevc.decoder")) {
        mMode = MODE_HEVC;
    } else if (!strcmp(name, "OMX.ffmpeg.vtrial.decoder")) {
        mMode = MODE_TRIAL;
    } else {
        TRESPASS();
    }
}

SoftFFmpegVideo::SoftFFmpegVideo(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mMode(MODE_NONE),
      mFFmpegAlreadyInited(false),
      mCodecAlreadyOpened(false),
      mPendingFrameAsSettingChanged(false),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mFrame(NULL),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mSignalledError(false),
      mDoDeinterlace(true),
      mWidth(320),
      mHeight(240),
      mStride(320),
      mOutputPortSettingsChange(NONE) {

    setMode(name);

    ALOGD("SoftFFmpegVideo component: %s mMode: %d", name, mMode);

    initPorts();
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftFFmpegVideo::~SoftFFmpegVideo() {
    ALOGV("~SoftFFmpegVideo");
    deInitDecoder();
    if (mFFmpegAlreadyInited) {
        deInitFFmpeg();
    }
}

void SoftFFmpegVideo::initInputFormat(uint32_t mode,
        OMX_PARAM_PORTDEFINITIONTYPE &def) {
    switch (mode) {
    case MODE_MPEG2:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_MPEG2);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
        break;
    case MODE_H263:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_H263);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingH263;
        break;
    case MODE_MPEG4:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_MPEG4);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        break;
    case MODE_WMV:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_WMV);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
        break;
    case MODE_RV:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_RV);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingRV;
        break;
    case MODE_H264:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_AVC);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
        break;
    case MODE_VPX:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_VPX);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingVPX;
        break;
    case MODE_VC1:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_VC1);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
        break;
    case MODE_FLV1:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_FLV1);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingFLV1;
        break;
    case MODE_DIVX:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_DIVX);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingDIVX;
        break;
    case MODE_HEVC:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_HEVC);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingHEVC;
        break;
    case MODE_TRIAL:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_FFMPEG);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
        break;
    default:
        CHECK(!"Should not be here. Unsupported mime type and compression format");
        break;
    }

    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mWidth;
    def.format.video.nFrameHeight = mHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    def.format.video.pNativeWindow = NULL;
}

void SoftFFmpegVideo::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 1280 * 720 * 3 / 2; // 256 * 1024?
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    initInputFormat(mMode, def);

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_RAW);
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mWidth;
    def.format.video.nFrameHeight = mHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    def.format.video.xFramerate = 0;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    def.format.video.eColorFormat = OMX_COLOR_FormatYUV420Planar;
    def.format.video.pNativeWindow = NULL;

    def.nBufferSize =
        (def.format.video.nFrameWidth * def.format.video.nFrameHeight * 3) / 2;

    addPort(def);
}

void SoftFFmpegVideo::setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec) {
    int fast = 0;

    avctx->workaround_bugs   = 1;
    avctx->lowres            = 0;
    if(avctx->lowres > codec->max_lowres){
        ALOGW("The maximum value for lowres supported by the decoder is %d",
                codec->max_lowres);
        avctx->lowres= codec->max_lowres;
    }
    avctx->idct_algo         = 0;
    avctx->skip_frame        = AVDISCARD_DEFAULT;
    avctx->skip_idct         = AVDISCARD_DEFAULT;
    avctx->skip_loop_filter  = AVDISCARD_DEFAULT;
    avctx->error_concealment = 3;

    if(avctx->lowres) avctx->flags |= CODEC_FLAG_EMU_EDGE;
    if (fast)   avctx->flags2 |= CODEC_FLAG2_FAST;
    if(codec->capabilities & CODEC_CAP_DR1)
        avctx->flags |= CODEC_FLAG_EMU_EDGE;
}

status_t SoftFFmpegVideo::initDecoder() {
    status_t status;
    
    status = initFFmpeg();
    if (status != OK) {
        return NO_INIT;
    }
    mFFmpegAlreadyInited = true;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx)
    {
        ALOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    switch (mMode) {
    case MODE_MPEG2:
        mCtx->codec_id = AV_CODEC_ID_MPEG2VIDEO;
        break;
    case MODE_H263:
        mCtx->codec_id = AV_CODEC_ID_H263;
        //FIXME, which?
        //mCtx->codec_id = AV_CODEC_ID_H263P;
        //mCtx->codec_id = AV_CODEC_ID_H263I;
        break;
    case MODE_MPEG4:
        mCtx->codec_id = AV_CODEC_ID_MPEG4;
        break;
    case MODE_WMV:
        mCtx->codec_id = AV_CODEC_ID_WMV2;	// default, adjust in "internalSetParameter" fxn
        break;
    case MODE_RV:
        mCtx->codec_id = AV_CODEC_ID_RV40;	// default, adjust in "internalSetParameter" fxn
        break;
    case MODE_H264:
        mCtx->codec_id = AV_CODEC_ID_H264;
        break;
    case MODE_VPX:
        mCtx->codec_id = AV_CODEC_ID_VP8;
        break;
    case MODE_VC1:
        mCtx->codec_id = AV_CODEC_ID_VC1;
        break;
    case MODE_FLV1:
        mCtx->codec_id = AV_CODEC_ID_FLV1;
        break;
    case MODE_DIVX:
        mCtx->codec_id = AV_CODEC_ID_MPEG4;
        break;
    case MODE_HEVC:
        mCtx->codec_id = AV_CODEC_ID_HEVC;
        break;
    case MODE_TRIAL:
        mCtx->codec_id = AV_CODEC_ID_NONE;
        break;
    default:
        CHECK(!"Should not be here. Unsupported codec");
        break;
    }

    mCtx->extradata_size = 0;
    mCtx->extradata = NULL;
    mCtx->width = mWidth;
    mCtx->height = mHeight;

    return OK;
}

void SoftFFmpegVideo::deInitDecoder() {
    if (mCtx) {
        if (avcodec_is_open(mCtx)) {
            avcodec_flush_buffers(mCtx);
        }
        if (mCtx->extradata) {
            av_free(mCtx->extradata);
            mCtx->extradata = NULL;
            mCtx->extradata_size = 0;
        }
        if (mCodecAlreadyOpened) {
            avcodec_close(mCtx);
            av_free(mCtx);
            mCtx = NULL;
        }
    }
    if (mFrame) {
        av_freep(&mFrame);
        mFrame = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
}

void SoftFFmpegVideo::getInputFormat(uint32_t mode,
        OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams) {
    switch (mode) {
    case MODE_MPEG2:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingMPEG2;
        break;
    case MODE_H263:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingH263;
        break;
    case MODE_MPEG4:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        break;
    case MODE_WMV:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingWMV;
        break;
    case MODE_RV:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingRV;
        break;
    case MODE_H264:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingAVC;
        break;
    case MODE_VPX:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingVPX;
        break;
    case MODE_VC1:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingVC1;
        break;
    case MODE_FLV1:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingFLV1;
        break;
    case MODE_DIVX:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingDIVX;
        break;
    case MODE_HEVC:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingHEVC;
        break;
    case MODE_TRIAL:
        formatParams->eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
        break;
    default:
       CHECK(!"Should not be here. Unsupported compression format.");
       break;
    }
    formatParams->eColorFormat = OMX_COLOR_FormatUnused;
    formatParams->xFramerate = 0;
}

OMX_ERRORTYPE SoftFFmpegVideo::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    //ALOGV("internalGetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            if (formatParams->nPortIndex == kInputPortIndex) {
                getInputFormat(mMode, formatParams);
            } else {
                CHECK_EQ(formatParams->nPortIndex, kOutputPortIndex);

                formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
                formatParams->eColorFormat = OMX_COLOR_FormatYUV420Planar;
                formatParams->xFramerate = 0;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *profile =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_VIDEO_WMVFormatUnused;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *profile =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eFormat = OMX_VIDEO_RVFormatUnused;

            return OMX_ErrorNone;
        }

		case OMX_IndexParamVideoFFmpeg:
        {
            OMX_VIDEO_PARAM_FFMPEGTYPE *profile =
                (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eCodecId = AV_CODEC_ID_NONE;
            profile->nWidth   = 0;
            profile->nHeight  = 0;

            return OMX_ErrorNone;
        }

        default:

            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftFFmpegVideo::isRoleSupported(
		        const OMX_PARAM_COMPONENTROLETYPE *roleParams) {
    bool supported = true;

    switch (mMode) {
    case MODE_MPEG2:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.mpeg2v", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_H263:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.h263", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_MPEG4:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.mpeg4", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_WMV:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.wmv", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_RV:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.rv", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_H264:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.avc", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_VPX:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.vpx", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_VC1:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.vc1", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_FLV1:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.flv1", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_DIVX:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.divx", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_HEVC:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.hevc", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    case MODE_TRIAL:
        if (strncmp((const char *)roleParams->cRole,
                "video_decoder.trial", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
            break;
    default:
        CHECK(!"Should not be here. Unsupported role.");
        break;
    }

    if (!supported) {
        ALOGE("unsupported role: %s", (const char *)roleParams->cRole);
        return OMX_ErrorUndefined;
    }
    return OMX_ErrorNone;
}

OMX_ERRORTYPE SoftFFmpegVideo::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    //ALOGV("internalSetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;
            return isRoleSupported(roleParams);
        }

        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *defParams =
                (OMX_PARAM_PORTDEFINITIONTYPE *)params;

            if (defParams->nPortIndex > kOutputPortIndex ||
                    defParams->nSize != sizeof(OMX_PARAM_PORTDEFINITIONTYPE)) {
                return OMX_ErrorUndefined;
            }

            CHECK_EQ((int)defParams->eDomain, (int)OMX_PortDomainVideo);

            //only care about input port
            if (defParams->nPortIndex == kOutputPortIndex) {
                OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &defParams->format.video;
                mCtx->width = video_def->nFrameWidth;
                mCtx->height = video_def->nFrameHeight;
                ALOGV("got OMX_IndexParamPortDefinition, width: %lu, height: %lu",
                        video_def->nFrameWidth, video_def->nFrameHeight);
                return OMX_ErrorNone;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *profile =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_VIDEO_WMVFormat7) {
                mCtx->codec_id = AV_CODEC_ID_WMV1;
            } else if (profile->eFormat == OMX_VIDEO_WMVFormat8) {
                mCtx->codec_id = AV_CODEC_ID_WMV2;
            } else if (profile->eFormat == OMX_VIDEO_WMVFormat9) {
                mCtx->codec_id = AV_CODEC_ID_WMV3;
            } else {
                ALOGE("unsupported wmv codec: 0x%x", profile->eFormat);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *profile =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            if (profile->eFormat == OMX_VIDEO_RVFormatG2) {
                mCtx->codec_id = AV_CODEC_ID_RV20;
            } else if (profile->eFormat == OMX_VIDEO_RVFormat8) {
                mCtx->codec_id = AV_CODEC_ID_RV30;
            } else if (profile->eFormat == OMX_VIDEO_RVFormat9) {
                mCtx->codec_id = AV_CODEC_ID_RV40;
            } else {
                ALOGE("unsupported rv codec: 0x%x", profile->eFormat);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoFFmpeg:
        {
            OMX_VIDEO_PARAM_FFMPEGTYPE *profile =
                (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = (enum AVCodecID)profile->eCodecId;
            mCtx->width    = profile->nWidth;
            mCtx->height   = profile->nHeight;

            ALOGD("got OMX_IndexParamVideoFFmpeg, "
                "eCodecId:%ld(%s), width:%lu, height:%lu",
                profile->eCodecId,
                avcodec_get_name(mCtx->codec_id),
                profile->nWidth,
                profile->nHeight);

            return OMX_ErrorNone;
        }

        default:

            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

bool SoftFFmpegVideo::handlePortSettingChangeEvent() {
    if (mCtx->width != mWidth || mCtx->height != mHeight) {
       ALOGI("ffmpeg video port setting change event(%dx%d)->(%dx%d).",
               mWidth, mHeight, mCtx->width, mCtx->height);

       mWidth = mCtx->width;
       mHeight = mCtx->height;
       mStride = mWidth;

       updatePortDefinitions();
       notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
       mOutputPortSettingsChange = AWAITING_DISABLED;
       return true;
    }

    return false;
}

int32_t SoftFFmpegVideo::handleExtradata() {
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = *inQueue.begin();
    OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

    ALOGI("got extradata, ignore: %d, size: %lu",
            mIgnoreExtradata, inHeader->nFilledLen);
    hexdump(inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);

    if (mIgnoreExtradata) {
        ALOGI("got extradata, size: %lu, but ignore it", inHeader->nFilledLen);
	} else {
        if (!mExtradataReady) {
            //if (mMode == MODE_H264)
            //it is possible to receive multiple input buffer with OMX_BUFFERFLAG_CODECCONFIG flag.
            //for example, H264, the first input buffer is SPS, and another is PPS!
            int orig_extradata_size = mCtx->extradata_size;
            mCtx->extradata_size += inHeader->nFilledLen;
            mCtx->extradata = (uint8_t *)realloc(mCtx->extradata,
                    mCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
            if (!mCtx->extradata) {
                ALOGE("ffmpeg video decoder failed to alloc extradata memory.");
                return ERR_OOM;
            }

            memcpy(mCtx->extradata + orig_extradata_size,
                    inHeader->pBuffer + inHeader->nOffset,
                    inHeader->nFilledLen);
            memset(mCtx->extradata + mCtx->extradata_size, 0,
                    FF_INPUT_BUFFER_PADDING_SIZE);
        }
    }

    inQueue.erase(inQueue.begin());
    inInfo->mOwnedByUs = false;
    notifyEmptyBufferDone(inHeader);

    return ERR_OK;
}

int32_t SoftFFmpegVideo::openDecoder() {
    if (mCodecAlreadyOpened) {
        return ERR_OK;
    }

    if (!mExtradataReady) {
        ALOGI("extradata is ready, size: %d", mCtx->extradata_size);
        hexdump(mCtx->extradata, mCtx->extradata_size);
        mExtradataReady = true;
    }

    //find decoder again as codec_id may have changed
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec) {
        ALOGE("ffmpeg video decoder failed to find codec");
        return ERR_CODEC_NOT_FOUND;
    }

    setDefaultCtx(mCtx, mCtx->codec);

    ALOGD("begin to open ffmpeg decoder(%s) now",
            avcodec_get_name(mCtx->codec_id));

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("ffmpeg video decoder failed to initialize. (%s)", av_err2str(err));
        return ERR_DECODER_OPEN_FAILED;
    }
	mCodecAlreadyOpened = true;

    ALOGD("open ffmpeg video decoder(%s) success",
            avcodec_get_name(mCtx->codec_id));

    mFrame = avcodec_alloc_frame();
    if (!mFrame) {
        ALOGE("oom for video frame");
        return ERR_OOM;
    }

    return ERR_OK;
}

void SoftFFmpegVideo::initPacket(AVPacket *pkt,
        OMX_BUFFERHEADERTYPE *inHeader) {
    memset(pkt, 0, sizeof(AVPacket));
    av_init_packet(pkt);

    if (inHeader) {
        pkt->data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt->size = inHeader->nFilledLen;
        pkt->pts = inHeader->nTimeStamp;
    } else {
        pkt->data = NULL;
        pkt->size = 0;
        pkt->pts = AV_NOPTS_VALUE;
    }

#if DEBUG_PKT
    if (pkt->pts != AV_NOPTS_VALUE)
    {
        ALOGV("pkt size:%d, pts:%lld", pkt->size, pkt->pts);
    } else {
        ALOGV("pkt size:%d, pts:N/A", pkt->size);
    }
#endif
}

int32_t SoftFFmpegVideo::decodeVideo() {
    int len = 0;
    int gotPic = false;
    int32_t ret = ERR_OK;
    bool is_flush = (mEOSStatus != INPUT_DATA_AVAILABLE);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    if (!is_flush) {
        inInfo = *inQueue.begin();
        CHECK(inInfo != NULL);
        inHeader = inInfo->mHeader;
    }

    AVPacket pkt;
    initPacket(&pkt, inHeader);
    //av_frame_unref(mFrame); //Don't unref mFrame!!!
    avcodec_get_frame_defaults(mFrame);

    int err = avcodec_decode_video2(mCtx, mFrame, &gotPic, &pkt);
    if (err < 0) {
        ALOGE("ffmpeg video decoder failed to decode frame. (%d)", err);
        //don't send error to OMXCodec, skip!
        ret = ERR_NO_FRM;
    } else {
        if (!gotPic) {
            ALOGI("ffmpeg video decoder failed to get frame.");
            //stop sending empty packets if the decoder is finished
            if (is_flush && mCtx->codec->capabilities & CODEC_CAP_DELAY) {
                ret = ERR_FLUSHED;
            } else {
                ret = ERR_NO_FRM;
            }
        } else {
            if (handlePortSettingChangeEvent()) {
                mPendingFrameAsSettingChanged = true;
            }
			ret = ERR_OK;
        }
    }

	if (!is_flush) {
        inQueue.erase(inQueue.begin());
        inInfo->mOwnedByUs = false;
        notifyEmptyBufferDone(inHeader);
	}

	return ret;
}

int32_t SoftFFmpegVideo::preProcessVideoFrame(AVPicture *picture, void **bufp) {
    AVPicture *picture2;
    AVPicture picture_tmp;
    uint8_t *buf = NULL;

    //deinterlace : must be done before any resize
    if (mDoDeinterlace) {
        int size = 0;

        //create temporary picture
        size = avpicture_get_size(mCtx->pix_fmt, mCtx->width, mCtx->height);
        buf  = (uint8_t *)av_malloc(size);
        if (!buf) {
            ALOGE("oom for temporary picture");
            return ERR_OOM;
        }

        picture2 = &picture_tmp;
        avpicture_fill(picture2, buf, mCtx->pix_fmt, mCtx->width, mCtx->height);

        if (avpicture_deinterlace(picture2, picture,
                mCtx->pix_fmt, mCtx->width, mCtx->height) < 0) {
            //if error, do not deinterlace
            ALOGE("Deinterlacing failed");
            av_free(buf);
            buf = NULL;
            picture2 = picture;
        }
    } else {
        picture2 = picture;
    }

    if (picture != picture2)
        *picture = *picture2;
    *bufp = buf;

    return ERR_OK;
}

int32_t SoftFFmpegVideo::drainOneOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
	OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    AVPicture pict;
    void *buffer_to_free = NULL;
    int64_t pts = AV_NOPTS_VALUE;
    uint8_t *dst = outHeader->pBuffer;

    //do deinterlace if necessary. for example, your TV is progressive
    int32_t err = preProcessVideoFrame((AVPicture *)mFrame, &buffer_to_free);
    if (err != ERR_OK) {
        ALOGE("preProcessVideoFrame failed");
        return err;
    }

    memset(&pict, 0, sizeof(AVPicture));
    pict.data[0] = dst;
    pict.data[1] = dst + mStride * mHeight;
    pict.data[2] = pict.data[1] + (mStride / 2  * mHeight / 2);
    pict.linesize[0] = mStride;
    pict.linesize[1] = mStride / 2;
    pict.linesize[2] = mStride / 2;

    int sws_flags = SWS_BICUBIC;
    mImgConvertCtx = sws_getCachedContext(mImgConvertCtx,
           mWidth, mHeight, mCtx->pix_fmt, mWidth, mHeight,
           PIX_FMT_YUV420P, sws_flags, NULL, NULL, NULL);
    if (mImgConvertCtx == NULL) {
        ALOGE("Cannot initialize the conversion context");
        av_free(buffer_to_free);
        return ERR_SWS_FAILED;
    }
    sws_scale(mImgConvertCtx, mFrame->data, mFrame->linesize,
            0, mHeight, pict.data, pict.linesize);

    outHeader->nOffset = 0;
    outHeader->nFilledLen = (mStride * mHeight * 3) / 2;
    outHeader->nFlags = 0;
    if (mFrame->key_frame) {
        outHeader->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
    }

    //process timestamps
    if (decoder_reorder_pts == -1) {
        pts = *(int64_t*)av_opt_ptr(avcodec_get_frame_class(),
                mFrame, "best_effort_timestamp");
    } else if (decoder_reorder_pts) {
        pts = mFrame->pkt_pts;
    } else {
        pts = mFrame->pkt_dts;
    }

    if (pts == AV_NOPTS_VALUE) {
        pts = 0;
    }
    outHeader->nTimeStamp = pts;

#if DEBUG_FRM
    ALOGV("mFrame pts: %lld", pts);
#endif

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    av_free(buffer_to_free);

    return ERR_OK;
}

void SoftFFmpegVideo::drainEOSOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
    BufferInfo *outInfo = *outQueue.begin();
    CHECK(outInfo != NULL);
    OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

    ALOGD("ffmpeg video decoder fill eos outbuf");

    outHeader->nTimeStamp = 0;
    outHeader->nFilledLen = 0;
    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    mEOSStatus = OUTPUT_FRAMES_FLUSHED;
}

void SoftFFmpegVideo::drainAllOutputBuffers() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
   if (!mCodecAlreadyOpened) {
        drainEOSOutputBuffer();
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
	   return;
   }

    if(!(mCtx->codec->capabilities & CODEC_CAP_DELAY)) {
        drainEOSOutputBuffer();
        mEOSStatus = OUTPUT_FRAMES_FLUSHED;
        return;
    }

    while (!outQueue.empty()) {
        if (!mPendingFrameAsSettingChanged) {
            int32_t err = decodeVideo();
		    if (err < ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            } else if (err == ERR_FLUSHED) {
                drainEOSOutputBuffer();
                return;
            } else {
                CHECK_EQ(err, ERR_OK);
                if (mPendingFrameAsSettingChanged) {
					return;
                }
            }
		}

        if (drainOneOutputBuffer() != ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
		}
		
        if (mPendingFrameAsSettingChanged) {
            mPendingFrameAsSettingChanged = false;
        }
    }
}

void SoftFFmpegVideo::onQueueFilled(OMX_U32 portIndex) {
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    while (((mEOSStatus != INPUT_DATA_AVAILABLE) || !inQueue.empty())
            && !outQueue.empty()) {
        if (mEOSStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        inInfo   = *inQueue.begin();
        inHeader = inInfo->mHeader;

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            ALOGD("ffmpeg video decoder empty eos inbuf");
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
            mEOSStatus = INPUT_EOS_SEEN;
			continue;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            if (handleExtradata() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
            }
            continue;
        }

        if (!mCodecAlreadyOpened) {
		    if (openDecoder() != ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
        }

        if (!mPendingFrameAsSettingChanged) {
            int32_t err = decodeVideo();
		    if (err < ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            } else if (err == ERR_NO_FRM) {
                continue;
            } else {
                CHECK_EQ(err, ERR_OK);
                if (mPendingFrameAsSettingChanged) {
					return;
                }
            }
		}

        if (drainOneOutputBuffer() != ERR_OK) {
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            return;
		}
		
        if (mPendingFrameAsSettingChanged) {
            mPendingFrameAsSettingChanged = false;
        }
    }
}

void SoftFFmpegVideo::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGV("ffmpeg video decoder flush port(%lu)", portIndex);
    if (portIndex == kInputPortIndex && mCtx) {
        if (mCtx) {
            //Make sure that the next buffer output does not still
            //depend on fragments from the last one decoded.
            avcodec_flush_buffers(mCtx);
        }
        mEOSStatus = INPUT_DATA_AVAILABLE;
    }
}

void SoftFFmpegVideo::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != kOutputPortIndex) {
        return;
    }

    switch (mOutputPortSettingsChange) {
        case NONE:
            break;

        case AWAITING_DISABLED:
        {
            CHECK(!enabled);
            mOutputPortSettingsChange = AWAITING_ENABLED;
            break;
        }

        default:
        {
            CHECK_EQ((int)mOutputPortSettingsChange, (int)AWAITING_ENABLED);
            CHECK(enabled);
            mOutputPortSettingsChange = NONE;
            break;
        }
    }
}

void SoftFFmpegVideo::updatePortDefinitions() {
    OMX_PARAM_PORTDEFINITIONTYPE *def = &editPortInfo(0)->mDef;
    def->format.video.nFrameWidth = mWidth;
    def->format.video.nFrameHeight = mHeight;
    def->format.video.nStride = def->format.video.nFrameWidth;
    def->format.video.nSliceHeight = def->format.video.nFrameHeight;
    def->nBufferSize =
            def->format.video.nFrameWidth * def->format.video.nFrameHeight;

    def = &editPortInfo(1)->mDef;
    def->format.video.nFrameWidth = mWidth;
    def->format.video.nFrameHeight = mHeight;
    def->format.video.nStride = def->format.video.nFrameWidth;
    def->format.video.nSliceHeight = def->format.video.nFrameHeight;
#if 0
    def->nBufferSize =
        (def->format.video.nFrameWidth
            * def->format.video.nFrameHeight * 3) / 2;
#else
    def->nBufferSize =
        (((def->format.video.nFrameWidth + 15) & -16)
            * ((def->format.video.nFrameHeight + 15) & -16) * 3) / 2;
#endif
}

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SoftFFmpegVideo(name, callbacks, appData, component);
}

