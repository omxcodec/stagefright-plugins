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
        kInputPortIndex   = 0,
        kOutputPortIndex  = 1,
        kNumInputBuffers  = 5,
        kNumOutputBuffers = 2,
    };

    enum {
        MODE_NONE,
        MODE_MPEG2,
        MODE_H263,
        MODE_MPEG4,
        MODE_WMV,
        MODE_RV,
        MODE_H264,
        MODE_VPX,
        MODE_VC1,
        MODE_FLV1,
        MODE_DIVX,
        MODE_TRIAL
    } mMode;

    enum EOSStatus {
        INPUT_DATA_AVAILABLE,
        INPUT_EOS_SEEN,
        OUTPUT_FRAMES_FLUSHED
    };

    enum {
        ERR_NO_FRM              = 2,
        ERR_FLUSHED             = 1,
		ERR_OK                  = 0,  //No errors
        ERR_OOM                 = -1, //Out of memmory
		ERR_CODEC_NOT_FOUND     = -2,
		ERR_DECODER_OPEN_FAILED = -2,
		ERR_SWS_FAILED          = -3,
    };

    bool mFFmpegAlreadyInited;
	bool mCodecAlreadyOpened;
	bool mPendingFrameAsSettingChanged;
    AVCodecContext *mCtx;
    struct SwsContext *mImgConvertCtx;
	AVFrame *mFrame;

    EOSStatus mEOSStatus;

    bool mExtradataReady;
    bool mIgnoreExtradata;
    bool mSignalledError;
    bool mDoDeinterlace;
    int32_t mWidth, mHeight, mStride;

    enum {
        NONE,
        AWAITING_DISABLED,
        AWAITING_ENABLED
    } mOutputPortSettingsChange;

    void setMode(const char *name);
    void initInputFormat(uint32_t mode, OMX_PARAM_PORTDEFINITIONTYPE &def);
	void getInputFormat(uint32_t mode, OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams);
    void setDefaultCtx(AVCodecContext *avctx, const AVCodec *codec);
    OMX_ERRORTYPE isRoleSupported(const OMX_PARAM_COMPONENTROLETYPE *roleParams);

    void initPorts();
    status_t initDecoder();
    void deInitDecoder();

	bool    handlePortSettingChangeEvent();
	int32_t handleExtradata();
	int32_t openDecoder();
    void    initPacket(AVPacket *pkt, OMX_BUFFERHEADERTYPE *inHeader);
    int32_t decodeVideo();
    int32_t preProcessVideoFrame(AVPicture *picture, void **bufp);
	int32_t drainOneOutputBuffer();
	void    drainEOSOutputBuffer();
	void    drainAllOutputBuffers();

    void updatePortDefinitions();

    DISALLOW_EVIL_CONSTRUCTORS(SoftFFmpegVideo);
};

}  // namespace android

#endif  // SOFT_FFMPEGVIDEO_H_
