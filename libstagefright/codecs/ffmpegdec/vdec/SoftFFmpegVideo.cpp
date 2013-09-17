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

#include <OMX_FFExt.h>

#include "utils/ffmpeg_utils.h"


#undef realloc
#include <stdlib.h>

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

SoftFFmpegVideo::SoftFFmpegVideo(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mMode(MODE_H264),
      mFFmpegInited(false),
      mCtx(NULL),
      mImgConvertCtx(NULL),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mSignalledError(false),
      mDoDeinterlace(true),
      mWidth(320),
      mHeight(240),
      mStride(320),
      mOutputPortSettingsChange(NONE) {
    if (!strcmp(name, "OMX.ffmpeg.h264.decoder")) {
        mMode = MODE_H264;
	} else if (!strcmp(name, "OMX.ffmpeg.mpeg4.decoder")) {
        mMode = MODE_MPEG4;
    } else if (!strcmp(name, "OMX.ffmpeg.mpeg2v.decoder")) {
        mMode = MODE_MPEG2;
    } else if (!strcmp(name, "OMX.ffmpeg.h263.decoder")) {
        mMode = MODE_H263;
    } else if (!strcmp(name, "OMX.ffmpeg.vpx.decoder")) {
        mMode = MODE_VPX;
    } else if (!strcmp(name, "OMX.ffmpeg.vc1.decoder")) {
        mMode = MODE_VC1;
    } else if (!strcmp(name, "OMX.ffmpeg.divx.decoder")) {
        mMode = MODE_DIVX;
    } else if (!strcmp(name, "OMX.ffmpeg.wmv.decoder")) {
        mMode = MODE_WMV;
    } else if (!strcmp(name, "OMX.ffmpeg.flv.decoder")) {
        mMode = MODE_FLV;
    } else if (!strcmp(name, "OMX.ffmpeg.rv.decoder")) {
        mMode = MODE_RV;
    } else if (!strcmp(name, "OMX.ffmpeg.vheuristic.decoder")) {
        mMode = MODE_HEURISTIC;
    } else {
        TRESPASS();
    }

    ALOGD("SoftFFmpegVideo component: %s mMode: %d", name, mMode);

    initPorts();
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftFFmpegVideo::~SoftFFmpegVideo() {
    ALOGV("~SoftFFmpegVideo");
    deInitDecoder();
    if (mFFmpegInited) {
        deInitFFmpeg();
    }
}

void SoftFFmpegVideo::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 1280 * 720; // 256 * 1024?
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    switch (mMode) {
    case MODE_H264:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_AVC);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingAVC;
        break;
    case MODE_MPEG4:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_MPEG4);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG4;
        break;
    case MODE_MPEG2:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_MPEG2);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingMPEG2;
        break;
    case MODE_H263:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_H263);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingH263;
        break;
    case MODE_VPX:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_VPX);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingVPX;
        break;
    case MODE_VC1:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_VC1);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
        break;
    case MODE_WMV:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_WMV);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingWMV;
        break;
    case MODE_FLV:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_FLV1);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingAutoDetect; // no flv omx codec
        break;
    case MODE_RV:
        def.format.video.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_VIDEO_RV);
        def.format.video.eCompressionFormat = OMX_VIDEO_CodingRV;
        break;
    case MODE_HEURISTIC:
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

void SoftFFmpegVideo::setAVCtxToDefault(AVCodecContext *avctx, const AVCodec *codec) {
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
    if (status != OK)
        return NO_INIT;

    mFFmpegInited = true;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx)
    {
        ALOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
    }

    mCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    switch (mMode) {
    case MODE_H264:
        mCtx->codec_id = CODEC_ID_H264;
        break;
    case MODE_MPEG4:
        mCtx->codec_id = CODEC_ID_MPEG4;
        break;
    case MODE_MPEG2:
        mCtx->codec_id = CODEC_ID_MPEG2VIDEO;
        break;
    case MODE_H263:
        mCtx->codec_id = CODEC_ID_H263;
        // TODO, which?
        //mCtx->codec_id = CODEC_ID_H263P;
        //mCtx->codec_id = CODEC_ID_H263I;
        break;
    case MODE_VPX:
        mCtx->codec_id = CODEC_ID_VP8;
        break;
    case MODE_VC1:
        mCtx->codec_id = CODEC_ID_VC1;
        break;
    case MODE_WMV:
        mCtx->codec_id = CODEC_ID_WMV2;	// default, adjust in "internalSetParameter" fxn
        break;
    case MODE_RV:
        mCtx->codec_id = CODEC_ID_RV40;	// default, adjust in "internalSetParameter" fxn
        break;
    case MODE_FLV:
        mCtx->codec_id = CODEC_ID_FLV1;
        break;
    case MODE_HEURISTIC:
        mCtx->codec_id = CODEC_ID_NONE;
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
        avcodec_close(mCtx);
        av_free(mCtx);
        mCtx = NULL;
    }
    if (mImgConvertCtx) {
        sws_freeContext(mImgConvertCtx);
        mImgConvertCtx = NULL;
    }
}

void SoftFFmpegVideo::preProcessVideoFrame(AVPicture *picture, void **bufp)
{
    AVPicture *picture2;
    AVPicture picture_tmp;
    uint8_t *buf = NULL;

    /* deinterlace : must be done before any resize */
    if (mDoDeinterlace) {
        int size;

        /* create temporary picture */
        size = avpicture_get_size(mCtx->pix_fmt, mCtx->width, mCtx->height);
        buf  = (uint8_t *)av_malloc(size);
        if (!buf)
            return;

        picture2 = &picture_tmp;
        avpicture_fill(picture2, buf, mCtx->pix_fmt, mCtx->width, mCtx->height);

        if (avpicture_deinterlace(picture2, picture,
                mCtx->pix_fmt, mCtx->width, mCtx->height) < 0) {
            /* if error, do not deinterlace */
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
}

OMX_ERRORTYPE SoftFFmpegVideo::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > 1) {
                return OMX_ErrorUndefined;
            }

            if (formatParams->nIndex != 0) {
                return OMX_ErrorNoMore;
            }

            if (formatParams->nPortIndex == 0) {
                switch (mMode) {
                case MODE_H264:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingAVC;
                    break;
                case MODE_MPEG4:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingMPEG4;
                    break;
                case MODE_MPEG2:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingMPEG2;
                    break;
                case MODE_H263:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingH263;
                    break;
                case MODE_VPX:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingVPX;
                    break;
                case MODE_VC1:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingWMV;
                    break;
                case MODE_WMV:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingWMV;
                    break;
                case MODE_RV:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingRV;
                    break;
                case MODE_FLV:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
                    break;
                case MODE_HEURISTIC:
                    formatParams->eCompressionFormat = OMX_VIDEO_CodingAutoDetect;
                    break;
                default:
                    CHECK(!"Should not be here. Unsupported compression format.");
                    break;
                }
                formatParams->eColorFormat = OMX_COLOR_FormatUnused;
                formatParams->xFramerate = 0;
            } else {
                CHECK_EQ(formatParams->nPortIndex, 1u);

                formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
                formatParams->eColorFormat = OMX_COLOR_FormatYUV420Planar;
                formatParams->xFramerate = 0;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *wmvParams =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (wmvParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            wmvParams->eFormat = OMX_VIDEO_WMVFormatUnused;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *rvParams =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (rvParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            rvParams->eFormat = OMX_VIDEO_RVFormatUnused;

            return OMX_ErrorNone;
        }

        default:
            if ((OMX_FF_INDEXTYPE)index == OMX_IndexParamVideoFFmpeg)
            {
                OMX_VIDEO_PARAM_FFMPEGTYPE *ffmpegParams =
                    (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

                if (ffmpegParams->nPortIndex != 0) {
                    return OMX_ErrorUndefined;
                }

                ffmpegParams->eCodecId = CODEC_ID_NONE;
                ffmpegParams->nWidth   = 0;
                ffmpegParams->nHeight  = 0;

                return OMX_ErrorNone;
            }

            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftFFmpegVideo::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;

            bool supported = true;
            switch (mMode) {
            case MODE_H264:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.avc", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_MPEG4:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.mpeg4", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_MPEG2:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.mpeg2v", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_H263:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.h263", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_VPX:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.vpx", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_VC1:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.vc1", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_WMV:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.wmv", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_RV:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.rv", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_HEURISTIC:
                if (strncmp((const char *)roleParams->cRole,
                        "video_decoder.heuristic", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
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

        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)params;

            if (formatParams->nPortIndex > 1) {
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

            if (defParams->nPortIndex > 1 ||
                    defParams->nSize != sizeof(OMX_PARAM_PORTDEFINITIONTYPE)) {
                return OMX_ErrorUndefined;
            }

            CHECK_EQ((int)defParams->eDomain, (int)OMX_PortDomainVideo);

            // only care about input port
            if (defParams->nPortIndex == kPortIndexOutput) {
                OMX_VIDEO_PORTDEFINITIONTYPE *video_def = &defParams->format.video;
                mWidth = video_def->nFrameWidth;
                mHeight = video_def->nFrameHeight;
                ALOGV("got OMX_IndexParamPortDefinition, mWidth: %d, mHeight: %d",
                        mWidth, mHeight);
                return OMX_ErrorNone;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoWmv:
        {
            OMX_VIDEO_PARAM_WMVTYPE *wmvParams =
                (OMX_VIDEO_PARAM_WMVTYPE *)params;

            if (wmvParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            if (wmvParams->eFormat == OMX_VIDEO_WMVFormat7) {
                mCtx->codec_id = CODEC_ID_WMV1;
            } else if (wmvParams->eFormat == OMX_VIDEO_WMVFormat8) {
                mCtx->codec_id = CODEC_ID_WMV2;
            } else if (wmvParams->eFormat == OMX_VIDEO_WMVFormat9) {
                mCtx->codec_id = CODEC_ID_WMV3;
            } else {
                ALOGE("unsupported wmv codec: 0x%x", wmvParams->eFormat);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamVideoRv:
        {
            OMX_VIDEO_PARAM_RVTYPE *rvParams =
                (OMX_VIDEO_PARAM_RVTYPE *)params;

            if (rvParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            if (rvParams->eFormat == OMX_VIDEO_RVFormatG2) {
                mCtx->codec_id = CODEC_ID_RV20;
            } else if (rvParams->eFormat == OMX_VIDEO_RVFormat8) {
                mCtx->codec_id = CODEC_ID_RV30;
            } else if (rvParams->eFormat == OMX_VIDEO_RVFormat9) {
                mCtx->codec_id = CODEC_ID_RV40;
            } else {
                ALOGE("unsupported rv codec: 0x%x", rvParams->eFormat);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }

        default:
            if ((OMX_FF_INDEXTYPE)index == OMX_IndexParamVideoFFmpeg)
            {
                OMX_VIDEO_PARAM_FFMPEGTYPE *ffmpegParams =
                    (OMX_VIDEO_PARAM_FFMPEGTYPE *)params;

                if (ffmpegParams->nPortIndex != 0) {
                    return OMX_ErrorUndefined;
                }

                mCtx->codec_id = (enum AVCodecID)ffmpegParams->eCodecId;
                mCtx->width    = ffmpegParams->nWidth;
                mCtx->height   = ffmpegParams->nHeight;

                ALOGD("got OMX_IndexParamVideoFFmpeg, "
                    "eCodecId:%ld(%s), nWidth:%lu, nHeight:%lu",
                    ffmpegParams->eCodecId,
                    avcodec_get_name(mCtx->codec_id),
                    ffmpegParams->nWidth,
                    ffmpegParams->nHeight); 

                return OMX_ErrorNone;
            }

            ALOGI("internalSetParameter, index: 0x%x", index);
            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

void SoftFFmpegVideo::onQueueFilled(OMX_U32 portIndex) {
    int err = 0;

    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);

    while (!inQueue.empty() && !outQueue.empty()) {
        BufferInfo *inInfo = *inQueue.begin();
        OMX_BUFFERHEADERTYPE *inHeader = inInfo->mHeader;

        BufferInfo *outInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

        if (mCtx->width != mWidth || mCtx->height != mHeight) {
            mCtx->width = mWidth;
            mCtx->height = mHeight;
            mStride = mWidth;

            updatePortDefinitions();

            notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
            mOutputPortSettingsChange = AWAITING_DISABLED;
            return;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            ALOGD("ffmpeg video decoder empty eos inbuf");
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);

            ALOGD("ffmpeg video decoder fill eos outbuf");
            outHeader->nFilledLen = 0;
            outHeader->nFlags = OMX_BUFFERFLAG_EOS;

            outQueue.erase(outQueue.begin());
            outInfo->mOwnedByUs = false;
            notifyFillBufferDone(outHeader);
            return;
        }

        if (inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            ALOGI("got extradata, ignore: %d, size: %lu", mIgnoreExtradata, inHeader->nFilledLen);
            hexdump(inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);
            if (!mExtradataReady && !mIgnoreExtradata) {
                //if (mMode == MODE_H264)
                // it is possible to receive multiple input buffer with OMX_BUFFERFLAG_CODECCONFIG flag.
                // for example, H264, the first input buffer is SPS, and another is PPS!
                int orig_extradata_size = mCtx->extradata_size;
                mCtx->extradata_size += inHeader->nFilledLen;
                mCtx->extradata = (uint8_t *)realloc(mCtx->extradata,
                        mCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                if (!mCtx->extradata) {
                    ALOGE("ffmpeg video decoder failed to alloc extradata memory.");
                    notify(OMX_EventError, OMX_ErrorInsufficientResources, 0, NULL);
                    mSignalledError = true;
                    return;
                }

                memcpy(mCtx->extradata + orig_extradata_size,
                        inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);
                memset(mCtx->extradata + mCtx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);

                inInfo->mOwnedByUs = false;
                inQueue.erase(inQueue.begin());
                inInfo = NULL;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;

                continue;
            }

            if (mIgnoreExtradata) {
                ALOGI("got extradata, size: %lu, but ignore it", inHeader->nFilledLen);
                inInfo->mOwnedByUs = false;
                inQueue.erase(inQueue.begin());
                inInfo = NULL;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;

                continue;
            }
        }

        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt.size = inHeader->nFilledLen;
        pkt.pts = inHeader->nTimeStamp;
#if DEBUG_PKT
        ALOGV("pkt size: %d, pts: %lld", pkt.size, pkt.pts);
#endif
        if (!mExtradataReady) {
            ALOGI("extradata is ready, size: %d", mCtx->extradata_size);
            hexdump(mCtx->extradata, mCtx->extradata_size);
            mExtradataReady = true;

            // find decoder again as codec_id may have changed
            mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
            if (!mCtx->codec) {
                ALOGE("ffmpeg video decoder failed to find codec");
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }

            setAVCtxToDefault(mCtx, mCtx->codec);

            ALOGI("open ffmpeg decoder now");
            err = avcodec_open2(mCtx, mCtx->codec, NULL);
            if (err < 0) {
                ALOGE("ffmpeg video decoder failed to initialize. (%d)", err);
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
        }

        int gotPic = false;
        AVFrame *frame = avcodec_alloc_frame();
        err = avcodec_decode_video2(mCtx, frame, &gotPic, &pkt);
        if (err < 0) {
            ALOGE("ffmpeg video decoder failed to decode frame. (%d)", err);
#if 0
            // Don't send error to OMXCodec, skip!
            notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
            mSignalledError = true;
            av_free(frame);
            return;
#endif
        }

        if (gotPic) {
            AVPicture pict;
            void *buffer_to_free = NULL;
            int64_t pts = AV_NOPTS_VALUE;
            uint8_t *dst = outHeader->pBuffer;

            // do deinterlace if necessary. for example, your TV is progressive
            preProcessVideoFrame((AVPicture *)frame, &buffer_to_free);

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
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                av_free(frame);
                return;
            }
            sws_scale(mImgConvertCtx, frame->data, frame->linesize,
                  0, mHeight, pict.data, pict.linesize);

            outHeader->nOffset = 0;
            outHeader->nFilledLen = (mStride * mHeight * 3) / 2;
            outHeader->nFlags = 0;
            if (frame->key_frame)
                outHeader->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;

            // process timestamps
            if (decoder_reorder_pts == -1) {
                pts = *(int64_t*)av_opt_ptr(avcodec_get_frame_class(),
                        frame, "best_effort_timestamp");
            } else if (decoder_reorder_pts) {
                pts = frame->pkt_pts;
            } else {
                pts = frame->pkt_dts;
            }

            if (pts == AV_NOPTS_VALUE) {
                pts = 0;
            }
            outHeader->nTimeStamp = pts;

#if DEBUG_FRM
            ALOGV("frame pts: %lld", pts);
#endif

            outInfo->mOwnedByUs = false;
            outQueue.erase(outQueue.begin());
            outInfo = NULL;
            notifyFillBufferDone(outHeader);
            outHeader = NULL;

            av_free(buffer_to_free);
        }

        inInfo->mOwnedByUs = false;
        inQueue.erase(inQueue.begin());
        inInfo = NULL;
        notifyEmptyBufferDone(inHeader);
        inHeader = NULL;
        av_free(frame);
    }
}

void SoftFFmpegVideo::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == 0 && mCtx) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
    }
}

void SoftFFmpegVideo::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
    if (portIndex != 1) {
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

