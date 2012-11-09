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
#define LOG_TAG "SoftFFmpegAudio"
#include <utils/Log.h>

#include "SoftFFmpegAudio.h"

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/MediaDefs.h>

#include "ffmpeg_utils/ffmpeg_utils.h"

#undef realloc
#include <stdlib.h>

#define DEBUG_PKT 0
#define DEBUG_FRM 0

namespace android {

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SoftFFmpegAudio::SoftFFmpegAudio(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mMode(MODE_MPEG),
      mCtx(NULL),
      mSwrCtx(NULL),
      mCodecOpened(false),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mFlushComplete(false),
      mSignalledError(false),
      mReceivedEOS(false),
      mFrame(NULL),
      mAnchorTimeUs(0),
      mNumFramesOutput(0),
      mInputBufferSize(0),
      mAudioBufferSize(0),
      mNumChannels(2),
      mSamplingRate(44100),
      mSamplingFmt(AV_SAMPLE_FMT_S16),
      mAudioConfigChanged(false),
      mOutputPortSettingsChange(NONE) {
    if (!strcmp(name, "OMX.ffmpeg.mp3.decoder")) {
        mMode = MODE_MPEG;
        mIgnoreExtradata = true;
    } else if (!strcmp(name, "OMX.ffmpeg.mp1.decoder")) {
        mMode = MODE_MPEGL1;
    } else if (!strcmp(name, "OMX.ffmpeg.mp2.decoder")) {
        mMode = MODE_MPEGL2;
    } else if (!strcmp(name, "OMX.ffmpeg.aac.decoder")) {
        mMode = MODE_AAC;
    } else {
        CHECK(!strcmp(name, "OMX.ffmpeg.ac3.decoder"));
        mMode = MODE_AC3;
    }

    LOGV("SoftFFmpegAudio component: %s", name);

    initPorts();
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftFFmpegAudio::~SoftFFmpegAudio() {
    av_freep(&mFrame);
    LOGV("~SoftFFmpegAudio");
    deInitDecoder();
    deInitFFmpeg();
}

void SoftFFmpegAudio::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = 8192;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    switch (mMode) {
    case MODE_MPEG:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_MPEG);
        def.format.audio.eEncoding = OMX_AUDIO_CodingMP3;
        break;
    case MODE_MPEGL1:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
        def.format.audio.eEncoding = OMX_AUDIO_CodingMP3;
        break;
    case MODE_MPEGL2:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
        def.format.audio.eEncoding = OMX_AUDIO_CodingMP3;
        break;
    case MODE_AAC:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_AAC);
        def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;
        break;
    case MODE_AC3:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_AC3);
        // TODO
        //def.format.audio.eEncoding = OMX_AUDIO_CodingAC3;
        def.format.audio.eEncoding = OMX_AUDIO_CodingAutoDetect; // right?? orz
        break;
    default:
        CHECK(!"Should not be here. Unsupported mime type and compression format");
        break;
    }

    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.nBufferSize = kOutputBufferSize;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 2;

    def.format.audio.cMIMEType = const_cast<char *>("audio/raw");
    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
    def.format.audio.eEncoding = OMX_AUDIO_CodingPCM;

    addPort(def);
}

static int lockmgr(void **mtx, enum AVLockOp op) {
   switch(op) {
      case AV_LOCK_CREATE:
          *mtx = (void *)SDL_CreateMutex();
          if(!*mtx)
              return 1;
          return 0;
      case AV_LOCK_OBTAIN:
          return !!SDL_LockMutex((SDL_mutex *)*mtx);
      case AV_LOCK_RELEASE:
          return !!SDL_UnlockMutex((SDL_mutex *)*mtx);
      case AV_LOCK_DESTROY:
          SDL_DestroyMutex((SDL_mutex *)*mtx);
          return 0;
   }
   return 1;
}

status_t SoftFFmpegAudio::initFFmpeg() {
    //nam_av_log_set_flags(AV_LOG_SKIP_REPEATED);
    //av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(nam_av_log_callback);

    /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    av_register_all();
    avformat_network_init();

    init_opts();

    if (av_lockmgr_register(lockmgr)) {
        LOGE("could not initialize lock manager!");
        return NO_INIT;
    }

    return OK;
}

void SoftFFmpegAudio::deInitFFmpeg() {
    av_lockmgr_register(NULL);
    uninit_opts();
    avformat_network_deinit();
}

void SoftFFmpegAudio::setAVCtxToDefault(AVCodecContext *avctx, const AVCodec *codec) {
    int fast = 0;

    avctx->workaround_bugs   = 1;
    avctx->lowres            = 0;
    if(avctx->lowres > codec->max_lowres){
        LOGW("The maximum value for lowres supported by the decoder is %d",
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

status_t SoftFFmpegAudio::initDecoder() {
    status_t status;

    status = initFFmpeg();
    if (status != OK)
        return status;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx)
    {
        LOGE("avcodec_alloc_context failed.");
        return OMX_ErrorInsufficientResources;
    }

    mCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    switch (mMode) {
    case MODE_MPEG:
        mCtx->codec_id = CODEC_ID_MP3;
        break;
    case MODE_MPEGL1:
        mCtx->codec_id = CODEC_ID_MP1;
        break;
    case MODE_MPEGL2:
        mCtx->codec_id = CODEC_ID_MP2;
        break;
    case MODE_AAC:
        mCtx->codec_id = CODEC_ID_AAC;
        break;
    case MODE_AC3:
        mCtx->codec_id = CODEC_ID_AC3;
        break;
    default:
        CHECK(!"Should not be here. Unsupported codec");
        break;
    }

    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec)
    {
        LOGE("find codec failed");
        return OMX_ErrorNotImplemented;
    }

    setAVCtxToDefault(mCtx, mCtx->codec);

#if 0
    // FIXME, defer to open? ref: OMXCodec.cpp:setAudioOutputFormat
    err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        LOGE("ffmpeg audio decoder failed to  initialize. (%d)", err);
        return OMX_ErrorUndefined;
    }
#endif

    mCtx->sample_fmt = AV_SAMPLE_FMT_S16;

    mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
    mAudioSrcFreq = mAudioTgtFreq = mSamplingRate;
    mAudioSrcChannels = mAudioTgtChannels = mNumChannels;
    mAudioSrcChannelLayout = mAudioTgtChannelLayout = 
        av_get_default_channel_layout(mNumChannels);

    memset(mSilenceBuffer, 0, kOutputBufferSize);

    return OMX_ErrorNone;
}

void SoftFFmpegAudio::deInitDecoder() {
    if (mCtx) {
        avcodec_flush_buffers(mCtx);
        if (!mCtx->extradata) {
            av_free(mCtx->extradata);
            mCtx->extradata = NULL;
            mCtx->extradata_size = 0;
        }
        avcodec_close(mCtx);
        av_free(mCtx);
        mCtx = NULL;
    }

    if (mSwrCtx) {
        swr_free(&mSwrCtx);
        mSwrCtx = NULL;
    }

}

OMX_ERRORTYPE SoftFFmpegAudio::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    int32_t channels = 0;
    int32_t sampling_rate = 0;

    switch (index) {
        case OMX_IndexParamAudioAac:
        {
            OMX_AUDIO_PARAM_AACPROFILETYPE *aacParams =
                (OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (aacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            aacParams->nBitRate = 0;
            aacParams->nAudioBandWidth = 0;
            aacParams->nAACtools = 0;
            aacParams->nAACERtools = 0;
            aacParams->eAACProfile = OMX_AUDIO_AACObjectMain;
            aacParams->eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;
            aacParams->eChannelMode = OMX_AUDIO_ChannelModeStereo;

            aacParams->nChannels = mNumChannels;
            aacParams->nSampleRate = mSamplingRate;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *pcmParams =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (pcmParams->nPortIndex > 1) {
                return OMX_ErrorUndefined;
            }

            pcmParams->eNumData = OMX_NumericalDataSigned;
            pcmParams->eEndian = OMX_EndianBig;
            pcmParams->bInterleaved = OMX_TRUE;
            pcmParams->nBitPerSample = 16;
            pcmParams->ePCMMode = OMX_AUDIO_PCMModeLinear;
            pcmParams->eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            pcmParams->eChannelMapping[1] = OMX_AUDIO_ChannelRF;

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update target channel and sampling rate etc
            mAudioTgtChannels =  channels;
            mAudioTgtFreq = sampling_rate;
            mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            pcmParams->nChannels = channels;
            pcmParams->nSamplingRate = sampling_rate;

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftFFmpegAudio::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;

            bool supported = true;
            switch (mMode) {
            case MODE_MPEG:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.mp3", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_MPEGL1:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.mp1", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_MPEGL2:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.mp2", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_AAC:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.aac", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_AC3:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.ac3", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            default:
                CHECK(!"Should not be here. Unsupported role.");
                break;
            }
            if (!supported) {
                LOGE("unsupported role: %s", (const char *)roleParams->cRole);
                return OMX_ErrorUndefined;
            }

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioAac:
        {
            const OMX_AUDIO_PARAM_AACPROFILETYPE *aacParams =
                (const OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (aacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            mNumChannels = aacParams->nChannels;
            mSamplingRate = aacParams->nSampleRate;

            LOGV("got OMX_IndexParamAudioAac, mNumChannels: %d, mSamplingRate: %d",
                mNumChannels, mSamplingRate);

            return OMX_ErrorNone;
        }
        default:
            LOGI("internalSetParameter, index: 0x%x", index);
            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

void SoftFFmpegAudio::onQueueFilled(OMX_U32 portIndex) {
    int len = 0;
    int err = 0;
    size_t dataSize = 0;
    int64_t decChannelLayout;
    int32_t inputBufferUsedLength = 0;
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(0);
    List<BufferInfo *> &outQueue = getPortQueue(1);

    while ((!inQueue.empty() || mInputBufferSize > 0 ||
            mAudioBufferSize > 0 || mFlushComplete) && !outQueue.empty()) {
        if (!inQueue.empty() || mInputBufferSize > 0) {
            inInfo = *inQueue.begin();
            inHeader = inInfo->mHeader;
        } else {
            inInfo = NULL;
            inHeader = NULL;
        }

        BufferInfo *outInfo = *outQueue.begin();
        OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

        if (inHeader && inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
            mReceivedEOS = true;
        }

        if (mReceivedEOS && (mFlushComplete || !(mCtx->codec->capabilities & CODEC_CAP_DELAY))) {
            outHeader->nFilledLen = 0;
            outHeader->nFlags = OMX_BUFFERFLAG_EOS;

            outQueue.erase(outQueue.begin());
            outInfo->mOwnedByUs = false;
            notifyFillBufferDone(outHeader);
            return;
        }

        if (inHeader && inHeader->nFlags & OMX_BUFFERFLAG_CODECCONFIG) {
            LOGI("got extradata, ignore: %d, size: %lu", mIgnoreExtradata, inHeader->nFilledLen);
            hexdump(inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);
            if (!mExtradataReady && !mIgnoreExtradata) {
                int orig_extradata_size = mCtx->extradata_size;
                mCtx->extradata_size += inHeader->nFilledLen;
                mCtx->extradata = (uint8_t *)realloc(mCtx->extradata,
                        mCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                if (!mCtx->extradata) {
                    LOGE("ffmpeg audio decoder failed to alloc extradata memory.");
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
                LOGI("got extradata, size: %lu, but ignore it", inHeader->nFilledLen);
                inInfo->mOwnedByUs = false;
                inQueue.erase(inQueue.begin());
                inInfo = NULL;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;

                continue;
            }
        }

        if (!mCodecOpened) {
            if (!mExtradataReady && !mIgnoreExtradata) {
                LOGI("extradata is ready");
                hexdump(mCtx->extradata, mCtx->extradata_size);
                mExtradataReady = true;
            }
            LOGI("open ffmpeg decoder now");

            err = avcodec_open2(mCtx, mCtx->codec, NULL);
            if (err < 0) {
                LOGE("ffmpeg audio decoder failed to initialize. (%d)", err);
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
            mCodecOpened = true;
        }

        /* update the audio clock with the pts */
        if (inHeader && inHeader->nOffset == 0) {
            mAnchorTimeUs = inHeader->nTimeStamp;
            mNumFramesOutput = 0;
            mInputBufferSize = inHeader->nFilledLen;
        }

        if (inHeader && mAudioBufferSize == 0 && !mFlushComplete) {
            AVPacket pkt;
            av_init_packet(&pkt);
            if (!mFlushComplete) {
                pkt.data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
                pkt.size = inHeader->nFilledLen;
                pkt.pts = inHeader->nTimeStamp; // ingore it, we will compute it
            } else {
                pkt.data = NULL;
                pkt.size = 0;
                pkt.pts = AV_NOPTS_VALUE;
            }
#if DEBUG_PKT
            LOGV("pkt size: %d, pts: %lld", pkt.size, pkt.pts);
#endif
            if (!mFrame) {
                if (!(mFrame = avcodec_alloc_frame())) {
                    LOGE("ffmpeg audio decoder failed to alloc memory.");
                    notify(OMX_EventError, OMX_ErrorInsufficientResources, 0, NULL);
                    mSignalledError = true;
                    return;
                }
            } else {
                avcodec_get_frame_defaults(mFrame);
            }

            int gotFrm = false;
            inputBufferUsedLength = 0;
            len = avcodec_decode_audio4(mCtx, mFrame, &gotFrm, &pkt);
	    if (len < 0) {
                LOGE("ffmpeg audio decoder failed to decode frame. (0x%x)", len);

                /* if !mAudioConfigChanged, Don't fill the out buffer */
                if (!mAudioConfigChanged) {
                    inInfo->mOwnedByUs = false;
                    inQueue.erase(inQueue.begin());
                    inInfo = NULL;
                    notifyEmptyBufferDone(inHeader);
                    inHeader = NULL;
                    continue;
                }

                inputBufferUsedLength = inHeader->nFilledLen;
                /* if error, we skip the frame and play silence instead */
                mPAudioBuffer = mSilenceBuffer;
                mAudioBufferSize = kOutputBufferSize;
            } else if (!gotFrm) {
                LOGI("ffmpeg audio decoder failed to get frame.");
                /* stop sending empty packets if the decoder is finished */
                if (!pkt.data && mCtx->codec->capabilities & CODEC_CAP_DELAY)
                    mFlushComplete = true;
                continue;
            } else {
                /**
                 * FIXME, check mAudioConfigChanged when the first time you call the audio4!
                 * mCtx->sample_rate and mCtx->channels may be changed by audio decoder later, why???
                 */
                if (!mAudioConfigChanged) {
                    if (mCtx->channels != mNumChannels || mCtx->sample_rate != mSamplingRate) {
                        LOGI("audio OMX_EventPortSettingsChanged, mCtx->channels: %d, mNumChannels: %d, mCtx->sample_rate: %d, mSamplingRate: %d",
                                mCtx->channels, mNumChannels, mCtx->sample_rate, mSamplingRate);
                        mNumChannels = mCtx->channels;
                        mSamplingRate = mCtx->sample_rate;
                        mSamplingFmt = mCtx->sample_fmt;
                        mAudioConfigChanged = true;
                        notify(OMX_EventPortSettingsChanged, 1, 0, NULL);
                        mOutputPortSettingsChange = AWAITING_DISABLED;
                        return;
                    } else {
                        // match with the default, set mAudioConfigChanged true anyway!
                        mAudioConfigChanged = true;
                    }
                }

                dataSize = av_samples_get_buffer_size(NULL, mNumChannels, mFrame->nb_samples, mSamplingFmt, 1);

                decChannelLayout = av_get_default_channel_layout(mNumChannels);
                if (mSamplingFmt != mAudioSrcFmt ||
                        decChannelLayout != mAudioSrcChannelLayout ||
                        mSamplingRate != mAudioSrcFreq ) {
                    if (mSwrCtx)
                        swr_free(&mSwrCtx);
                    mSwrCtx = swr_alloc_set_opts(NULL,
                                                 mAudioTgtChannelLayout, mAudioTgtFmt, mAudioTgtFreq,
                                                 decChannelLayout,       mSamplingFmt, mSamplingRate,
                                                 0, NULL);
                    if (!mSwrCtx || swr_init(mSwrCtx) < 0) {
                        LOGE("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                                mSamplingRate,
                                av_get_sample_fmt_name(mSamplingFmt),
                                mNumChannels,
                                mAudioTgtFreq,
                                av_get_sample_fmt_name(mAudioTgtFmt),
                                mAudioTgtChannels);
                        notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                        mSignalledError = true;
                        return;
                    }
                    mAudioSrcChannelLayout = decChannelLayout;
                    mAudioSrcChannels = mNumChannels;
                    mAudioSrcFreq = mSamplingRate;
                    mAudioSrcFmt = mSamplingFmt;
                }

                if (mSwrCtx) {
                    const uint8_t *in[] = { mFrame->data[0] };
                    uint8_t *out[] = {mAudioBuf2};
                    int len2 = swr_convert(mSwrCtx, out, sizeof(mAudioBuf2) / mAudioTgtChannels / av_get_bytes_per_sample(mAudioTgtFmt),
                                       in, mFrame->nb_samples);
                    if (len2 < 0) {
                        LOGE("audio_resample() failed\n");
                        break;
                    }
                    if (len2 == sizeof(mAudioBuf2) / mAudioTgtChannels / av_get_bytes_per_sample(mAudioTgtFmt)) {
                        LOGE("warning: audio buffer is probably too small");
                        swr_init(mSwrCtx);
                    }
                    mPAudioBuffer = mAudioBuf2;
                    mAudioBufferSize = len2 * mAudioTgtChannels * av_get_bytes_per_sample(mAudioTgtFmt);
                } else {
                    mPAudioBuffer = mFrame->data[0];
                    mAudioBufferSize = dataSize;
                }

                inputBufferUsedLength = len;
#if DEBUG_FRM
                LOGV("ffmpeg audio decoder get frame. (%d), mAudioBufferSize: %d", len, mAudioBufferSize);
#endif
            }
        }

        size_t copyToOutputBufferLen = mAudioBufferSize;
        if (mAudioBufferSize > kOutputBufferSize)
            copyToOutputBufferLen = kOutputBufferSize;

        outHeader->nOffset = 0;
        outHeader->nFilledLen = copyToOutputBufferLen;
        outHeader->nTimeStamp = mAnchorTimeUs
                + (mNumFramesOutput * 1000000ll) / mSamplingRate;
        memcpy(outHeader->pBuffer, mPAudioBuffer, copyToOutputBufferLen);
        outHeader->nFlags = 0;

        mPAudioBuffer += copyToOutputBufferLen;
        mAudioBufferSize -= copyToOutputBufferLen;
        mNumFramesOutput += copyToOutputBufferLen / av_get_bytes_per_sample(mCtx->sample_fmt) / mNumChannels;

        if (inHeader) {
            CHECK_GE(inHeader->nFilledLen, inputBufferUsedLength);
            inHeader->nOffset += inputBufferUsedLength;
            inHeader->nFilledLen -= inputBufferUsedLength;
            mInputBufferSize -= inputBufferUsedLength;
            if (inHeader->nFilledLen == 0) {
                inInfo->mOwnedByUs = false;
                inQueue.erase(inQueue.begin());
                inInfo = NULL;
                notifyEmptyBufferDone(inHeader);
                inHeader = NULL;

                mInputBufferSize = 0;
            }
        }

        outInfo->mOwnedByUs = false;
        outQueue.erase(outQueue.begin());
        outInfo = NULL;
        notifyFillBufferDone(outHeader);
        outHeader = NULL;
    }
}

void SoftFFmpegAudio::onPortFlushCompleted(OMX_U32 portIndex) {
    if (portIndex == 0 && mCtx) {
        // Make sure that the next buffer output does not still
        // depend on fragments from the last one decoded.
        avcodec_flush_buffers(mCtx);
    }

}

void SoftFFmpegAudio::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
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

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SoftFFmpegAudio(name, callbacks, appData, component);
}
