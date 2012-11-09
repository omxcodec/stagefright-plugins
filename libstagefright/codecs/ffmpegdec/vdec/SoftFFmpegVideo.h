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

#ifndef SOFT_FFMPEGVIDEO_H_

#define SOFT_FFMPEGVIDEO_H_

#include "SimpleSoftOMXComponent.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
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
#include "libavcodec/audioconvert.h"
#include "libavutil/opt.h"
#include "libavutil/internal.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#include "cmdutils.h"

#include <SDL.h>
#include <SDL_thread.h>

#ifdef __cplusplus
}
#endif

namespace android {

struct SoftFFmpegVideo : public SimpleSoftOMXComponent {
    SoftFFmpegVideo(const char *name,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

protected:
    virtual ~SoftFFmpegVideo();

    virtual OMX_ERRORTYPE internalGetParameter(
            OMX_INDEXTYPE index, OMX_PTR params);

    virtual OMX_ERRORTYPE internalSetParameter(
            OMX_INDEXTYPE index, const OMX_PTR params);

    virtual void onQueueFilled(OMX_U32 portIndex);
    virtual void onPortFlushCompleted(OMX_U32 portIndex);
    virtual void onPortEnableCompleted(OMX_U32 portIndex, bool enabled);

private:
    enum {
        kNumInputBuffers  = 5,
        kNumOutputBuffers = 2,
    };

    enum {
        MODE_H264,
        MODE_MPEG4,
        MODE_MPEG2,
        MODE_H263,
        MODE_VC1
    } mMode;

    enum {
        kPortIndexInput  = 0,
        kPortIndexOutput = 1,
    };

    AVCodecContext *mCtx;
    struct SwsContext *mImgConvertCtx;

    bool mExtradataReady;
    bool mIgnoreExtradata;
    bool mSignalledError;
    int32_t mWidth, mHeight, mStride;

    enum {
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    } mOutputPortSettingsChange;

    void setAVCtxToDefault(AVCodecContext *avctx, const AVCodec *codec);

    void initPorts();
    status_t initDecoder();
    void deInitDecoder();

    void updatePortDefinitions();

    DISALLOW_EVIL_CONSTRUCTORS(SoftFFmpegVideo);
};

}  // namespace android

#endif  // SOFT_FFMPEGVIDEO_H_
