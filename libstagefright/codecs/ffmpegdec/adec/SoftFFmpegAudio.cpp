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

#include "utils/common_utils.h"
#include "utils/ffmpeg_utils.h"

//#undef realloc
//#include <stdlib.h>

#define DEBUG_PKT 0
#define DEBUG_FRM 0

/**
 * Note: DECLARE_ALIGNED should move from "*.h" to here, otherwise
 * "Fatal signal 7 (SIGBUS)"!!! SIGBUS is because of an alignment exception
 */
DECLARE_ALIGNED(16, uint8_t, mAudioBuf2)[AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];

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
      mBitRate(0),
      mBlockAlign(0),
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
    } else if (!strcmp(name, "OMX.ffmpeg.wma.decoder")) {
        mMode = MODE_WMA;
    } else if (!strcmp(name, "OMX.ffmpeg.ra.decoder")) {
        mMode = MODE_RA;
    } else if (!strcmp(name, "OMX.ffmpeg.ac3.decoder")) {
        mMode = MODE_AC3;
    } else if (!strcmp(name, "OMX.ffmpeg.ape.decoder")) {
        mMode = MODE_APE;
    } else if (!strcmp(name, "OMX.ffmpeg.dts.decoder")) {
        mMode = MODE_DTS;
    } else if (!strcmp(name, "OMX.ffmpeg.flac.decoder")) {
        mMode = MODE_FLAC;
    } else {
        TRESPASS();
    }

    LOGV("SoftFFmpegAudio component: %s", name);
    LOGV("SoftFFmpegAudio mMode: %d", mMode);

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
    if (mMode == MODE_APE) {
        def.nBufferSize = 204800; // ape!
    } else if (mMode == MODE_DTS) {
        def.nBufferSize = 512000; // dts!
    } else {
        def.nBufferSize = 20480; // 8192 is too small
    }
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
    case MODE_WMA:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_WMA);
        def.format.audio.eEncoding = OMX_AUDIO_CodingWMA;
        break;
    case MODE_RA:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_RA);
        def.format.audio.eEncoding = OMX_AUDIO_CodingRA;
        break;
    case MODE_APE:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_APE);
        def.format.audio.eEncoding = OMX_AUDIO_CodingAPE;
        break;
    case MODE_DTS:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS);
        def.format.audio.eEncoding = OMX_AUDIO_CodingDTS;
        break;
    case MODE_FLAC:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_FLAC);
        def.format.audio.eEncoding = OMX_AUDIO_CodingFLAC;
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
        return NO_INIT;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx)
    {
        LOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
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
    case MODE_WMA:
        mCtx->codec_id = CODEC_ID_WMAV2; // FIXME, CODEC_ID_WMAV1 or CODEC_ID_WMAV2?
        break;
    case MODE_RA:
        mCtx->codec_id = CODEC_ID_COOK; // FIXME
        break;
    case MODE_APE:
        mCtx->codec_id = CODEC_ID_APE;
    case MODE_DTS:
        mCtx->codec_id = CODEC_ID_DTS;
    case MODE_FLAC:
        mCtx->codec_id = CODEC_ID_FLAC;
        break;
    default:
        CHECK(!"Should not be here. Unsupported codec");
        break;
    }

    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec)
    {
        LOGE("find codec failed");
        return BAD_TYPE;
    }

    setAVCtxToDefault(mCtx, mCtx->codec);

    mCtx->channels = mNumChannels;
    mCtx->sample_rate = mSamplingRate;
    mCtx->bit_rate = mBitRate;
    mCtx->sample_fmt = mSamplingFmt;

    mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
    mAudioSrcFreq = mAudioTgtFreq = mSamplingRate;
    mAudioSrcChannels = mAudioTgtChannels = mNumChannels;
    mAudioSrcChannelLayout = mAudioTgtChannelLayout = 
        av_get_default_channel_layout(mNumChannels);

    memset(mSilenceBuffer, 0, kOutputBufferSize);

    return OK;
}

void SoftFFmpegAudio::deInitDecoder() {
    if (mCtx) {
        //avcodec_flush_buffers(mCtx); // is it necessary? crash sometimes if call it
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
        case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *wmaParams =
                (OMX_AUDIO_PARAM_WMATYPE *)params;

            if (wmaParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            wmaParams->nChannels = 0;
            wmaParams->nSamplingRate = 0;
            wmaParams->nBitRate = 0;
            wmaParams->eFormat = OMX_AUDIO_WMAFormatUnused;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioRa:
        {
            OMX_AUDIO_PARAM_RATYPE *raParams =
                (OMX_AUDIO_PARAM_RATYPE *)params;

            if (raParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            raParams->nChannels = 0;
            raParams->nSamplingRate = 0;
            raParams->eFormat = OMX_AUDIO_RAFormatUnused;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioApe:
        {
            OMX_AUDIO_PARAM_APETYPE *apeParams =
                (OMX_AUDIO_PARAM_APETYPE *)params;

            if (apeParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            apeParams->nChannels = 0;
            apeParams->nSamplingRate = 0;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioDts:
        {
            OMX_AUDIO_PARAM_DTSTYPE *dtsParams =
                (OMX_AUDIO_PARAM_DTSTYPE *)params;

            if (dtsParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            dtsParams->nChannels = 0;
            dtsParams->nSamplingRate = 0;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioFlac:
        {
            OMX_AUDIO_PARAM_FLACTYPE *flacParams =
                (OMX_AUDIO_PARAM_FLACTYPE *)params;

            if (flacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            flacParams->nChannels = 0;
            flacParams->nSamplingRate = 0;

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

            LOGV("audio config change");

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target(except aac), only once!
            mAudioSrcChannels = mAudioTgtChannels =  channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

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
    int32_t channels = 0;
    int32_t sampling_rate = 0;

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
            case MODE_WMA:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.wma", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_RA:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.ra", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_APE:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.ape", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_DTS:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.dts", OMX_MAX_STRINGNAME_SIZE - 1))
                    supported =  false;
                break;
            case MODE_FLAC:
                if (strncmp((const char *)roleParams->cRole,
                        "audio_decoder.flac", OMX_MAX_STRINGNAME_SIZE - 1))
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

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target(only aac), only once!
            mAudioSrcChannels = mAudioTgtChannels = channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            LOGV("got OMX_IndexParamAudioAac, mNumChannels: %d, mSamplingRate: %d",
                mNumChannels, mSamplingRate);

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *wmaParams =
                (OMX_AUDIO_PARAM_WMATYPE *)params;

            if (wmaParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            if (wmaParams->eFormat == OMX_AUDIO_WMAFormat7) {
               mCtx->codec_id = CODEC_ID_WMAV2;
            } else if (wmaParams->eFormat == OMX_AUDIO_WMAFormat8) {
               mCtx->codec_id = CODEC_ID_WMAPRO;
            } else if (wmaParams->eFormat == OMX_AUDIO_WMAFormat9) {
               mCtx->codec_id = CODEC_ID_WMALOSSLESS;
            } else {
                LOGE("unsupported wma codec: 0x%x", wmaParams->eFormat);
                return OMX_ErrorUndefined;
            }

            mNumChannels = wmaParams->nChannels;
            mSamplingRate = wmaParams->nSamplingRate;
            mBitRate = wmaParams->nBitRate;

            // wma need bitrate
            mCtx->bit_rate = mBitRate;

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target(only wma), only once!
            mAudioSrcChannels = mAudioTgtChannels = channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            LOGV("got OMX_IndexParamAudioWma, mNumChannels: %d, mSamplingRate: %d, mBitRate: %d",
                mNumChannels, mSamplingRate, mBitRate);

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioRa:
        {
            OMX_AUDIO_PARAM_RATYPE *raParams =
                (OMX_AUDIO_PARAM_RATYPE *)params;

            if (raParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = CODEC_ID_COOK;

            mNumChannels = raParams->nChannels;
            mSamplingRate = raParams->nSamplingRate;
            // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
            // the cook audio codec need blockAlign!
            mBlockAlign = raParams->nNumRegions;

            // cook decoder need block_align
            mCtx->block_align = mBlockAlign;

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target(only wma), only once!
            mAudioSrcChannels = mAudioTgtChannels = channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            LOGV("got OMX_IndexParamAudioRa, mNumChannels: %d, mSamplingRate: %d, mBlockAlign: %d",
                mNumChannels, mSamplingRate, mBlockAlign);

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioApe:
        {
            OMX_AUDIO_PARAM_APETYPE *apeParams =
                (OMX_AUDIO_PARAM_APETYPE *)params;

            if (apeParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = CODEC_ID_APE;

            mNumChannels = apeParams->nChannels;
            mSamplingRate = apeParams->nSamplingRate;

            // ape decoder need bits_per_coded_sample
            mCtx->bits_per_coded_sample = apeParams->nBitsPerSample;

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target, only once!
            mAudioSrcChannels = mAudioTgtChannels = channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            LOGV("got OMX_IndexParamAudioApe, mNumChannels: %d, mSamplingRate: %d, nBitsPerSample: %d",
                mNumChannels, mSamplingRate, apeParams->nBitsPerSample);

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioDts:
        {
            OMX_AUDIO_PARAM_DTSTYPE *dtsParams =
                (OMX_AUDIO_PARAM_DTSTYPE *)params;

            if (dtsParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = CODEC_ID_DTS;

            mNumChannels = dtsParams->nChannels;
            mSamplingRate = dtsParams->nSamplingRate;

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target, only once!
            mAudioSrcChannels = mAudioTgtChannels = channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            LOGV("got OMX_IndexParamAudioDts, mNumChannels: %d, mSamplingRate: %d",
                mNumChannels, mSamplingRate);

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioFlac:
        {
            OMX_AUDIO_PARAM_FLACTYPE *flacParams =
                (OMX_AUDIO_PARAM_FLACTYPE *)params;

            if (flacParams->nPortIndex != 0) {
                return OMX_ErrorUndefined;
            }

            mCtx->codec_id = CODEC_ID_FLAC;

            mNumChannels = flacParams->nChannels;
            mSamplingRate = flacParams->nSamplingRate;

            channels = mNumChannels >= 2 ? 2 : 1;
            sampling_rate = mSamplingRate;
            // 4000 <= nSamplingRate <= 48000
            if (mSamplingRate < 4000) {
                sampling_rate = 4000;
            } else if (mSamplingRate > 48000) {
                sampling_rate = 48000;
            }

            // update src and target, only once!
            mAudioSrcChannels = mAudioTgtChannels = channels;
            mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
            mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
            mAudioSrcChannelLayout = mAudioTgtChannelLayout = av_get_default_channel_layout(channels);

            LOGV("got OMX_IndexParamAudioFlac, mNumChannels: %d, mSamplingRate: %d",
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
                mCtx->extradata = (uint8_t *)av_realloc(mCtx->extradata,
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

            // find decoder again as codec_id may have changed
            mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
            if (!mCtx->codec) {
                LOGE("ffmpeg audio decoder failed to find codec");
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }

            setAVCtxToDefault(mCtx, mCtx->codec);

            LOGI("open ffmpeg decoder now");
            err = avcodec_open2(mCtx, mCtx->codec, NULL);
            if (err < 0) {
                print_error("avcodec_open2", err);
                LOGE("ffmpeg audio decoder failed to initialize. (%d)", err);
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
                return;
            }
            mCodecOpened = true;
            LOGI("open ffmpeg audio decoder, mCtx sample_rate: %d, channels: %d, channel_layout: %llu, sample_fmt: %s",
                mCtx->sample_rate, mCtx->channels, mCtx->channel_layout, av_get_sample_fmt_name(mCtx->sample_fmt));
        }

        /* update the audio clock with the pts */
        if (inHeader && inHeader->nOffset == 0) {
            mAnchorTimeUs = inHeader->nTimeStamp;
            mInputBufferSize = inHeader->nFilledLen;
        }

        if (inHeader && mAudioBufferSize == 0 && !mFlushComplete) {
            AVPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
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
                LOGE("ffmpeg audio decoder failed to decode frame. consume pkt len: %d", len);

                /* if !mAudioConfigChanged, Don't fill the out buffer */
                if (!mAudioConfigChanged) {
                    inInfo->mOwnedByUs = false;
                    inQueue.erase(inQueue.begin());
                    inInfo = NULL;
                    notifyEmptyBufferDone(inHeader);
                    inHeader = NULL;

                    mInputBufferSize = 0; // need?
                    continue;
                }

                //inputBufferUsedLength = inHeader->nFilledLen;
                /* if error, we skip the frame and play silence instead */
                mPAudioBuffer = mSilenceBuffer;
                mAudioBufferSize = kOutputBufferSize;
            }

#if DEBUG_PKT
            LOGV("ffmpeg audio decoder, consume pkt len: %d", len);
#endif

            if (len < 0)
                inputBufferUsedLength = inHeader->nFilledLen;
            else
                inputBufferUsedLength = len;

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

            if (!gotFrm) {
                LOGI("ffmpeg audio decoder failed to get frame.");
                /* stop sending empty packets if the decoder is finished */
                if (!pkt.data && mCtx->codec->capabilities & CODEC_CAP_DELAY)
                    mFlushComplete = true;
                continue;
            }

            /**
             * FIXME, check mAudioConfigChanged when the first time you call the audio4!
             * mCtx->sample_rate and mCtx->channels may be changed by audio decoder later, why???
             */
            if (!mAudioConfigChanged) {
                //if (mCtx->channels != mNumChannels || mCtx->sample_rate != mSamplingRate || mCtx->sample_fmt != mSamplingFmt) {
                if (mCtx->channels != mNumChannels || mCtx->sample_rate != mSamplingRate) {
                    LOGI("audio OMX_EventPortSettingsChanged, mCtx->channels: %d, mNumChannels: %d, mCtx->sample_rate: %d, mSamplingRate: %d, mCtx->sample_fmt: %s, mSamplingFmt: %s",
                            mCtx->channels, mNumChannels, mCtx->sample_rate, mSamplingRate,
                            av_get_sample_fmt_name(mCtx->sample_fmt), av_get_sample_fmt_name(mSamplingFmt));
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
                    mSamplingFmt = mCtx->sample_fmt;
                }
            }

            dataSize = av_samples_get_buffer_size(NULL, mNumChannels, mFrame->nb_samples, mSamplingFmt, 1);

#if DEBUG_FRM
            LOGV("audio decoder, nb_samples: %d, get buffer size: %d", mFrame->nb_samples, dataSize);
            LOGV("audio decoder: mCtx channel_layout: %llu, channels: %d, channels_from_layout: %d\n",
                mCtx->channel_layout,  mCtx->channels, av_get_channel_layout_nb_channels(mCtx->channel_layout));
#endif

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
                    LOGE("Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!",
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

                LOGI("Create sample rate converter for conversion of %d Hz %s %d channels(%lld channel_layout) to %d Hz %s %d channels(%lld channel_layout)!",
                        mSamplingRate,
                        av_get_sample_fmt_name(mSamplingFmt),
                        mNumChannels,
                        mAudioTgtChannelLayout,
                        mAudioTgtFreq,
                        av_get_sample_fmt_name(mAudioTgtFmt),
                        mAudioTgtChannels,
                        decChannelLayout);

                mAudioSrcChannelLayout = decChannelLayout;
                mAudioSrcChannels = mNumChannels;
                mAudioSrcFreq = mSamplingRate;
                mAudioSrcFmt = mSamplingFmt;
            }

            if (mSwrCtx) {
                //const uint8_t *in[] = { mFrame->data[0] };
                const uint8_t **in = (const uint8_t **)mFrame->extended_data;
                uint8_t *out[] = {mAudioBuf2};
                int out_count = sizeof(mAudioBuf2) / mAudioTgtChannels / av_get_bytes_per_sample(mAudioTgtFmt);
#if DEBUG_FRM
                LOGV("swr_convert 1, out_count: %d, mFrame->nb_samples: %d, src frm: %s, tgt frm: %s",
                    out_count, mFrame->nb_samples, av_get_sample_fmt_name(mCtx->sample_fmt), av_get_sample_fmt_name(mAudioTgtFmt));
#endif
                int len2 = swr_convert(mSwrCtx, out, out_count, in, mFrame->nb_samples);
                if (len2 < 0) {
                    LOGE("audio_resample() failed");
                    break;
                }
                if (len2 == out_count) {
                    LOGE("warning: audio buffer is probably too small");
                    swr_init(mSwrCtx);
                }
                mPAudioBuffer = mAudioBuf2;
                mAudioBufferSize = len2 * mAudioTgtChannels * av_get_bytes_per_sample(mAudioTgtFmt);
            } else {
                mPAudioBuffer = mFrame->data[0];
                mAudioBufferSize = dataSize;
            }

#if DEBUG_FRM
            LOGV("ffmpeg audio decoder get frame. consume pkt len: %d, nb_samples(before resample): %d, mAudioBufferSize: %d",
                    len, mFrame->nb_samples, mAudioBufferSize);
#endif
        } // if (inHeader && mAudioBufferSize == 0 && !mFlushComplete)

        size_t copyToOutputBufferLen = mAudioBufferSize;
        if (mAudioBufferSize > kOutputBufferSize)
            copyToOutputBufferLen = kOutputBufferSize;

        outHeader->nOffset = 0;
        outHeader->nFilledLen = copyToOutputBufferLen;
        outHeader->nTimeStamp = mAnchorTimeUs + (mNumFramesOutput * 1000000ll) / mSamplingRate;
        memcpy(outHeader->pBuffer, mPAudioBuffer, copyToOutputBufferLen);
        outHeader->nFlags = 0;

#if DEBUG_FRM
        LOGV("ffmpeg audio decoder, fill out buffer, pts: %lld, mNumFramesOutput: %lld", outHeader->nTimeStamp, mNumFramesOutput);
#endif

        mPAudioBuffer += copyToOutputBufferLen;
        mAudioBufferSize -= copyToOutputBufferLen;
        mNumFramesOutput += copyToOutputBufferLen / (av_get_bytes_per_sample(mAudioTgtFmt) * mNumChannels);

        // reset mNumFramesOutput
        if (mAudioBufferSize == 0) {
#if DEBUG_FRM
            LOGV("~~~~ mAudioBufferSize, set mNumFramesOutput = 0");
#endif
            mNumFramesOutput = 0;
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
