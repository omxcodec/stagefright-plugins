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

#include "utils/ffmpeg_utils.h"

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

void SoftFFmpegAudio::setMode(const char *name) {
    if (!strcmp(name, "OMX.ffmpeg.aac.decoder")) {
        mMode = MODE_AAC;
	} else if (!strcmp(name, "OMX.ffmpeg.mp3.decoder")) {
        mMode = MODE_MPEG;
        mIgnoreExtradata = true;
    } else if (!strcmp(name, "OMX.ffmpeg.vorbis.decoder")) {
        mMode = MODE_VORBIS;
    } else if (!strcmp(name, "OMX.ffmpeg.wma.decoder")) {
        mMode = MODE_WMA;
    } else if (!strcmp(name, "OMX.ffmpeg.ra.decoder")) {
        mMode = MODE_RA;
    } else if (!strcmp(name, "OMX.ffmpeg.flac.decoder")) {
        mMode = MODE_FLAC;
    } else if (!strcmp(name, "OMX.ffmpeg.mp2.decoder")) {
        mMode = MODE_MPEGL2;
    } else if (!strcmp(name, "OMX.ffmpeg.ac3.decoder")) {
        mMode = MODE_AC3;
    } else if (!strcmp(name, "OMX.ffmpeg.ape.decoder")) {
        mMode = MODE_APE;
    } else if (!strcmp(name, "OMX.ffmpeg.dts.decoder")) {
        mMode = MODE_DTS;
    } else if (!strcmp(name, "OMX.ffmpeg.atrial.decoder")) {
        mMode = MODE_TRIAL;
    } else {
        TRESPASS();
    }
}

SoftFFmpegAudio::SoftFFmpegAudio(
        const char *name,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mMode(MODE_NONE),
      mFFmpegAlreadyInited(false),
      mCodecAlreadyOpened(false),
      mExtradataReady(false),
      mIgnoreExtradata(false),
      mCtx(NULL),
      mSwrCtx(NULL),
      mFrame(NULL),
      mEOSStatus(INPUT_DATA_AVAILABLE),
      mSignalledError(false),
      mAudioClock(0),
      mInputBufferSize(0),
      mResampledData(NULL),
      mResampledDataSize(0),
      mOutputPortSettingsChange(NONE) {

    setMode(name);

    ALOGD("SoftFFmpegAudio component: %s mMode: %d", name, mMode);

    initPorts();
    CHECK_EQ(initDecoder(), (status_t)OK);
}

SoftFFmpegAudio::~SoftFFmpegAudio() {
    ALOGV("~SoftFFmpegAudio");
    deInitDecoder();
    if (mFFmpegAlreadyInited) {
        deInitFFmpeg();
    }
}

void SoftFFmpegAudio::initInputFormat(uint32_t mode,
        OMX_PARAM_PORTDEFINITIONTYPE &def) {
    switch (mode) {
    case MODE_AAC:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_AAC);
        def.format.audio.eEncoding = OMX_AUDIO_CodingAAC;
        break;
    case MODE_MPEG:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_MPEG);
        def.format.audio.eEncoding = OMX_AUDIO_CodingMP3;
        break;
    case MODE_VORBIS:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_VORBIS);
        def.format.audio.eEncoding = OMX_AUDIO_CodingVORBIS;
        break;
    case MODE_WMA:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_WMA);
        def.format.audio.eEncoding = OMX_AUDIO_CodingWMA;
        break;
    case MODE_RA:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_RA);
        def.format.audio.eEncoding = OMX_AUDIO_CodingRA;
        break;
    case MODE_FLAC:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_FLAC);
        def.format.audio.eEncoding = OMX_AUDIO_CodingFLAC;
        break;
    case MODE_MPEGL2:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
        def.format.audio.eEncoding = OMX_AUDIO_CodingMP3;
        break;
    case MODE_AC3:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_AC3);
        def.format.audio.eEncoding = OMX_AUDIO_CodingAC3;
        break;
    case MODE_APE:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_APE);
        def.format.audio.eEncoding = OMX_AUDIO_CodingAPE;
        break;
    case MODE_DTS:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_DTS);
        def.format.audio.eEncoding = OMX_AUDIO_CodingDTS;
        break;
    case MODE_TRIAL:
        def.format.audio.cMIMEType = const_cast<char *>(MEDIA_MIMETYPE_AUDIO_FFMPEG);
        def.format.audio.eEncoding = OMX_AUDIO_CodingAutoDetect;
        break;
    default:
        CHECK(!"Should not be here. Unsupported mime type and compression format");
        break;
    }

    def.format.audio.pNativeRender = NULL;
    def.format.audio.bFlagErrorConcealment = OMX_FALSE;
}

void SoftFFmpegAudio::initPorts() {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    InitOMXParams(&def);

    def.nPortIndex = 0;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = kNumInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    if (mMode == MODE_APE) {
        def.nBufferSize = 1000000; // ape!
    } else if (mMode == MODE_DTS) {
        def.nBufferSize = 1000000; // dts!
    } else {
        def.nBufferSize = 20480; // 8192 is too small
    }
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainAudio;
    def.bBuffersContiguous = OMX_FALSE;
    def.nBufferAlignment = 1;

    initInputFormat(mMode, def);

    addPort(def);

    def.nPortIndex = 1;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = kNumOutputBuffers;
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

void SoftFFmpegAudio::setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec) {
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

bool SoftFFmpegAudio::isConfigured() {
	return mAudioSrcChannels != -1;
}

void SoftFFmpegAudio::resetCtx() {
    mCtx->channels = -1;
    mCtx->sample_rate = -1;
    mCtx->bit_rate = -1;
    mCtx->sample_fmt = AV_SAMPLE_FMT_NONE;

    mAudioSrcChannels = mAudioTgtChannels = -1;
    mAudioSrcFreq = mAudioTgtFreq = -1;
    mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_NONE;
    mAudioSrcChannelLayout = mAudioTgtChannelLayout = 0;
}

status_t SoftFFmpegAudio::initDecoder() {
    status_t status;

    status = initFFmpeg();
    if (status != OK) {
        return NO_INIT;
    }
    mFFmpegAlreadyInited = true;

    mCtx = avcodec_alloc_context3(NULL);
    if (!mCtx) {
        ALOGE("avcodec_alloc_context failed.");
        return NO_MEMORY;
    }

    mCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    switch (mMode) {
    case MODE_AAC:
        mCtx->codec_id = AV_CODEC_ID_AAC;
        break;
    case MODE_MPEG:
        mCtx->codec_id = AV_CODEC_ID_MP3;
        break;
    case MODE_VORBIS:
        mCtx->codec_id = AV_CODEC_ID_VORBIS;
        break;
    case MODE_WMA:
        mCtx->codec_id = AV_CODEC_ID_WMAV2; //should be adjusted later
        break;
    case MODE_RA:
        mCtx->codec_id = AV_CODEC_ID_COOK;
        break;
    case MODE_FLAC:
        mCtx->codec_id = AV_CODEC_ID_FLAC;
        break;
    case MODE_MPEGL2:
        mCtx->codec_id = AV_CODEC_ID_MP2;
        break;
    case MODE_AC3:
        mCtx->codec_id = AV_CODEC_ID_AC3;
        break;
    case MODE_APE:
        mCtx->codec_id = AV_CODEC_ID_APE;
        break;
    case MODE_DTS:
        mCtx->codec_id = AV_CODEC_ID_DTS;
        break;
    case MODE_TRIAL:
        mCtx->codec_id = AV_CODEC_ID_NONE;
        break;
    default:
        CHECK(!"Should not be here. Unsupported codec");
        break;
    }

    //invalid ctx
    resetCtx();

    mCtx->extradata = NULL;
    mCtx->extradata_size = 0;

    memset(mSilenceBuffer, 0, kOutputBufferSize);

    return OK;
}

void SoftFFmpegAudio::deInitDecoder() {
    if (mCtx) {
        if (!mCtx->extradata) {
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
    if (mSwrCtx) {
        swr_free(&mSwrCtx);
        mSwrCtx = NULL;
    }
}

OMX_ERRORTYPE SoftFFmpegAudio::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR params) {
    //ALOGV("internalGetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *profile =
                (OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (profile->nPortIndex > kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            profile->eNumData = OMX_NumericalDataSigned;
            profile->eEndian = OMX_EndianBig;
            profile->bInterleaved = OMX_TRUE;
            profile->nBitPerSample = 16;
            profile->ePCMMode = OMX_AUDIO_PCMModeLinear;
            profile->eChannelMapping[0] = OMX_AUDIO_ChannelLF;
            profile->eChannelMapping[1] = OMX_AUDIO_ChannelRF;

            CHECK(isConfigured());

            profile->nChannels = mAudioSrcChannels;
            profile->nSamplingRate = mAudioSrcFreq;

            //mCtx has been updated(adjustAudioParams)!
            ALOGV("get pcm params, nChannels:%lu, nSamplingRate:%lu",
                   profile->nChannels, profile->nSamplingRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAac:
        {
            OMX_AUDIO_PARAM_AACPROFILETYPE *profile =
                (OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nBitRate = 0;
            profile->nAudioBandWidth = 0;
            profile->nAACtools = 0;
            profile->nAACERtools = 0;
            profile->eAACProfile = OMX_AUDIO_AACObjectMain;
            profile->eAACStreamFormat = OMX_AUDIO_AACStreamFormatMP4FF;
            profile->eChannelMode = OMX_AUDIO_ChannelModeStereo;

            profile->nChannels = 0;
            profile->nSampleRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp3:
        {
            OMX_AUDIO_PARAM_MP3TYPE *profile =
                (OMX_AUDIO_PARAM_MP3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSampleRate = 0;
            profile->nBitRate = 0;
            profile->nAudioBandWidth = 0;
            profile->eChannelMode = OMX_AUDIO_ChannelModeStereo;
            profile->eFormat = OMX_AUDIO_MP3StreamFormatMP1Layer3;

            return OMX_ErrorNone;
        }
        case OMX_IndexParamAudioVorbis:
        {
            OMX_AUDIO_PARAM_VORBISTYPE *profile =
                (OMX_AUDIO_PARAM_VORBISTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nBitRate = 0;
            profile->nMinBitRate = 0;
            profile->nMaxBitRate = 0;
            profile->nAudioBandWidth = 0;
            profile->nQuality = 3;
            profile->bManaged = OMX_FALSE;
            profile->bDownmix = OMX_FALSE;

            profile->nChannels = 0;
            profile->nSampleRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *profile =
                (OMX_AUDIO_PARAM_WMATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSamplingRate = 0;
            profile->nBitRate = 0;
            profile->eFormat = OMX_AUDIO_WMAFormatUnused;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioRa:
        {
            OMX_AUDIO_PARAM_RATYPE *profile =
                (OMX_AUDIO_PARAM_RATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSamplingRate = 0;
            profile->eFormat = OMX_AUDIO_RAFormatUnused;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFlac:
        {
            OMX_AUDIO_PARAM_FLACTYPE *profile =
                (OMX_AUDIO_PARAM_FLACTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSampleRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp2:
        {
            OMX_AUDIO_PARAM_MP2TYPE *profile =
                (OMX_AUDIO_PARAM_MP2TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSampleRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAc3:
        {
            OMX_AUDIO_PARAM_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSamplingRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioApe:
        {
            OMX_AUDIO_PARAM_APETYPE *profile =
                (OMX_AUDIO_PARAM_APETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSamplingRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioDts:
        {
            OMX_AUDIO_PARAM_DTSTYPE *profile =
                (OMX_AUDIO_PARAM_DTSTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->nChannels = 0;
            profile->nSamplingRate = 0;

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFFmpeg:
        {
            OMX_AUDIO_PARAM_FFMPEGTYPE *profile =
                (OMX_AUDIO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            profile->eCodecId = 0;
            profile->nChannels = 0;
            profile->nBitRate = 0;
            profile->nBitsPerSample = 0;
            profile->nSampleRate = 0;
            profile->nBlockAlign = 0;
            profile->eSampleFormat = 0;

            return OMX_ErrorNone;
        }

        default:

            return SimpleSoftOMXComponent::internalGetParameter(index, params);
    }
}

OMX_ERRORTYPE SoftFFmpegAudio::isRoleSupported(
        const OMX_PARAM_COMPONENTROLETYPE *roleParams) {
    bool supported = true;

    switch (mMode) {
    case MODE_AAC:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.aac", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_MPEG:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.mp3", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
        break;
    case MODE_VORBIS:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.vorbis", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_WMA:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.wma", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_RA:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.ra", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_FLAC:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.flac", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_MPEGL2:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.mp2", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
        break;
    case MODE_AC3:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.ac3", OMX_MAX_STRINGNAME_SIZE - 1))
            supported = false;
        break;
    case MODE_APE:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.ape", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_DTS:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.dts", OMX_MAX_STRINGNAME_SIZE - 1))
        supported = false;
        break;
    case MODE_TRIAL:
        if (strncmp((const char *)roleParams->cRole,
                "audio_decoder.trial", OMX_MAX_STRINGNAME_SIZE - 1))
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

void SoftFFmpegAudio::adjustAudioParams() {
    int32_t channels = 0;
    int32_t sampling_rate = 0;

    CHECK(!isConfigured());

    sampling_rate = mCtx->sample_rate;

    //channels support 1 or 2 only
    channels = mCtx->channels >= 2 ? 2 : 1;

    //4000 <= sampling rate <= 48000
    if (sampling_rate < 4000) {
        sampling_rate = 4000;
    } else if (sampling_rate > 48000) {
        sampling_rate = 48000;
    }

    mAudioSrcChannels = mAudioTgtChannels = channels;
    mAudioSrcFreq = mAudioTgtFreq = sampling_rate;
    mAudioSrcFmt = mAudioTgtFmt = AV_SAMPLE_FMT_S16;
    mAudioSrcChannelLayout = mAudioTgtChannelLayout =
        av_get_default_channel_layout(channels);
}

OMX_ERRORTYPE SoftFFmpegAudio::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR params) {
    //ALOGV("internalSetParameter index:0x%x", index);
    switch (index) {
        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)params;
			return isRoleSupported(roleParams);
        }

        case OMX_IndexParamAudioPcm:
        {
            const OMX_AUDIO_PARAM_PCMMODETYPE *profile =
                (const OMX_AUDIO_PARAM_PCMMODETYPE *)params;

            if (profile->nPortIndex != kOutputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;
            mCtx->bits_per_coded_sample = profile->nBitPerSample;

            ALOGV("set OMX_IndexParamAudioPcm, nChannels:%lu, "
                    "nSampleRate:%lu, nBitsPerSample:%lu",
                profile->nChannels, profile->nSamplingRate,
                profile->nBitPerSample);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAac:
        {
            const OMX_AUDIO_PARAM_AACPROFILETYPE *profile =
                (const OMX_AUDIO_PARAM_AACPROFILETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioAac, nChannels:%lu, nSampleRate:%lu",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp3:
        {
            const OMX_AUDIO_PARAM_MP3TYPE *profile =
                (const OMX_AUDIO_PARAM_MP3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioMp3, nChannels:%lu, nSampleRate:%lu",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioVorbis:
        {
            const OMX_AUDIO_PARAM_VORBISTYPE *profile =
                (const OMX_AUDIO_PARAM_VORBISTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGD("set OMX_IndexParamAudioVorbis, "
                    "nChannels=%lu, nSampleRate=%lu, nBitRate=%lu, "
                    "nMinBitRate=%lu, nMaxBitRate=%lu",
                profile->nChannels, profile->nSampleRate,
                profile->nBitRate, profile->nMinBitRate,
                profile->nMaxBitRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *profile =
                (OMX_AUDIO_PARAM_WMATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            if (profile->eFormat == OMX_AUDIO_WMAFormat7) {
               mCtx->codec_id = AV_CODEC_ID_WMAV2;
            } else if (profile->eFormat == OMX_AUDIO_WMAFormat8) {
               mCtx->codec_id = AV_CODEC_ID_WMAPRO;
            } else if (profile->eFormat == OMX_AUDIO_WMAFormat9) {
               mCtx->codec_id = AV_CODEC_ID_WMALOSSLESS;
            } else {
                ALOGE("unsupported wma codec: 0x%x", profile->eFormat);
                return OMX_ErrorUndefined;
            }

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            // wmadec needs bitrate, block_align
            mCtx->bit_rate = profile->nBitRate;
            mCtx->block_align = profile->nBlockAlign;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioWma, nChannels:%u, "
                    "nSampleRate:%lu, nBitRate:%lu, nBlockAlign:%u",
                profile->nChannels, profile->nSamplingRate,
                profile->nBitRate, profile->nBlockAlign);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioRa:
        {
            OMX_AUDIO_PARAM_RATYPE *profile =
                (OMX_AUDIO_PARAM_RATYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            // FIXME, HACK!!!, I use the nNumRegions parameter pass blockAlign!!!
            // the cook audio codec need blockAlign!
            mCtx->block_align = profile->nNumRegions;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioRa, nChannels:%lu, "
                    "nSampleRate:%lu, nBlockAlign:%d",
                profile->nChannels, profile->nSamplingRate, mCtx->block_align);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFlac:
        {
            OMX_AUDIO_PARAM_FLACTYPE *profile =
                (OMX_AUDIO_PARAM_FLACTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioFlac, nChannels:%lu, nSampleRate:%lu",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioMp2:
        {
            OMX_AUDIO_PARAM_MP2TYPE *profile =
                (OMX_AUDIO_PARAM_MP2TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSampleRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioMp2, nChannels:%lu, nSampleRate:%lu",
                profile->nChannels, profile->nSampleRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioAc3:
        {
            OMX_AUDIO_PARAM_AC3TYPE *profile =
                (OMX_AUDIO_PARAM_AC3TYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioFlac, nChannels:%lu, nSampleRate:%lu",
                profile->nChannels, profile->nSamplingRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioApe:
        {
            OMX_AUDIO_PARAM_APETYPE *profile =
                (OMX_AUDIO_PARAM_APETYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            //ape decoder need bits_per_coded_sample
            mCtx->bits_per_coded_sample = profile->nBitsPerSample;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioApe, nChannels:%lu, "
                    "nSampleRate:%lu, nBitsPerSample:%lu",
                profile->nChannels, profile->nSamplingRate,
                profile->nBitsPerSample);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioDts:
        {
            OMX_AUDIO_PARAM_DTSTYPE *profile =
                (OMX_AUDIO_PARAM_DTSTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());

            mCtx->channels = profile->nChannels;
            mCtx->sample_rate = profile->nSamplingRate;

            adjustAudioParams();

            ALOGV("set OMX_IndexParamAudioDts, nChannels:%lu, nSampleRate:%lu",
                profile->nChannels, profile->nSamplingRate);

            return OMX_ErrorNone;
        }

        case OMX_IndexParamAudioFFmpeg:
        {
            OMX_AUDIO_PARAM_FFMPEGTYPE *profile =
                (OMX_AUDIO_PARAM_FFMPEGTYPE *)params;

            if (profile->nPortIndex != kInputPortIndex) {
                return OMX_ErrorUndefined;
            }

            CHECK(!isConfigured());


            mCtx->codec_id = (enum AVCodecID)profile->eCodecId;
            mCtx->channels = profile->nChannels;
            mCtx->bit_rate = profile->nBitRate;
            mCtx->bits_per_coded_sample = profile->nBitsPerSample;
            mCtx->sample_rate = profile->nSampleRate;
            mCtx->block_align = profile->nBlockAlign;
            mCtx->sample_fmt = (AVSampleFormat)profile->eSampleFormat;

            adjustAudioParams();

            ALOGD("set OMX_IndexParamAudioFFmpeg, "
                "eCodecId:%ld(%s), nChannels:%lu, nBitRate:%lu, "
                "nBitsPerSample:%lu, nSampleRate:%lu, "
                "nBlockAlign:%lu, eSampleFormat:%lu(%s)",
                profile->eCodecId, avcodec_get_name(mCtx->codec_id),
                profile->nChannels, profile->nBitRate,
                profile->nBitsPerSample, profile->nSampleRate,
                profile->nBlockAlign, profile->eSampleFormat,
                av_get_sample_fmt_name(mCtx->sample_fmt));
            return OMX_ErrorNone;
        }

        default:

            return SimpleSoftOMXComponent::internalSetParameter(index, params);
    }
}

int32_t SoftFFmpegAudio::handleExtradata() {
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
            int orig_extradata_size = mCtx->extradata_size;
            mCtx->extradata_size += inHeader->nFilledLen;
            mCtx->extradata = (uint8_t *)av_realloc(mCtx->extradata,
                    mCtx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
            if (!mCtx->extradata) {
                ALOGE("ffmpeg audio decoder failed to alloc extradata memory.");
                return ERR_OOM;
            }

            memcpy(mCtx->extradata + orig_extradata_size,
                    inHeader->pBuffer + inHeader->nOffset, inHeader->nFilledLen);
            memset(mCtx->extradata + mCtx->extradata_size, 0, FF_INPUT_BUFFER_PADDING_SIZE);
        }
    }

    inInfo->mOwnedByUs = false;
    inQueue.erase(inQueue.begin());
    inInfo = NULL;
    notifyEmptyBufferDone(inHeader);
    inHeader = NULL;

    return ERR_OK;
}

int32_t SoftFFmpegAudio::openDecoder() {
    if (mCodecAlreadyOpened) {
        return ERR_OK;
    }

    if (!mExtradataReady && !mIgnoreExtradata) {
        ALOGI("extradata is ready, size: %d", mCtx->extradata_size);
        hexdump(mCtx->extradata, mCtx->extradata_size);
        mExtradataReady = true;
    }

    //find decoder
    mCtx->codec = avcodec_find_decoder(mCtx->codec_id);
    if (!mCtx->codec) {
        ALOGE("ffmpeg audio decoder failed to find codec");
        return ERR_CODEC_NOT_FOUND;
    }

    CHECK(isConfigured());

    setDefaultCtx(mCtx, mCtx->codec);

    ALOGD("begin to open ffmpeg audio decoder(%s), mCtx sample_rate: %d, "
           "channels: %d, , sample_fmt: %s",
           avcodec_get_name(mCtx->codec_id),
           mCtx->sample_rate, mCtx->channels,
           av_get_sample_fmt_name(mCtx->sample_fmt));

    int err = avcodec_open2(mCtx, mCtx->codec, NULL);
    if (err < 0) {
        ALOGE("ffmpeg audio decoder failed to initialize.(%s)", av_err2str(err));
        return ERR_DECODER_OPEN_FAILED;
    }
    mCodecAlreadyOpened = true;

    ALOGD("open ffmpeg audio decoder(%s) success, mCtx sample_rate: %d, "
            "channels: %d, sample_fmt: %s",
            avcodec_get_name(mCtx->codec_id),
            mCtx->sample_rate, mCtx->channels,
            av_get_sample_fmt_name(mCtx->sample_fmt));

    mFrame = avcodec_alloc_frame();
    if (!mFrame) {
        ALOGE("oom for video frame");
        return ERR_OOM;
    }

	return ERR_OK;
}

void SoftFFmpegAudio::updateTimeStamp(OMX_BUFFERHEADERTYPE *inHeader) {
    CHECK_EQ(mInputBufferSize, 0);

    //XXX reset to AV_NOPTS_VALUE if the pts is invalid
    if (inHeader->nTimeStamp == SF_NOPTS_VALUE) {
        inHeader->nTimeStamp = AV_NOPTS_VALUE;
    }

    //update the audio clock if the pts is valid
    if (inHeader->nTimeStamp != AV_NOPTS_VALUE) {
        mAudioClock = inHeader->nTimeStamp;
    }
}

void SoftFFmpegAudio::initPacket(AVPacket *pkt,
        OMX_BUFFERHEADERTYPE *inHeader) {
    memset(pkt, 0, sizeof(AVPacket));
    av_init_packet(pkt);

    if (inHeader) {
        pkt->data = (uint8_t *)inHeader->pBuffer + inHeader->nOffset;
        pkt->size = inHeader->nFilledLen;
        pkt->pts = inHeader->nTimeStamp; //ingore it, we will compute it
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

int32_t SoftFFmpegAudio::decodeAudio() {
    int len = 0;
    int gotFrm = false;
	int32_t ret = ERR_OK;
    int32_t inputBufferUsedLength = 0;
	bool is_flush = (mEOSStatus != INPUT_DATA_AVAILABLE);
    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    CHECK_EQ(mResampledDataSize, 0);

    if (!is_flush) {
        inInfo = *inQueue.begin();
        CHECK(inInfo != NULL);
        inHeader = inInfo->mHeader;

		if (mInputBufferSize == 0) {
		    updateTimeStamp(inHeader);
            mInputBufferSize = inHeader->nFilledLen;
        }
    }

    AVPacket pkt;
    initPacket(&pkt, inHeader);
    av_frame_unref(mFrame);
    avcodec_get_frame_defaults(mFrame);

    len = avcodec_decode_audio4(mCtx, mFrame, &gotFrm, &pkt);
    //a negative error code is returned if an error occurred during decoding
    if (len < 0) {
        ALOGW("ffmpeg audio decoder err, we skip the frame and play silence instead");
        mResampledData = mSilenceBuffer;
        mResampledDataSize = kOutputBufferSize;
        ret = ERR_OK;
    } else {
#if DEBUG_PKT
        ALOGV("ffmpeg audio decoder, consume pkt len: %d", len);
#endif
        if (!gotFrm) {
#if DEBUG_FRM
            ALOGI("ffmpeg audio decoder failed to get frame.");
#endif
            //stop sending empty packets if the decoder is finished
            if (is_flush && mCtx->codec->capabilities & CODEC_CAP_DELAY) {
                ALOGI("ffmpeg audio decoder failed to get more frames when flush.");
			    ret = ERR_FLUSHED;
		    } else {
		        ret = ERR_NO_FRM;
		    }
        } else {
            ret = resampleAudio();
		}
    }

	if (!is_flush) {
        if (len < 0) {
            //if error, we skip the frame 
            inputBufferUsedLength = mInputBufferSize;
        } else {
            inputBufferUsedLength = len;
        }

        CHECK_GE(inHeader->nFilledLen, inputBufferUsedLength);
        inHeader->nOffset += inputBufferUsedLength;
        inHeader->nFilledLen -= inputBufferUsedLength;
        mInputBufferSize -= inputBufferUsedLength;

        if (inHeader->nFilledLen == 0) {
            CHECK_EQ(mInputBufferSize, 0);
            inQueue.erase(inQueue.begin());
            inInfo->mOwnedByUs = false;
            notifyEmptyBufferDone(inHeader);
        }
	}

    return ret;
}

int32_t SoftFFmpegAudio::resampleAudio() {
	int channels = 0;
    int64_t channelLayout = 0;
    size_t dataSize = 0;

    dataSize = av_samples_get_buffer_size(NULL, av_frame_get_channels(mFrame),
            mFrame->nb_samples, (enum AVSampleFormat)mFrame->format, 1);

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder, nb_samples:%d, get buffer size:%d",
            mFrame->nb_samples, dataSize);
#endif

	channels = av_get_channel_layout_nb_channels(mFrame->channel_layout);
    channelLayout =
        (mFrame->channel_layout && av_frame_get_channels(mFrame) == channels) ?
        mFrame->channel_layout : av_get_default_channel_layout(av_frame_get_channels(mFrame));

    if (mFrame->format != mAudioSrcFmt
            || channelLayout != mAudioSrcChannelLayout
            || mFrame->sample_rate != mAudioSrcFreq) {
        if (mSwrCtx) {
            swr_free(&mSwrCtx);
        }
        mSwrCtx = swr_alloc_set_opts(NULL,
                mAudioTgtChannelLayout, mAudioTgtFmt,                     mAudioTgtFreq,
                channelLayout,       (enum AVSampleFormat)mFrame->format, mFrame->sample_rate,
                0, NULL);
        if (!mSwrCtx || swr_init(mSwrCtx) < 0) {
            ALOGE("Cannot create sample rate converter for conversion "
                    "of %d Hz %s %d channels to %d Hz %s %d channels!",
                    mFrame->sample_rate,
                    av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                    av_frame_get_channels(mFrame),
                    mAudioTgtFreq,
                    av_get_sample_fmt_name(mAudioTgtFmt),
                    mAudioTgtChannels);
            return ERR_SWR_INIT_FAILED;
        }

        char src_layout_name[1024] = {0};
        char tgt_layout_name[1024] = {0};
        av_get_channel_layout_string(src_layout_name, sizeof(src_layout_name),
                mCtx->channels, channelLayout);
        av_get_channel_layout_string(tgt_layout_name, sizeof(tgt_layout_name),
                mAudioTgtChannels, mAudioTgtChannelLayout);
        ALOGI("Create sample rate converter for conversion "
                "of %d Hz %s %d channels(%s) "
                "to %d Hz %s %d channels(%s)!",
                mFrame->sample_rate,
                av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                av_frame_get_channels(mFrame),
                src_layout_name,
                mAudioTgtFreq,
                av_get_sample_fmt_name(mAudioTgtFmt),
                mAudioTgtChannels,
                tgt_layout_name);

        mAudioSrcChannelLayout = channelLayout;
        mAudioSrcChannels = av_frame_get_channels(mFrame);
        mAudioSrcFreq = mFrame->sample_rate;
        mAudioSrcFmt = (enum AVSampleFormat)mFrame->format;
    }

    if (mSwrCtx) {
        const uint8_t **in = (const uint8_t **)mFrame->extended_data;
        uint8_t *out[] = {mAudioBuffer};
        int out_count = sizeof(mAudioBuffer) / mAudioTgtChannels / av_get_bytes_per_sample(mAudioTgtFmt);
        int out_size  = av_samples_get_buffer_size(NULL, mAudioTgtChannels, out_count, mAudioTgtFmt, 0);
        int len2 = 0;
        if (out_size < 0) {
            ALOGE("av_samples_get_buffer_size() failed");
            return ERR_INVALID_PARAM;
        }

        len2 = swr_convert(mSwrCtx, out, out_count, in, mFrame->nb_samples);
        if (len2 < 0) {
            ALOGE("audio_resample() failed");
            return ERR_RESAMPLE_FAILED;
        }
        if (len2 == out_count) {
            ALOGE("warning: audio buffer is probably too small");
            swr_init(mSwrCtx);
        }
        mResampledData = mAudioBuffer;
        mResampledDataSize = len2 * mAudioTgtChannels * av_get_bytes_per_sample(mAudioTgtFmt);

#if DEBUG_FRM
        ALOGV("ffmpeg audio decoder(resample), mFrame->nb_samples:%d, len2:%d, mResampledDataSize:%d, "
                "src channel:%u, src fmt:%s, tgt channel:%u, tgt fmt:%s",
                mFrame->nb_samples, len2, mResampledDataSize,
                av_frame_get_channels(mFrame),
                av_get_sample_fmt_name((enum AVSampleFormat)mFrame->format),
                mAudioTgtChannels,
                av_get_sample_fmt_name(mAudioTgtFmt));
#endif
    } else {
        mResampledData = mFrame->data[0];
        mResampledDataSize = dataSize;

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder(no resample),"
            "nb_samples(before resample):%d, mResampledDataSize:%d",
            mFrame->nb_samples, mResampledDataSize);
#endif
    }

	return ERR_OK;
}

void SoftFFmpegAudio::drainOneOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
	BufferInfo *outInfo = *outQueue.begin();
	CHECK(outInfo != NULL);
	OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

	CHECK_GT(mResampledDataSize, 0);

    size_t copy = mResampledDataSize;
    if (mResampledDataSize > kOutputBufferSize) {
        copy = kOutputBufferSize;
	}

    outHeader->nOffset = 0;
    outHeader->nFilledLen = copy;
    outHeader->nTimeStamp = mAudioClock; 
    memcpy(outHeader->pBuffer, mResampledData, copy);
    outHeader->nFlags = 0;

    //update mResampledSize
    mResampledData += copy;
    mResampledDataSize -= copy;

    //update audio pts
    size_t frames = copy / (av_get_bytes_per_sample(mAudioTgtFmt) * mAudioTgtChannels);
    mAudioClock += (frames * 1000000ll) / mAudioTgtFreq;

#if DEBUG_FRM
    ALOGV("ffmpeg audio decoder, fill out buffer, copy:%u, pts: %lld",
            copy, outHeader->nTimeStamp);
#endif

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);
}

void SoftFFmpegAudio::drainEOSOutputBuffer() {
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);
	BufferInfo *outInfo = *outQueue.begin();
	CHECK(outInfo != NULL);
	OMX_BUFFERHEADERTYPE *outHeader = outInfo->mHeader;

	CHECK_EQ(mResampledDataSize, 0);

    ALOGD("ffmpeg audio decoder fill eos outbuf");

    outHeader->nTimeStamp = 0;
    outHeader->nFilledLen = 0;
    outHeader->nFlags = OMX_BUFFERFLAG_EOS;

    outQueue.erase(outQueue.begin());
    outInfo->mOwnedByUs = false;
    notifyFillBufferDone(outHeader);

    mEOSStatus = OUTPUT_FRAMES_FLUSHED;
}

void SoftFFmpegAudio::drainAllOutputBuffers() {
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
        if (mResampledDataSize == 0) {
            int32_t err = decodeAudio();
            if (err < ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
			    return;
            } else if (err == ERR_FLUSHED) {
                drainEOSOutputBuffer();
                return;
			} else {
                CHECK_EQ(err, ERR_OK);
			}
        }

		if (mResampledDataSize > 0) {
            drainOneOutputBuffer();
        }
    }
}

void SoftFFmpegAudio::onQueueFilled(OMX_U32 portIndex) {
    BufferInfo *inInfo = NULL;
    OMX_BUFFERHEADERTYPE *inHeader = NULL;

    if (mSignalledError || mOutputPortSettingsChange != NONE) {
        return;
    }

    if (mEOSStatus == OUTPUT_FRAMES_FLUSHED) {
        return;
    }

    List<BufferInfo *> &inQueue = getPortQueue(kInputPortIndex);
    List<BufferInfo *> &outQueue = getPortQueue(kOutputPortIndex);

    while (((mEOSStatus != INPUT_DATA_AVAILABLE) || !inQueue.empty())
            && !outQueue.empty()) {

        if (mEOSStatus == INPUT_EOS_SEEN) {
            drainAllOutputBuffers();
            return;
        }

        inInfo   = *inQueue.begin();
        inHeader = inInfo->mHeader;

        if (inHeader->nFlags & OMX_BUFFERFLAG_EOS) {
            ALOGD("ffmpeg audio decoder empty eos inbuf");
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
                return;
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

		if (mResampledDataSize == 0) {
			int32_t err = decodeAudio();
            if (err < ERR_OK) {
                notify(OMX_EventError, OMX_ErrorUndefined, 0, NULL);
                mSignalledError = true;
			    return;
            } else if (err == ERR_NO_FRM) {
                CHECK_EQ(mResampledDataSize, 0);
                continue;
			} else {
                CHECK_EQ(err, ERR_OK);
			}
		}

		if (mResampledDataSize > 0) {
			drainOneOutputBuffer();
		}
    }
}

void SoftFFmpegAudio::onPortFlushCompleted(OMX_U32 portIndex) {
    ALOGV("ffmpeg audio decoder flush port(%lu)", portIndex);
    if (portIndex == kInputPortIndex) {
        if (mCtx) {
            //Make sure that the next buffer output does not still
            //depend on fragments from the last one decoded.
            avcodec_flush_buffers(mCtx);
        }

	    mAudioClock = 0;
	    mInputBufferSize = 0;
	    mResampledDataSize = 0;
	    mResampledData = NULL;
        mEOSStatus = INPUT_DATA_AVAILABLE;
    }
}

void SoftFFmpegAudio::onPortEnableCompleted(OMX_U32 portIndex, bool enabled) {
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

}  // namespace android

android::SoftOMXComponent *createSoftOMXComponent(
        const char *name, const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData, OMX_COMPONENTTYPE **component) {
    return new android::SoftFFmpegAudio(name, callbacks, appData, component);
}
