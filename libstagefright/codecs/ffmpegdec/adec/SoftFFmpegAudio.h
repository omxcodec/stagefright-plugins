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

#ifndef SOFT_FFMPEGAUDIO_H_

#define SOFT_FFMPEGAUDIO_H_

#include "SimpleSoftOMXComponent.h"

#ifndef __GNUC__
//fix DECLARE_ALIGNED
#error "__GNUC__ cflags should be enabled"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <math.h>
#include <limits.h> /* INT_MAX */

#include "config.h"
#include "libavutil/avstring.h"
#include "libavutil/colorspace.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavutil/internal.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#ifdef __cplusplus
}
#endif

#define USE_PRE_AUDIO_BUF

const int AVCODEC_MAX_AUDIO_FRAME_SIZE = 192000; // Deprecated in ffmpeg

namespace android {

struct SoftFFmpegAudio : public SimpleSoftOMXComponent {
    SoftFFmpegAudio(const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftFFmpegAudio();

    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onPortEnableCompleted(OMX_U32 portIndex, bool enabled);

private:
    enum {
        kNumBuffers = 4,
        kOutputBufferSize = 4608 * 2
    };

    enum {
        MODE_MPEG,
        MODE_MPEGL1,
        MODE_MPEGL2,
        MODE_AAC,
        MODE_WMA,
        MODE_RA,
        MODE_AC3,
        MODE_APE,
        MODE_DTS,
        MODE_FLAC,
        MODE_VORBIS,
        MODE_HEURISTIC
    } mMode;

    enum {
        kPortIndexInput  = 0,
        kPortIndexOutput = 1,
    };

    bool mFFmpegInited;
    AVCodecContext *mCtx;
    struct SwrContext *mSwrCtx;

    bool mCodecAlreadyOpened;
    bool mExtradataReady;
    bool mIgnoreExtradata;
    bool mFlushComplete;
    bool mSignalledError;
    bool mReceivedEOS;

    AVFrame *mFrame;

    int64_t mAudioClock;
    int32_t mInputBufferSize;

#ifdef USE_PRE_AUDIO_BUF
    //"Fatal signal 7 (SIGBUS)"!!! SIGBUS is because of an alignment exception
    //LOCAL_CFLAGS += -D__GNUC__=1 in *.cpp file
    DECLARE_ALIGNED(16, uint8_t, mAudioBuffer)[AVCODEC_MAX_AUDIO_FRAME_SIZE * 4];
#else
    //Don't use the following code, because "NEON optimised stereo
    //fltp to s16 conversion" require byte alignment.
	uint8_t *mAudioBuffer;
	unsigned int mAudioBufferSize;
#endif

    uint8_t mSilenceBuffer[kOutputBufferSize];
    uint8_t *mResampledData;
    int32_t mResampledDataSize;

    //int32_t mNumChannels;
    //int32_t mSamplingRate;
    /* some audio codec need bit rate when init, e.g. wma, and should be > 0 */
    //int32_t mBitRate;
    //int32_t mBlockAlign;
    //AVSampleFormat mSamplingFmt;
    bool mAudioConfigChanged;

    enum AVSampleFormat mAudioSrcFmt;
    enum AVSampleFormat mAudioTgtFmt;
    int mAudioSrcChannels;
    int mAudioTgtChannels;
    int64_t mAudioSrcChannelLayout;
    int64_t mAudioTgtChannelLayout;
    int mAudioSrcFreq;
    int mAudioTgtFreq;

    enum {
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    } mOutputPortSettingsChange;

    void setMode(const char *name);
    void setAVCtxToDefault(AVCodecContext *avctx, const AVCodec *codec);
	void configDefaultCtx();
	OMX_ERRORTYPE isRoleSupported(const OMX_PARAM_COMPONENTROLETYPE *roleParams);
	void adjustAudioParameter();

    void initPorts();
    status_t initDecoder();
    void deInitDecoder();

    DISALLOW_EVIL_CONSTRUCTORS(SoftFFmpegAudio);
};

}  // namespace android

#endif  // SOFT_FFMPEGAUDIO_H_


