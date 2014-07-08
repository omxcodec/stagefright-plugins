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

//#define LOG_NDEBUG 0
#define LOG_TAG "FFmpegExtractor"
#include <utils/Log.h>

#include <stdint.h>
#include <limits.h> /* INT_MAX */
#include <inttypes.h>

#include <utils/misc.h>
#include <utils/String8.h>
#include <cutils/properties.h>
#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include "include/avc_utils.h"

#include "utils/codec_utils.h"
#include "utils/ffmpeg_cmdutils.h"

#include "FFmpegExtractor.h"

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_AUDIOQ_SIZE (20 * 16 * 1024)
#define MIN_FRAMES 5
#define EXTRACTOR_MAX_PROBE_PACKETS 200
#define FF_MAX_EXTRADATA_SIZE ((1 << 28) - FF_INPUT_BUFFER_PADDING_SIZE)

#define WAIT_KEY_PACKET_AFTER_SEEK 1
#define SUPPOURT_UNKNOWN_FORMAT    1

//debug
#define DEBUG_READ_ENTRY           0
#define DEBUG_DISABLE_VIDEO        0
#define DEBUG_DISABLE_AUDIO        0
#define DEBUG_PKT                  0

enum {
    NO_SEEK = 0,
    SEEK,
};

namespace android {

struct FFmpegSource : public MediaSource {
    FFmpegSource(const sp<FFmpegExtractor> &extractor, size_t index);

    virtual status_t start(MetaData *params);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options);

protected:
    virtual ~FFmpegSource();

private:
    friend struct FFmpegExtractor;

    sp<FFmpegExtractor> mExtractor;
    size_t mTrackIndex;

    enum AVMediaType mMediaType;

    mutable Mutex mLock;

    bool mIsAVC;
    size_t mNALLengthSize;
    bool mNal2AnnexB;

    AVStream *mStream;
    PacketQueue *mQueue;

    int64_t mFirstKeyPktTimestamp;

    DISALLOW_EVIL_CONSTRUCTORS(FFmpegSource);
};

////////////////////////////////////////////////////////////////////////////////

FFmpegExtractor::FFmpegExtractor(const sp<DataSource> &source, const sp<AMessage> &meta)
    : mDataSource(source),
      mMeta(new MetaData),
      mInitCheck(NO_INIT),
      mFFmpegInited(false),
      mFormatCtx(NULL),
      mReaderThreadStarted(false) {
    ALOGV("FFmpegExtractor::FFmpegExtractor");

    fetchStuffsFromSniffedMeta(meta);

    int err = initStreams();
    if (err < 0) {
        ALOGE("failed to init ffmpeg");
        return;
    }

    // start reader here, as we want to extract extradata from bitstream if no extradata
    startReaderThread();

    while(mProbePkts <= EXTRACTOR_MAX_PROBE_PACKETS && !mEOF &&
        (mFormatCtx->pb ? !mFormatCtx->pb->error : 1) &&
        (mDefersToCreateVideoTrack || mDefersToCreateAudioTrack)) {
        // FIXME, i am so lazy! Should use pthread_cond_wait to wait conditions
        usleep(5000);
    }

    ALOGV("mProbePkts: %d, mEOF: %d, pb->error(if has): %d, mDefersToCreateVideoTrack: %d, mDefersToCreateAudioTrack: %d",
        mProbePkts, mEOF, mFormatCtx->pb ? mFormatCtx->pb->error : 0, mDefersToCreateVideoTrack, mDefersToCreateAudioTrack);

    mInitCheck = OK;
}

FFmpegExtractor::~FFmpegExtractor() {
    ALOGV("FFmpegExtractor::~FFmpegExtractor");

    // stop reader here if no track!
    stopReaderThread();

    deInitStreams();
}

size_t FFmpegExtractor::countTracks() {
    return mInitCheck == OK ? mTracks.size() : 0;
}

sp<MediaSource> FFmpegExtractor::getTrack(size_t index) {
    ALOGV("FFmpegExtractor::getTrack[%d]", index);

    if (mInitCheck != OK) {
        return NULL;
    }

    if (index >= mTracks.size()) {
        return NULL;
    }

    return new FFmpegSource(this, index);
}

sp<MetaData> FFmpegExtractor::getTrackMetaData(size_t index, uint32_t flags) {
    ALOGV("FFmpegExtractor::getTrackMetaData[%d]", index);

    if (mInitCheck != OK) {
        return NULL;
    }

    if (index >= mTracks.size()) {
        return NULL;
    }

    return mTracks.itemAt(index).mMeta;
}

sp<MetaData> FFmpegExtractor::getMetaData() {
    ALOGV("FFmpegExtractor::getMetaData");

    if (mInitCheck != OK) {
        return NULL;
    }

    return mMeta;
}

uint32_t FFmpegExtractor::flags() const {
    ALOGV("FFmpegExtractor::flags");

    if (mInitCheck != OK) {
        return 0;
    }

    uint32_t flags = CAN_PAUSE;

    if (mFormatCtx->duration != AV_NOPTS_VALUE) {
        flags |= CAN_SEEK_BACKWARD | CAN_SEEK_FORWARD | CAN_SEEK;
    }

    return flags;
}

int FFmpegExtractor::check_extradata(AVCodecContext *avctx)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    const char *name = NULL;
    bool *defersToCreateTrack = NULL;
    AVBitStreamFilterContext **bsfc = NULL;

    // init
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        bsfc = &mVideoBsfc;
        defersToCreateTrack = &mDefersToCreateVideoTrack;
    } else if (avctx->codec_type == AVMEDIA_TYPE_AUDIO){
        bsfc = &mAudioBsfc;
        defersToCreateTrack = &mDefersToCreateAudioTrack;
    }

    codec_id = avctx->codec_id;

    // ignore extradata
    if (codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_MPEG1VIDEO
            && codec_id != AV_CODEC_ID_MPEG2VIDEO
            && codec_id != AV_CODEC_ID_AAC) {
        return 1;
    }

    // is extradata compatible with android?
    if (codec_id != AV_CODEC_ID_AAC) {
        int is_compatible = is_extradata_compatible_with_android(avctx);
        if (!is_compatible) {
            ALOGI("%s extradata is not compatible with android, should to extract it from bitstream",
                    av_get_media_type_string(avctx->codec_type));
            *defersToCreateTrack = true;
            *bsfc = NULL; // H264 don't need bsfc, only AAC?
            return 0;
        }
        return 1;
    }

    if (codec_id == AV_CODEC_ID_AAC) {
        name = "aac_adtstoasc";
    }

    if (avctx->extradata_size <= 0) {
        ALOGI("No %s extradata found, should to extract it from bitstream",
                av_get_media_type_string(avctx->codec_type));
        *defersToCreateTrack = true;
         //CHECK(name != NULL);
        if (!*bsfc && name) {
            *bsfc = av_bitstream_filter_init(name);
            if (!*bsfc) {
                ALOGE("Cannot open the %s BSF!", name);
                *defersToCreateTrack = false;
                return -1;
            } else {
                ALOGV("open the %s bsf", name);
                return 0;
            }
        } else {
            return 0;
        }
    }
    return 1;
}

void FFmpegExtractor::printTime(int64_t time)
{
    int hours, mins, secs, us;

    if (time == AV_NOPTS_VALUE)
        return;

    secs = time / AV_TIME_BASE;
    us = time % AV_TIME_BASE;
    mins = secs / 60;
    secs %= 60;
    hours = mins / 60;
    mins %= 60;
    ALOGI("the time is %02d:%02d:%02d.%02d",
        hours, mins, secs, (100 * us) / AV_TIME_BASE);
}

bool FFmpegExtractor::is_codec_supported(enum AVCodecID codec_id)
{
    bool supported = false;

    switch(codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AC3:
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
    case AV_CODEC_ID_WMV1:
    case AV_CODEC_ID_WMV2:
    case AV_CODEC_ID_WMV3:
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
    case AV_CODEC_ID_WMAPRO:
    case AV_CODEC_ID_WMALOSSLESS:
    case AV_CODEC_ID_RV20:
    case AV_CODEC_ID_RV30:
    case AV_CODEC_ID_RV40:
    case AV_CODEC_ID_COOK:
    case AV_CODEC_ID_APE:
    case AV_CODEC_ID_DTS:
    case AV_CODEC_ID_FLAC:
    case AV_CODEC_ID_FLV1:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_HEVC:

        supported = true;
        break;
    default:
        ALOGD("unsuppoted codec(%s), but give it a chance",
                avcodec_get_name(codec_id));
        //Won't promise that the following codec id can be supported.
        //Just give these codecs a chance.
        supported = true;
        break;
    }

    return supported;
}

sp<MetaData> FFmpegExtractor::setVideoFormat(AVStream *stream)
{
    AVCodecContext *avctx = NULL;
    sp<MetaData> meta = NULL;

    avctx = stream->codec;
    CHECK_EQ(avctx->codec_type, AVMEDIA_TYPE_VIDEO);

    switch(avctx->codec_id) {
    case AV_CODEC_ID_H264:
        if (avctx->extradata[0] == 1) {
            meta = setAVCFormat(avctx);
        } else {
            meta = setH264Format(avctx);
        }
        break;
    case AV_CODEC_ID_MPEG4:
        meta = setMPEG4Format(avctx);
        break;
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
        meta = setH263Format(avctx);
        break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
        meta = setMPEG2VIDEOFormat(avctx);
        break;
    case AV_CODEC_ID_VC1:
        meta = setVC1Format(avctx);
        break;
    case AV_CODEC_ID_WMV1:
        meta = setWMV1Format(avctx);
        break;
    case AV_CODEC_ID_WMV2:
        meta = setWMV2Format(avctx);
        break;
    case AV_CODEC_ID_WMV3:
        meta = setWMV3Format(avctx);
        break;
    case AV_CODEC_ID_RV20:
        meta = setRV20Format(avctx);
        break;
    case AV_CODEC_ID_RV30:
        meta = setRV30Format(avctx);
        break;
    case AV_CODEC_ID_RV40:
        meta = setRV40Format(avctx);
        break;
    case AV_CODEC_ID_FLV1:
        meta = setFLV1Format(avctx);
        break;
    case AV_CODEC_ID_HEVC:
        meta = setHEVCFormat(avctx);
        break;
    default:
        ALOGD("unsuppoted video codec(id:%d, name:%s), but give it a chance",
                avctx->codec_id, avcodec_get_name(avctx->codec_id));
        meta = new MetaData;
        meta->setInt32(kKeyCodecId, avctx->codec_id);
        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_FFMPEG);
        if (avctx->extradata_size > 0) {
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
        }
        //CHECK(!"Should not be here. Unsupported codec.");
        break;
    }

    if (meta != NULL) {
        ALOGI("width: %d, height: %d, bit_rate: %d",
                avctx->width, avctx->height, avctx->bit_rate);

        meta->setInt32(kKeyWidth, avctx->width);
        meta->setInt32(kKeyHeight, avctx->height);
        if (avctx->bit_rate > 0) {
            meta->setInt32(kKeyBitRate, avctx->bit_rate);
        }
        setDurationMetaData(stream, meta);
    }

    return meta;
}

sp<MetaData> FFmpegExtractor::setAudioFormat(AVStream *stream)
{
    AVCodecContext *avctx = NULL;
    sp<MetaData> meta = NULL;

    avctx = stream->codec;
    CHECK_EQ(avctx->codec_type, AVMEDIA_TYPE_AUDIO);

    switch(avctx->codec_id) {
    case AV_CODEC_ID_MP2:
        meta = setMP2Format(avctx);
        break;
    case AV_CODEC_ID_MP3:
        meta = setMP3Format(avctx);
        break;
    case AV_CODEC_ID_VORBIS:
        meta = setVORBISFormat(avctx);
        break;
    case AV_CODEC_ID_AC3:
        meta = setAC3Format(avctx);
        break;
    case AV_CODEC_ID_AAC:
        meta = setAACFormat(avctx);
        break;
    case AV_CODEC_ID_WMAV1:
        meta = setWMAV1Format(avctx);
        break;
    case AV_CODEC_ID_WMAV2:
        meta = setWMAV2Format(avctx);
        break;
    case AV_CODEC_ID_WMAPRO:
        meta = setWMAProFormat(avctx);
        break;
    case AV_CODEC_ID_WMALOSSLESS:
        meta = setWMALossLessFormat(avctx);
        break;
    case AV_CODEC_ID_COOK:
        meta = setRAFormat(avctx);
        break;
    case AV_CODEC_ID_APE:
        meta = setAPEFormat(avctx);
        break;
    case AV_CODEC_ID_DTS:
        meta = setDTSFormat(avctx);
        break;
    case AV_CODEC_ID_FLAC:
        meta = setFLACFormat(avctx);
        break;
    default:
        ALOGD("unsuppoted audio codec(id:%d, name:%s), but give it a chance",
                avctx->codec_id, avcodec_get_name(avctx->codec_id));
        meta = new MetaData;
        meta->setInt32(kKeyCodecId, avctx->codec_id);
        meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_FFMPEG);
        if (avctx->extradata_size > 0) {
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
        }
        //CHECK(!"Should not be here. Unsupported codec.");
        break;
    }

    if (meta != NULL) {
        ALOGI("bit_rate: %d, sample_rate: %d, channels: %d, "
                "bits_per_coded_sample: %d, block_align:%d",
                avctx->bit_rate, avctx->sample_rate, avctx->channels,
                avctx->bits_per_coded_sample, avctx->block_align);

        meta->setInt32(kKeyChannelCount, avctx->channels);
        meta->setInt32(kKeyBitRate, avctx->bit_rate);
        meta->setInt32(kKeyBitspersample, avctx->bits_per_coded_sample);
        meta->setInt32(kKeySampleRate, avctx->sample_rate);
        meta->setInt32(kKeyBlockAlign, avctx->block_align);
        meta->setInt32(kKeySampleFormat, avctx->sample_fmt);
        setDurationMetaData(stream, meta);
    }

    return meta;
}

void FFmpegExtractor::setDurationMetaData(AVStream *stream, sp<MetaData> &meta)
{
    AVCodecContext *avctx = stream->codec;

    if (stream->duration != AV_NOPTS_VALUE) {
        int64_t duration = stream->duration * av_q2d(stream->time_base) * 1000000;
        printTime(duration);
        const char *s = av_get_media_type_string(avctx->codec_type);
        if (stream->start_time != AV_NOPTS_VALUE) {
            ALOGV("%s startTime:%lld", s, stream->start_time);
        } else {
            ALOGV("%s startTime:N/A", s);
        }
        meta->setInt64(kKeyDuration, duration);
    } else {
        // default when no stream duration
        meta->setInt64(kKeyDuration, mFormatCtx->duration);
    }
}

int FFmpegExtractor::stream_component_open(int stream_index)
{
    TrackInfo *trackInfo = NULL;
    AVCodecContext *avctx = NULL;
    sp<MetaData> meta = NULL;
    bool supported = false;
    uint32_t type = 0;
    const void *data = NULL;
    size_t size = 0;
    int ret = 0;

    ALOGI("stream_index: %d", stream_index);
    if (stream_index < 0 || stream_index >= (int)mFormatCtx->nb_streams)
        return -1;
    avctx = mFormatCtx->streams[stream_index]->codec;

    supported = is_codec_supported(avctx->codec_id);

    if (!supported) {
        ALOGE("unsupport the codec(%s)", avcodec_get_name(avctx->codec_id));
        return -1;
    }
    ALOGI("support the codec(%s)", avcodec_get_name(avctx->codec_id));

    unsigned streamType;
    for (size_t i = 0; i < mTracks.size(); ++i) {
        if (stream_index == mTracks.editItemAt(i).mIndex) {
            ALOGE("this track already exists");
            return 0;
        }
    }

    mFormatCtx->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    char tagbuf[32];
    av_get_codec_tag_string(tagbuf, sizeof(tagbuf), avctx->codec_tag);
    ALOGV("Tag %s/0x%08x with codec(%s)\n", tagbuf, avctx->codec_tag, avcodec_get_name(avctx->codec_id));

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (mVideoStreamIdx == -1)
            mVideoStreamIdx = stream_index;
        if (mVideoStream == NULL)
            mVideoStream = mFormatCtx->streams[stream_index];

        ret = check_extradata(avctx);
        if (ret != 1) {
            if (ret == -1) {
                // disable the stream
                mVideoStreamIdx = -1;
                mVideoStream = NULL;
                packet_queue_end(&mVideoQ);
                mFormatCtx->streams[stream_index]->discard = AVDISCARD_ALL;
            }
            return ret;
         }

        if (avctx->extradata) {
            ALOGV("video stream extradata:");
            hexdump(avctx->extradata, avctx->extradata_size);
        } else {
            ALOGV("video stream no extradata, but we can ignore it.");
        }

        meta = setVideoFormat(mVideoStream);
        if (meta == NULL) {
            ALOGE("setVideoFormat failed");
            return -1;
        }

        ALOGV("create a video track");
        mTracks.push();
        trackInfo = &mTracks.editItemAt(mTracks.size() - 1);
        trackInfo->mIndex  = stream_index;
        trackInfo->mMeta   = meta;
        trackInfo->mStream = mVideoStream;
        trackInfo->mQueue  = &mVideoQ;

        mDefersToCreateVideoTrack = false;

        break;
    case AVMEDIA_TYPE_AUDIO:
        if (mAudioStreamIdx == -1)
            mAudioStreamIdx = stream_index;
        if (mAudioStream == NULL)
            mAudioStream = mFormatCtx->streams[stream_index];

        ret = check_extradata(avctx);
        if (ret != 1) {
            if (ret == -1) {
                // disable the stream
                mAudioStreamIdx = -1;
                mAudioStream = NULL;
                packet_queue_end(&mAudioQ);
                mFormatCtx->streams[stream_index]->discard = AVDISCARD_ALL;
            }
            return ret;
        }

        if (avctx->extradata) {
            ALOGV("audio stream extradata(%d):", avctx->extradata_size);
            hexdump(avctx->extradata, avctx->extradata_size);
        } else {
            ALOGV("audio stream no extradata, but we can ignore it.");
        }

        meta = setAudioFormat(mAudioStream);
        if (meta == NULL) {
            ALOGE("setAudioFormat failed");
            return -1;
        }

        ALOGV("create a audio track");
        mTracks.push();
        trackInfo = &mTracks.editItemAt(mTracks.size() - 1);
        trackInfo->mIndex  = stream_index;
        trackInfo->mMeta   = meta;
        trackInfo->mStream = mAudioStream;
        trackInfo->mQueue  = &mAudioQ;

        mDefersToCreateAudioTrack = false;

        break;
    case AVMEDIA_TYPE_SUBTITLE:
        /* Unsupport now */
        CHECK(!"Should not be here. Unsupported media type.");
        break;
    default:
        CHECK(!"Should not be here. Unsupported media type.");
        break;
    }
    return 0;
}

void FFmpegExtractor::stream_component_close(int stream_index)
{
    AVCodecContext *avctx;

    if (stream_index < 0 || stream_index >= (int)mFormatCtx->nb_streams)
        return;
    avctx = mFormatCtx->streams[stream_index]->codec;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        ALOGV("packet_queue_abort videoq");
        packet_queue_abort(&mVideoQ);
        /* wait until the end */
        while (!mAbortRequest && !mVideoEOSReceived) {
            ALOGV("wait for video received");
            usleep(10000);
        }
        ALOGV("packet_queue_end videoq");
        packet_queue_end(&mVideoQ);
        break;
    case AVMEDIA_TYPE_AUDIO:
        ALOGV("packet_queue_abort audioq");
        packet_queue_abort(&mAudioQ);
        while (!mAbortRequest && !mAudioEOSReceived) {
            ALOGV("wait for audio received");
            usleep(10000);
        }
        ALOGV("packet_queue_end audioq");
        packet_queue_end(&mAudioQ);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        break;
    default:
        break;
    }

    mFormatCtx->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        mVideoStream    = NULL;
        mVideoStreamIdx = -1;
        if (mVideoBsfc) {
            av_bitstream_filter_close(mVideoBsfc);
            mVideoBsfc  = NULL;
        }
        break;
    case AVMEDIA_TYPE_AUDIO:
        mAudioStream    = NULL;
        mAudioStreamIdx = -1;
        if (mAudioBsfc) {
            av_bitstream_filter_close(mAudioBsfc);
            mAudioBsfc  = NULL;
        }
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        break;
    default:
        break;
    }
}

void FFmpegExtractor::reachedEOS(enum AVMediaType media_type)
{
    Mutex::Autolock autoLock(mLock);

    if (media_type == AVMEDIA_TYPE_VIDEO) {
        mVideoEOSReceived = true;
    } else if (media_type == AVMEDIA_TYPE_AUDIO) {
        mAudioEOSReceived = true;
    }
}

/* seek in the stream */
int FFmpegExtractor::stream_seek(int64_t pos, enum AVMediaType media_type)
{
    Mutex::Autolock autoLock(mLock);

    if (mVideoStreamIdx >= 0
            && mAudioStreamIdx >= 0
            && media_type == AVMEDIA_TYPE_AUDIO
            && !mVideoEOSReceived) {
       return NO_SEEK;
    }

    // flush immediately
    if (mAudioStreamIdx >= 0)
        packet_queue_flush(&mAudioQ);
    if (mVideoStreamIdx >= 0)
        packet_queue_flush(&mVideoQ);

    mSeekPos = pos;
    mSeekFlags &= ~AVSEEK_FLAG_BYTE;
    mSeekReq = 1;

    return SEEK;
}

// staitc
int FFmpegExtractor::decode_interrupt_cb(void *ctx)
{
    FFmpegExtractor *extrator = static_cast<FFmpegExtractor *>(ctx);
    return extrator->mAbortRequest;
}

void FFmpegExtractor::fetchStuffsFromSniffedMeta(const sp<AMessage> &meta)
{
    AString url;
    AString mime;

    //url
    CHECK(meta->findString("extended-extractor-url", &url));
    CHECK(url.c_str() != NULL);
    CHECK(url.size() < PATH_MAX);

    memcpy(mFilename, url.c_str(), url.size());
    mFilename[url.size()] = '\0';

    //mime
    CHECK(meta->findString("extended-extractor-mime", &mime));
    CHECK(mime.c_str() != NULL);
    mMeta->setCString(kKeyMIMEType, mime.c_str());
}

void FFmpegExtractor::setFFmpegDefaultOpts()
{
    mGenPTS       = 0;
#if DEBUG_DISABLE_VIDEO
    mVideoDisable = 1;
#else
    mVideoDisable = 0;
#endif
#if DEBUG_DISABLE_AUDIO
    mAudioDisable = 1;
#else
    mAudioDisable = 0;
#endif
    mShowStatus   = 0;
    mSeekByBytes  = 0; /* seek by bytes 0=off 1=on -1=auto" */
    mDuration     = AV_NOPTS_VALUE;
    mSeekPos      = AV_NOPTS_VALUE;
    mAutoExit     = 1;
    mLoop         = 1;

    mVideoStreamIdx = -1;
    mAudioStreamIdx = -1;
    mVideoStream  = NULL;
    mAudioStream  = NULL;
    mDefersToCreateVideoTrack = false;
    mDefersToCreateAudioTrack = false;
    mVideoBsfc = NULL;
    mAudioBsfc = NULL;

    mAbortRequest = 0;
    mPaused       = 0;
    mLastPaused   = 0;
    mSeekReq      = 0;

    mProbePkts    = 0;
    mEOF          = false;
}

int FFmpegExtractor::initStreams()
{
    int err = 0;
    int i = 0;
    status_t status = UNKNOWN_ERROR;
    int eof = 0;
    int ret = 0, audio_ret = -1, video_ret = -1;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t = NULL;
    AVDictionary **opts = NULL;
    int orig_nb_streams = 0;
    int st_index[AVMEDIA_TYPE_NB] = {0};
    int wanted_stream[AVMEDIA_TYPE_NB] = {0};
    st_index[AVMEDIA_TYPE_AUDIO]  = -1;
    st_index[AVMEDIA_TYPE_VIDEO]  = -1;
    wanted_stream[AVMEDIA_TYPE_AUDIO]  = -1;
    wanted_stream[AVMEDIA_TYPE_VIDEO]  = -1;
    const char *mime = NULL;

    setFFmpegDefaultOpts();

    status = initFFmpeg();
    if (status != OK) {
        ret = -1;
        goto fail;
    }
    mFFmpegInited = true;

    mFormatCtx = avformat_alloc_context();
    if (!mFormatCtx)
    {
        ALOGE("oom for alloc avformat context");
        ret = -1;
        goto fail;
    }
    mFormatCtx->interrupt_callback.callback = decode_interrupt_cb;
    mFormatCtx->interrupt_callback.opaque = this;
    ALOGV("mFilename: %s", mFilename);
    err = avformat_open_input(&mFormatCtx, mFilename, NULL, &format_opts);
    if (err < 0) {
        ALOGE("%s: avformat_open_input failed, err:%s", mFilename, av_err2str(err));
        ret = -1;
        goto fail;
    }

    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        ALOGE("Option %s not found.\n", t->key);
        //ret = AVERROR_OPTION_NOT_FOUND;
        ret = -1;
        goto fail;
    }

    if (mGenPTS)
        mFormatCtx->flags |= AVFMT_FLAG_GENPTS;

    opts = setup_find_stream_info_opts(mFormatCtx, codec_opts);
    orig_nb_streams = mFormatCtx->nb_streams;

    err = avformat_find_stream_info(mFormatCtx, opts);
    if (err < 0) {
        ALOGE("%s: could not find stream info, err:%s", mFilename, av_err2str(err));
        ret = -1;
        goto fail;
    }
    for (i = 0; i < orig_nb_streams; i++)
        av_dict_free(&opts[i]);
    av_freep(&opts);

    if (mFormatCtx->pb)
        mFormatCtx->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use url_feof() to test for the end

    if (mSeekByBytes < 0)
        mSeekByBytes = !!(mFormatCtx->iformat->flags & AVFMT_TS_DISCONT);

    for (i = 0; i < (int)mFormatCtx->nb_streams; i++)
        mFormatCtx->streams[i]->discard = AVDISCARD_ALL;
    if (!mVideoDisable)
        st_index[AVMEDIA_TYPE_VIDEO] =
            av_find_best_stream(mFormatCtx, AVMEDIA_TYPE_VIDEO,
                                wanted_stream[AVMEDIA_TYPE_VIDEO], -1, NULL, 0);
    if (!mAudioDisable)
        st_index[AVMEDIA_TYPE_AUDIO] =
            av_find_best_stream(mFormatCtx, AVMEDIA_TYPE_AUDIO,
                                wanted_stream[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);
    if (mShowStatus) {
        av_dump_format(mFormatCtx, 0, mFilename, 0);
    }

    if (mFormatCtx->duration != AV_NOPTS_VALUE &&
            mFormatCtx->start_time != AV_NOPTS_VALUE) {
        int hours, mins, secs, us;

        ALOGV("file startTime: %lld", mFormatCtx->start_time);

        mDuration = mFormatCtx->duration;

        secs = mDuration / AV_TIME_BASE;
        us = mDuration % AV_TIME_BASE;
        mins = secs / 60;
        secs %= 60;
        hours = mins / 60;
        mins %= 60;
        ALOGI("the duration is %02d:%02d:%02d.%02d",
            hours, mins, secs, (100 * us) / AV_TIME_BASE);
    }

    packet_queue_init(&mVideoQ);
    packet_queue_init(&mAudioQ);

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        audio_ret = stream_component_open(st_index[AVMEDIA_TYPE_AUDIO]);
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        video_ret = stream_component_open(st_index[AVMEDIA_TYPE_VIDEO]);
    }

    if ( audio_ret < 0 && video_ret < 0) {
        ALOGE("%s: could not open codecs\n", mFilename);
        ret = -1;
        goto fail;
    }

    ret = 0;

fail:
    return ret;
}

void FFmpegExtractor::deInitStreams()
{
    packet_queue_destroy(&mVideoQ);
    packet_queue_destroy(&mAudioQ);

    if (mFormatCtx) {
        avformat_close_input(&mFormatCtx);
    }

    if (mFFmpegInited) {
        deInitFFmpeg();
    }
}

status_t FFmpegExtractor::startReaderThread() {
    ALOGV("Starting reader thread");
    Mutex::Autolock autoLock(mLock);

    if (mReaderThreadStarted)
        return OK;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&mReaderThread, &attr, ReaderWrapper, this);
    pthread_attr_destroy(&attr);
    mReaderThreadStarted = true;
    ALOGD("Reader thread started");

    return OK;
}

void FFmpegExtractor::stopReaderThread() {
    ALOGV("Stopping reader thread");
    Mutex::Autolock autoLock(mLock);

    if (!mReaderThreadStarted) {
        ALOGD("Reader thread have been stopped");
        return;
    }

    mAbortRequest = 1;

    void *dummy;
    pthread_join(mReaderThread, &dummy);
    mReaderThreadStarted = false;
    ALOGD("Reader thread stopped");
}

// static
void *FFmpegExtractor::ReaderWrapper(void *me) {
    ((FFmpegExtractor *)me)->readerEntry();

    return NULL;
}

void FFmpegExtractor::readerEntry() {
    int err, i, ret;
    AVPacket pkt1, *pkt = &pkt1;
    int eof = 0;
    int pkt_in_play_range = 0;

    ALOGV("FFmpegExtractor enter thread(readerEntry)");

    mVideoEOSReceived = false;
    mAudioEOSReceived = false;

    for (;;) {
        if (mAbortRequest)
            break;

        if (mPaused != mLastPaused) {
            mLastPaused = mPaused;
            if (mPaused)
                mReadPauseReturn = av_read_pause(mFormatCtx);
            else
                av_read_play(mFormatCtx);
        }
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (mPaused &&
                (!strcmp(mFormatCtx->iformat->name, "rtsp") ||
                 (mFormatCtx->pb && !strncmp(mFilename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            usleep(10000);
            continue;
        }
#endif

        if (mSeekReq) {
            ALOGV("readerEntry, mSeekReq: %d", mSeekReq);
            ret = avformat_seek_file(mFormatCtx, -1, INT64_MIN, mSeekPos, INT64_MAX, mSeekFlags);
            if (ret < 0) {
                ALOGE("%s: error while seeking", mFormatCtx->filename);
            } else {
                if (mAudioStreamIdx >= 0) {
                    packet_queue_flush(&mAudioQ);
                    packet_queue_put(&mAudioQ, &mAudioQ.flush_pkt);
                }
                if (mVideoStreamIdx >= 0) {
                    packet_queue_flush(&mVideoQ);
                    packet_queue_put(&mVideoQ, &mVideoQ.flush_pkt);
                }
            }
            mSeekReq = 0;
            eof = 0;
        }

        /* if the queue are full, no need to read more */
        if (   mAudioQ.size + mVideoQ.size > MAX_QUEUE_SIZE
            || (   (mAudioQ   .size  > MIN_AUDIOQ_SIZE || mAudioStreamIdx < 0)
                && (mVideoQ   .nb_packets > MIN_FRAMES || mVideoStreamIdx < 0))) {
#if DEBUG_READ_ENTRY
            ALOGV("readerEntry, full(wtf!!!), mVideoQ.size: %d, mVideoQ.nb_packets: %d, mAudioQ.size: %d, mAudioQ.nb_packets: %d",
                    mVideoQ.size, mVideoQ.nb_packets, mAudioQ.size, mAudioQ.nb_packets);
#endif
            /* wait 10 ms */
            usleep(10000);
            continue;
        }

        if (eof) {
            if (mVideoStreamIdx >= 0) {
                packet_queue_put_nullpacket(&mVideoQ, mVideoStreamIdx);
            }
            if (mAudioStreamIdx >= 0) {
                packet_queue_put_nullpacket(&mAudioQ, mAudioStreamIdx);
            }
            usleep(10000);
#if DEBUG_READ_ENTRY
            ALOGV("readerEntry, eof = 1, mVideoQ.size: %d, mVideoQ.nb_packets: %d, mAudioQ.size: %d, mAudioQ.nb_packets: %d",
                    mVideoQ.size, mVideoQ.nb_packets, mAudioQ.size, mAudioQ.nb_packets);
#endif
            if (mAudioQ.size + mVideoQ.size  == 0) {
                if (mAutoExit) {
                    ret = AVERROR_EOF;
                    goto fail;
                }
            }
            eof=0;
            continue;
        }

        ret = av_read_frame(mFormatCtx, pkt);
        mProbePkts++;
        if (ret < 0) {
            if (ret == AVERROR_EOF || url_feof(mFormatCtx->pb))
                if (ret == AVERROR_EOF) {
                    //ALOGV("ret == AVERROR_EOF");
                }
                if (url_feof(mFormatCtx->pb)) {
                    //ALOGV("url_feof(mFormatCtx->pb)");
                }

                eof = 1;
                mEOF = true;
            if (mFormatCtx->pb && mFormatCtx->pb->error) {
                ALOGE("mFormatCtx->pb->error: %d", mFormatCtx->pb->error);
                break;
            }
            usleep(100000);
            continue;
        }

        if (pkt->stream_index == mVideoStreamIdx) {
             if (mDefersToCreateVideoTrack) {
                AVCodecContext *avctx = mFormatCtx->streams[mVideoStreamIdx]->codec;

                int i = parser_split(avctx, pkt->data, pkt->size);
                if (i > 0 && i < FF_MAX_EXTRADATA_SIZE) {
                    if (avctx->extradata)
                        av_freep(&avctx->extradata);
                    avctx->extradata_size= i;
                    avctx->extradata = (uint8_t *)av_malloc(avctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                    if (!avctx->extradata) {
                        //return AVERROR(ENOMEM);
                        ret = AVERROR(ENOMEM);
                        goto fail;
                    }
                    // sps + pps(there may be sei in it)
                    memcpy(avctx->extradata, pkt->data, avctx->extradata_size);
                    memset(avctx->extradata + i, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                } else {
                    av_free_packet(pkt);
                    continue;
                }

                stream_component_open(mVideoStreamIdx);
                if (!mDefersToCreateVideoTrack)
                    ALOGI("probe packet counter: %d when create video track ok", mProbePkts);
                if (mProbePkts == EXTRACTOR_MAX_PROBE_PACKETS)
                    ALOGI("probe packet counter to max: %d, create video track: %d",
                        mProbePkts, !mDefersToCreateVideoTrack);
            }
        } else if (pkt->stream_index == mAudioStreamIdx) {
            int ret;
            uint8_t *outbuf;
            int   outbuf_size;
            AVCodecContext *avctx = mFormatCtx->streams[mAudioStreamIdx]->codec;
            if (mAudioBsfc && pkt && pkt->data) {
                ret = av_bitstream_filter_filter(mAudioBsfc, avctx, NULL, &outbuf, &outbuf_size,
                                   pkt->data, pkt->size, pkt->flags & AV_PKT_FLAG_KEY);

                if (ret < 0 ||!outbuf_size) {
                    av_free_packet(pkt);
                    continue;
                }
                if (outbuf && outbuf != pkt->data) {
                    memmove(pkt->data, outbuf, outbuf_size);
                    pkt->size = outbuf_size;
                }
            }
            if (mDefersToCreateAudioTrack) {
                if (avctx->extradata_size <= 0) {
                    av_free_packet(pkt);
                    continue;
                }
                stream_component_open(mAudioStreamIdx);
                if (!mDefersToCreateAudioTrack)
                    ALOGI("probe packet counter: %d when create audio track ok", mProbePkts);
                if (mProbePkts == EXTRACTOR_MAX_PROBE_PACKETS)
                    ALOGI("probe packet counter to max: %d, create audio track: %d",
                        mProbePkts, !mDefersToCreateAudioTrack);
            }
        }

        if (pkt->stream_index == mAudioStreamIdx) {
            packet_queue_put(&mAudioQ, pkt);
        } else if (pkt->stream_index == mVideoStreamIdx) {
            packet_queue_put(&mVideoQ, pkt);
        } else {
            av_free_packet(pkt);
        }
    }
    /* wait until the end */
    while (!mAbortRequest) {
        usleep(100000);
    }

    ret = 0;
fail:
    ALOGI("reader thread goto end...");

    /* close each stream */
    if (mAudioStreamIdx >= 0)
        stream_component_close(mAudioStreamIdx);
    if (mVideoStreamIdx >= 0)
        stream_component_close(mVideoStreamIdx);
    if (mFormatCtx) {
        avformat_close_input(&mFormatCtx);
    }

    ALOGV("FFmpegExtractor exit thread(readerEntry)");
}

////////////////////////////////////////////////////////////////////////////////

FFmpegSource::FFmpegSource(
        const sp<FFmpegExtractor> &extractor, size_t index)
    : mExtractor(extractor),
      mTrackIndex(index),
      mIsAVC(false),
      mNal2AnnexB(false),
      mStream(mExtractor->mTracks.itemAt(index).mStream),
      mQueue(mExtractor->mTracks.itemAt(index).mQueue) {
    sp<MetaData> meta = mExtractor->mTracks.itemAt(index).mMeta;

    /* H.264 Video Types */
    {
        AVCodecContext *avctx = mStream->codec;

        if (avctx->codec_id == AV_CODEC_ID_H264
                && avctx->extradata_size > 0
                && avctx->extradata[0] == 1) {
            mIsAVC = true;

            uint32_t type;
            const void *data;
            size_t size;
            CHECK(meta->findData(kKeyAVCC, &type, &data, &size));

            const uint8_t *ptr = (const uint8_t *)data;

            CHECK(size >= 7);
            CHECK_EQ((unsigned)ptr[0], 1u);  // configurationVersion == 1

            // The number of bytes used to encode the length of a NAL unit.
            mNALLengthSize = 1 + (ptr[4] & 3);

            ALOGV("the stream is AVC, the length of a NAL unit: %d", mNALLengthSize);

            mNal2AnnexB = true;
        }
    }

    mMediaType = mStream->codec->codec_type;
    mFirstKeyPktTimestamp = AV_NOPTS_VALUE;
}

FFmpegSource::~FFmpegSource() {
    ALOGV("FFmpegSource::~FFmpegSource %s",
            av_get_media_type_string(mMediaType));
    mExtractor = NULL;
}

status_t FFmpegSource::start(MetaData *params) {
    ALOGV("FFmpegSource::start %s",
            av_get_media_type_string(mMediaType));
    return OK;
}

status_t FFmpegSource::stop() {
    ALOGV("FFmpegSource::stop %s",
            av_get_media_type_string(mMediaType));
    return OK;
}

sp<MetaData> FFmpegSource::getFormat() {
    return mExtractor->mTracks.itemAt(mTrackIndex).mMeta;;
}

status_t FFmpegSource::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    *buffer = NULL;

    Mutex::Autolock autoLock(mLock);

    AVPacket pkt;
    bool seeking = false;
    bool waitKeyPkt = false;
    ReadOptions::SeekMode mode;
    int64_t pktTS = AV_NOPTS_VALUE;
    int64_t seekTimeUs = AV_NOPTS_VALUE;
    int64_t timeUs = AV_NOPTS_VALUE;
    int key = 0;
    status_t status = OK;

    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        ALOGV("~~~%s seekTimeUs: %lld, mode: %d", av_get_media_type_string(mMediaType), seekTimeUs, mode);
        /* add the stream start time */
        if (mStream->start_time != AV_NOPTS_VALUE)
            seekTimeUs += mStream->start_time * av_q2d(mStream->time_base) * 1000000;
        ALOGV("~~~%s seekTimeUs[+startTime]: %lld, mode: %d", av_get_media_type_string(mMediaType), seekTimeUs, mode);

        if (mExtractor->stream_seek(seekTimeUs, mMediaType) == SEEK)
            seeking = true;
    }

retry:
    if (packet_queue_get(mQueue, &pkt, 1) < 0) {
        ALOGD("read %s abort reqeust", av_get_media_type_string(mMediaType));
        mExtractor->reachedEOS(mMediaType);
        return ERROR_END_OF_STREAM;
    }

    if (seeking) {
        if (pkt.data != mQueue->flush_pkt.data) {
            av_free_packet(&pkt);
            goto retry;
        } else {
            seeking = false;
#if WAIT_KEY_PACKET_AFTER_SEEK
            waitKeyPkt = true;
#endif
        }
    }

    if (pkt.data == mQueue->flush_pkt.data) {
        ALOGV("read %s flush pkt", av_get_media_type_string(mMediaType));
        av_free_packet(&pkt);
        mFirstKeyPktTimestamp = AV_NOPTS_VALUE;
        goto retry;
    } else if (pkt.data == NULL && pkt.size == 0) {
        ALOGD("read %s eos pkt", av_get_media_type_string(mMediaType));
        av_free_packet(&pkt);
        mExtractor->reachedEOS(mMediaType);
        return ERROR_END_OF_STREAM;
    }

    key = pkt.flags & AV_PKT_FLAG_KEY ? 1 : 0;
    pktTS = pkt.pts; //FIXME AV_NOPTS_VALUE??

    //use dts when AVI
    if (pkt.pts == AV_NOPTS_VALUE)
        pktTS = pkt.dts;

    //FIXME, drop, omxcodec requires a positive timestamp! e.g. vorbis
    if (pktTS != AV_NOPTS_VALUE && pktTS < 0) {
        ALOGW("drop the packet with negative timestamp(pts:%lld)", pktTS);
        av_free_packet(&pkt);
        goto retry;
    }

    if (waitKeyPkt) {
        if (!key) {
            ALOGV("drop the non-key packet");
            av_free_packet(&pkt);
            goto retry;
        } else {
            ALOGV("~~~~~~ got the key packet");
            waitKeyPkt = false;
        }
    }

    if (pktTS != AV_NOPTS_VALUE && mFirstKeyPktTimestamp == AV_NOPTS_VALUE) {
        // update the first key timestamp
        mFirstKeyPktTimestamp = pktTS;
    }
     
    if (pktTS != AV_NOPTS_VALUE && pktTS < mFirstKeyPktTimestamp) {
            ALOGV("drop the packet with the backward timestamp, maybe they are B-frames after I-frame ^_^");
            av_free_packet(&pkt);
            goto retry;
    }

    MediaBuffer *mediaBuffer = new MediaBuffer(pkt.size + FF_INPUT_BUFFER_PADDING_SIZE);
    mediaBuffer->meta_data()->clear();
    mediaBuffer->set_range(0, pkt.size);

    //copy data
    if (mIsAVC && mNal2AnnexB) {
        /* This only works for NAL sizes 3-4 */
        CHECK(mNALLengthSize == 3 || mNALLengthSize == 4);

        uint8_t *dst = (uint8_t *)mediaBuffer->data();
        /* Convert H.264 NAL format to annex b */
        status = convertNal2AnnexB(dst, pkt.size, pkt.data, pkt.size, mNALLengthSize);
        if (status != OK) {
            ALOGE("convertNal2AnnexB failed");
            mediaBuffer->release();
            mediaBuffer = NULL;
            av_free_packet(&pkt);
            return ERROR_MALFORMED;
        }
    } else {
        memcpy(mediaBuffer->data(), pkt.data, pkt.size);
    }

    int64_t start_time = mStream->start_time != AV_NOPTS_VALUE ? mStream->start_time : 0;
    if (pktTS != AV_NOPTS_VALUE)
        timeUs = (int64_t)((pktTS - start_time) * av_q2d(mStream->time_base) * 1000000);
    else
        timeUs = SF_NOPTS_VALUE; //FIXME AV_NOPTS_VALUE is negative, but stagefright need positive

#if DEBUG_PKT
    if (pktTS != AV_NOPTS_VALUE)
        ALOGV("read %s pkt, size:%d, key:%d, pts:%lld, dts:%lld, timeUs[-startTime]:%lld us (%.2f secs)",
            av_get_media_type_string(mMediaType), pkt.size, key, pkt.pts, pkt.dts, timeUs, timeUs/1E6);
    else
        ALOGV("read %s pkt, size:%d, key:%d, pts:N/A, dts:N/A, timeUs[-startTime]:N/A",
            av_get_media_type_string(mMediaType), pkt.size, key);
#endif

    mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);
    mediaBuffer->meta_data()->setInt32(kKeyIsSyncFrame, key);

    *buffer = mediaBuffer;

    av_free_packet(&pkt);

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

typedef struct {
    const char *format;
    const char *container;
} formatmap;

static formatmap FILE_FORMATS[] = {
        {"mpeg",                    MEDIA_MIMETYPE_CONTAINER_MPEG2PS  },
        {"mpegts",                  MEDIA_MIMETYPE_CONTAINER_TS       },
        {"mov,mp4,m4a,3gp,3g2,mj2", MEDIA_MIMETYPE_CONTAINER_MPEG4    },
        {"matroska,webm",           MEDIA_MIMETYPE_CONTAINER_MATROSKA },
        {"asf",                     MEDIA_MIMETYPE_CONTAINER_ASF      },
        {"rm",                      MEDIA_MIMETYPE_CONTAINER_RM       },
        {"flv",                     MEDIA_MIMETYPE_CONTAINER_FLV      },
        {"swf",                     MEDIA_MIMETYPE_CONTAINER_FLV      },
        {"avi",                     MEDIA_MIMETYPE_CONTAINER_AVI      },
        {"ape",                     MEDIA_MIMETYPE_CONTAINER_APE      },
        {"dts",                     MEDIA_MIMETYPE_CONTAINER_DTS      },
        {"flac",                    MEDIA_MIMETYPE_CONTAINER_FLAC     },
        {"ac3",                     MEDIA_MIMETYPE_AUDIO_AC3          },
        {"wav",                     MEDIA_MIMETYPE_CONTAINER_WAV      },
        {"ogg",                     MEDIA_MIMETYPE_CONTAINER_OGG      },
        {"vc1",                     MEDIA_MIMETYPE_CONTAINER_VC1      },
        {"hevc",                    MEDIA_MIMETYPE_CONTAINER_HEVC     },
};

static enum AVCodecID getCodecId(AVFormatContext *ic, AVMediaType codec_type)
{
    unsigned int idx = 0;
    AVCodecContext *avctx = NULL;

    for (idx = 0; idx < ic->nb_streams; idx++) {
        avctx = ic->streams[idx]->codec;
        if (avctx->codec_type == codec_type) {
            return avctx->codec_id;
        }
    }

    return AV_CODEC_ID_NONE;
}

static bool hasAudioCodecOnly(AVFormatContext *ic)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;
    bool haveVideo = false;
    bool haveAudio = false;

    if (getCodecId(ic, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE) {
        haveVideo = true;
    }
    if (getCodecId(ic, AVMEDIA_TYPE_AUDIO) != AV_CODEC_ID_NONE) {
        haveAudio = true;
    }

    if (!haveVideo && haveAudio) {
        return true;
    }

    return false;
}

//FIXME all codecs: frameworks/av/media/libstagefright/codecs/*
static bool isCodecSupportedByStagefright(enum AVCodecID codec_id)
{
    bool supported = false;

    switch(codec_id) {
    //video
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_MPEG4:
    case AV_CODEC_ID_H263:
    case AV_CODEC_ID_H263P:
    case AV_CODEC_ID_H263I:
    case AV_CODEC_ID_VP6:
    case AV_CODEC_ID_VP8:
    //audio
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_AMR_NB:
    case AV_CODEC_ID_AMR_WB:
    case AV_CODEC_ID_FLAC:
    case AV_CODEC_ID_VORBIS:
    case AV_CODEC_ID_PCM_MULAW: //g711
    case AV_CODEC_ID_PCM_ALAW:  //g711
    //case AV_CODEC_ID_PCM_XXX: //FIXME more PCM?
        supported = true;
        break;

    default:
        break;
    }

    ALOGD("%ssuppoted codec(%s) by official Stagefright",
            (supported ? "" : "un"),
            avcodec_get_name(codec_id));

    return supported;
}

static void adjustMPEG4Confidence(AVFormatContext *ic, float *confidence)
{
    AVDictionary *tags = NULL;
    AVDictionaryEntry *tag = NULL;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    //1. check codec id
    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_H263
            && codec_id != AV_CODEC_ID_H263P
            && codec_id != AV_CODEC_ID_H263I) {
        //the MEDIA_MIMETYPE_CONTAINER_MPEG4 of confidence is 0.4f
        ALOGI("[mp4]video codec(%s), confidence should be larger than MPEG4Extractor",
                avcodec_get_name(codec_id));
        *confidence = 0.41f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_MP3
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_AMR_NB
            && codec_id != AV_CODEC_ID_AMR_WB) {
        ALOGI("[mp4]audio codec(%s), confidence should be larger than MPEG4Extractor",
                avcodec_get_name(codec_id));
        *confidence = 0.41f;
    }

    //2. check tag
    tags = ic->metadata;
    //NOTE: You can use command to show these tags,
    //e.g. "ffprobe -show_format 2012.mov"
    tag = av_dict_get(tags, "major_brand", NULL, 0);
    if (!tag) {
        return;
    }

    ALOGV("major_brand tag is:%s", tag->value);

    //when MEDIA_MIMETYPE_CONTAINER_MPEG4
    //WTF, MPEG4Extractor.cpp can not extractor mov format
    //NOTE: isCompatibleBrand(MPEG4Extractor.cpp)
    //  Won't promise that the following file types can be played.
    //  Just give these file types a chance.
    //  FOURCC('q', 't', ' ', ' '),  // Apple's QuickTime
    //So......
    if (!strcmp(tag->value, "qt  ")) {
        ALOGI("[mp4]format is mov, confidence should be larger than mpeg4");
        *confidence = 0.41f;
    }
}

static void adjustMPEG2TSConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_MPEG1VIDEO
            && codec_id != AV_CODEC_ID_MPEG2VIDEO) {
        //the MEDIA_MIMETYPE_CONTAINER_MPEG2TS of confidence is 0.1f
        ALOGI("[mpeg2ts]video codec(%s), confidence should be larger than MPEG2TSExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.11f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_PCM_S16LE //FIXME, AV_CODEC_ID_PCM_S24LE, AV_CODEC_ID_PCM_S32LE?
            && codec_id != AV_CODEC_ID_MP1
            && codec_id != AV_CODEC_ID_MP2
            && codec_id != AV_CODEC_ID_MP3) {
        ALOGI("[mpeg2ts]audio codec(%s), confidence should be larger than MPEG2TSExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.11f;
    }
}

static void adjustMKVConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_H264
            && codec_id != AV_CODEC_ID_MPEG4
            && codec_id != AV_CODEC_ID_VP6
            && codec_id != AV_CODEC_ID_VP8) {
        //the MEDIA_MIMETYPE_CONTAINER_MATROSKA of confidence is 0.6f
        ALOGI("[mkv]video codec(%s), confidence should be larger than MatroskaExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.61f;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE
            && codec_id != AV_CODEC_ID_AAC
            && codec_id != AV_CODEC_ID_MP3
            && codec_id != AV_CODEC_ID_VORBIS) {
        ALOGI("[mkv]audio codec(%s), confidence should be larger than MatroskaExtractor",
                avcodec_get_name(codec_id));
        *confidence = 0.61f;
    }
}

static void adjustCodecConfidence(AVFormatContext *ic, float *confidence)
{
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    codec_id = getCodecId(ic, AVMEDIA_TYPE_VIDEO);
    if (codec_id != AV_CODEC_ID_NONE) {
        if (!isCodecSupportedByStagefright(codec_id)) {
            *confidence = 0.88f;
        }
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    if (codec_id != AV_CODEC_ID_NONE) {
        if (!isCodecSupportedByStagefright(codec_id)) {
            *confidence = 0.88f;
        }
    }

    if (getCodecId(ic, AVMEDIA_TYPE_VIDEO) != AV_CODEC_ID_NONE
            && getCodecId(ic, AVMEDIA_TYPE_AUDIO) == AV_CODEC_ID_MP3) {
        *confidence = 0.22f; //larger than MP3Extractor
    }
}

//TODO need more checks
static void adjustConfidenceIfNeeded(const char *mime,
        AVFormatContext *ic, float *confidence)
{
    //1. check mime
    if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)) {
        adjustMPEG4Confidence(ic, confidence);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2TS)) {
        adjustMPEG2TSConfidence(ic, confidence);
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MATROSKA)) {
        adjustMKVConfidence(ic, confidence);
    } else {
        //todo here
    }

    if (*confidence > 0.08) {
        return;
    }

    //2. check codec
    adjustCodecConfidence(ic, confidence);
}

static void adjustContainerIfNeeded(const char **mime, AVFormatContext *ic)
{
    const char *newMime = *mime;
    enum AVCodecID codec_id = AV_CODEC_ID_NONE;

    if (!hasAudioCodecOnly(ic)) {
        return;
    }

    codec_id = getCodecId(ic, AVMEDIA_TYPE_AUDIO);
    CHECK(codec_id != AV_CODEC_ID_NONE);
    switch (codec_id) {
    case AV_CODEC_ID_MP3:
        newMime = MEDIA_MIMETYPE_AUDIO_MPEG;
        break;
    case AV_CODEC_ID_AAC:
        newMime = MEDIA_MIMETYPE_AUDIO_AAC;
        break;
    case AV_CODEC_ID_VORBIS:
        newMime = MEDIA_MIMETYPE_AUDIO_VORBIS;
        break;
    case AV_CODEC_ID_FLAC:
        newMime = MEDIA_MIMETYPE_AUDIO_FLAC;
        break;
    case AV_CODEC_ID_AC3:
        newMime = MEDIA_MIMETYPE_AUDIO_AC3;
        break;
    case AV_CODEC_ID_APE:
        newMime = MEDIA_MIMETYPE_AUDIO_APE;
        break;
    case AV_CODEC_ID_DTS:
        newMime = MEDIA_MIMETYPE_AUDIO_DTS;
        break;
    case AV_CODEC_ID_MP2:
        newMime = MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II;
        break;
    case AV_CODEC_ID_COOK:
        newMime = MEDIA_MIMETYPE_AUDIO_RA;
        break;
    case AV_CODEC_ID_WMAV1:
    case AV_CODEC_ID_WMAV2:
    case AV_CODEC_ID_WMAPRO:
    case AV_CODEC_ID_WMALOSSLESS:
        newMime = MEDIA_MIMETYPE_AUDIO_WMA;
        break;
    default:
        break;
    }

    if (!strcmp(*mime, MEDIA_MIMETYPE_CONTAINER_FFMPEG)) {
        newMime = MEDIA_MIMETYPE_AUDIO_FFMPEG;
    }

    if (strcmp(*mime, newMime)) {
        ALOGI("adjust mime(%s -> %s)", *mime, newMime);
        *mime = newMime;
    }
}

static const char *findMatchingContainer(const char *name)
{
    size_t i = 0;
#if SUPPOURT_UNKNOWN_FORMAT
    //The FFmpegExtractor support all ffmpeg formats!!!
    //Unknown format is defined as MEDIA_MIMETYPE_CONTAINER_FFMPEG
    const char *container = MEDIA_MIMETYPE_CONTAINER_FFMPEG;
#else
    const char *container = NULL;
#endif

    ALOGV("list the formats suppoted by ffmpeg: ");
    ALOGV("========================================");
    for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
        ALOGV("format_names[%02d]: %s", i, FILE_FORMATS[i].format);
    }
    ALOGV("========================================");

    for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
        int len = strlen(FILE_FORMATS[i].format);
        if (!strncasecmp(name, FILE_FORMATS[i].format, len)) {
            container = FILE_FORMATS[i].container;
            break;
        }
    }

    return container;
}

static const char *SniffFFMPEGCommon(const char *url, float *confidence)
{
    int err = 0;
    size_t i = 0;
    size_t nb_streams = 0;
    const char *container = NULL;
    AVFormatContext *ic = NULL;
    AVDictionary **opts = NULL;

    status_t status = initFFmpeg();
    if (status != OK) {
        ALOGE("could not init ffmpeg");
        return NULL;
    }

    ic = avformat_alloc_context();
    if (!ic)
    {
        ALOGE("oom for alloc avformat context");
        goto fail;
    }

    err = avformat_open_input(&ic, url, NULL, NULL);
    if (err < 0) {
        ALOGE("%s: avformat_open_input failed, err:%s", url, av_err2str(err));
        goto fail;
    }

    opts = setup_find_stream_info_opts(ic, codec_opts);
    nb_streams = ic->nb_streams;
    err = avformat_find_stream_info(ic, opts);
    if (err < 0) {
        ALOGE("%s: could not find stream info, err:%s", url, av_err2str(err));
        goto fail;
    }
    for (i = 0; i < nb_streams; i++) {
        av_dict_free(&opts[i]);
    }
    av_freep(&opts);

    av_dump_format(ic, 0, url, 0);

    ALOGD("FFmpegExtrator, url: %s, format_name: %s, format_long_name: %s",
            url, ic->iformat->name, ic->iformat->long_name);

    container = findMatchingContainer(ic->iformat->name);
    if (container) {
        adjustContainerIfNeeded(&container, ic);
        adjustConfidenceIfNeeded(container, ic, confidence);
    }

fail:
    if (ic) {
        avformat_close_input(&ic);
    }
    if (status == OK) {
        deInitFFmpeg();
    }

    return container;
}

static const char *BetterSniffFFMPEG(const sp<DataSource> &source,
        float *confidence, sp<AMessage> meta)
{
    const char *ret = NULL;
    char url[PATH_MAX] = {0};

    ALOGI("android-source:%p", source.get());

    // pass the addr of smart pointer("source")
    snprintf(url, sizeof(url), "android-source:%p", source.get());

    ret = SniffFFMPEGCommon(url, confidence);
    if (ret) {
        meta->setString("extended-extractor-url", url);
    }

    return ret;
}

static const char *LegacySniffFFMPEG(const sp<DataSource> &source,
         float *confidence, sp<AMessage> meta)
{
    const char *ret = NULL;
    char url[PATH_MAX] = {0};

    String8 uri = source->getUri();
    if (!uri.string()) {
        return NULL;
    }

    ALOGV("source url:%s", uri.string());

    // pass the addr of smart pointer("source") + file name
    snprintf(url, sizeof(url), "android-source:%p|file:%s", source.get(), uri.string());

    ret = SniffFFMPEGCommon(url, confidence);
    if (ret) {
        meta->setString("extended-extractor-url", url);
    }

    return ret;
}

bool SniffFFMPEG(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    ALOGV("SniffFFMPEG");

    *meta = new AMessage;
    *confidence = 0.08f;  // be the last resort, by default

    const char *container = BetterSniffFFMPEG(source, confidence, *meta);
    if (!container) {
        ALOGW("sniff through BetterSniffFFMPEG failed, try LegacySniffFFMPEG");
        container = LegacySniffFFMPEG(source, confidence, *meta);
        if (container) {
            ALOGV("sniff through LegacySniffFFMPEG success");
        }
    } else {
        ALOGV("sniff through BetterSniffFFMPEG success");
    }

    if (container == NULL) {
        ALOGD("SniffFFMPEG failed to sniff this source");
        (*meta)->clear();
        *meta = NULL;
        return false;
    }

    ALOGD("ffmpeg detected media content as '%s' with confidence %.2f",
            container, *confidence);

    /* use MPEG4Extractor(not extended extractor) for HTTP source only */
    if (!strcasecmp(container, MEDIA_MIMETYPE_CONTAINER_MPEG4)
            && (source->flags() & DataSource::kIsCachingDataSource)) {
        ALOGI("support container: %s, but it is caching data source, "
                "Don't use ffmpegextractor", container);
        (*meta)->clear();
        *meta = NULL;
        return false;
    }

    mimeType->setTo(container);

    (*meta)->setString("extended-extractor", "extended-extractor");
    (*meta)->setString("extended-extractor-subtype", "ffmpegextractor");
    (*meta)->setString("extended-extractor-mime", container);

    //debug only
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.media.parser.ffmpeg", value, "0");
    if (atoi(value)) {
        ALOGD("[debug] use ffmpeg parser");
        *confidence = 0.88f;
    }

    if (*confidence > 0.08f) {
        (*meta)->setString("extended-extractor-use", "ffmpegextractor");
    }

    return true;
}

MediaExtractor *CreateFFmpegExtractor(const sp<DataSource> &source, const char *mime, const sp<AMessage> &meta) {
    MediaExtractor *ret = NULL;
    AString notuse;
    if (meta.get() && meta->findString("extended-extractor", &notuse) && (
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)          ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AAC)           ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)        ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FLAC)          ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_AC3)           ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_APE)           ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_DTS)           ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II) ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RA)            ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_WMA)           ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_FFMPEG)        ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)     ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MOV)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MATROSKA)  ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_TS)        ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG2PS)   ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_AVI)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_ASF)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WEBM)      ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WMV)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPG)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_FLV)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_DIVX)      ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_RM)        ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WAV)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_FLAC)      ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_APE)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_DTS)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MP2)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_RA)        ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_OGG)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_VC1)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_HEVC)      ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WMA)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_FFMPEG))) {
        ret = new FFmpegExtractor(source, meta);
    }

    ALOGD("%ssupported mime: %s", (ret ? "" : "un"), mime);
    return ret;
}

}  // namespace android

extern "C" void getExtractorPlugin(android::MediaExtractor::Plugin *plugin)
{
    plugin->sniff = android::SniffFFMPEG;
    plugin->create = android::CreateFFmpegExtractor;
}
