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
#define LOG_TAG "FFmpegExtractor"
#include <utils/Log.h>

#include <stdint.h>
#include <limits.h> /* INT_MAX */
#include <inttypes.h>

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
#include <cutils/properties.h>
#include <utils/String8.h>
#include <utils/misc.h>

#include "include/avc_utils.h"
#include "utils/ffmpeg_utils.h"
#include "utils/ffmpeg_cmdutils.h"
#include "FFmpegExtractor.h"

#define DEBUG_READ_ENTRY           0
#define DEBUG_DISABLE_VIDEO        0
#define DEBUG_DISABLE_AUDIO        0
#define WAIT_KEY_PACKET_AFTER_SEEK 1
#define DISABLE_NAL_TO_ANNEXB      0

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
#define MIN_AUDIOQ_SIZE (20 * 16 * 1024)
#define MIN_FRAMES 5
#define EXTRACTOR_MAX_PROBE_PACKETS 200

#define FF_MAX_EXTRADATA_SIZE ((1 << 28) - FF_INPUT_BUFFER_PADDING_SIZE)

enum {
    NO_SEEK = 0,
    SEEK,
};

static AVPacket flush_pkt;

namespace android {

struct FFmpegExtractor::Track : public MediaSource {
    Track(const sp<FFmpegExtractor> &extractor, sp<MetaData> meta, bool isAVC,
          AVStream *stream, PacketQueue *queue);

    virtual status_t start(MetaData *params);
    virtual status_t stop();
    virtual sp<MetaData> getFormat();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options);

protected:
    virtual ~Track();

private:
    friend struct FFmpegExtractor;

    sp<FFmpegExtractor> mExtractor;
    sp<MetaData> mMeta;

    enum AVMediaType mMediaType;

    mutable Mutex mLock;

    bool mIsAVC;
    size_t mNALLengthSize;
    bool mNal2AnnexB;

    AVStream *mStream;
    PacketQueue *mQueue;

    int64_t mFirstKeyPktTimestamp;

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

////////////////////////////////////////////////////////////////////////////////

FFmpegExtractor::FFmpegExtractor(const sp<DataSource> &source)
    : mDataSource(source),
      mInitCheck(NO_INIT),
      mReaderThreadStarted(false) {
    ALOGV("FFmpegExtractor::FFmpegExtractor");

    buildFileName(source);

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

    return mTracks.valueAt(index);
}

sp<MetaData> FFmpegExtractor::getTrackMetaData(size_t index, uint32_t flags) {
    ALOGV("FFmpegExtractor::getTrackMetaData[%d]", index);

    if (mInitCheck != OK) {
        return NULL;
    }

    if (index >= mTracks.size()) {
        return NULL;
    }

    return mTracks.valueAt(index)->getFormat();
}

sp<MetaData> FFmpegExtractor::getMetaData() {
    ALOGV("FFmpegExtractor::getMetaData");

    if (mInitCheck != OK) {
        return NULL;
    }

    sp<MetaData> meta = new MetaData;
    // TODO
    meta->setCString(kKeyMIMEType, "video/ffmpeg");

    return meta;
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

void FFmpegExtractor::packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    packet_queue_put(q, &flush_pkt);
}

void FFmpegExtractor::packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->mutex);
    for (pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
        pkt1 = pkt->next;
        av_free_packet(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    pthread_mutex_unlock(&q->mutex);
}

void FFmpegExtractor::packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
}

void FFmpegExtractor::packet_queue_abort(PacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);

    q->abort_request = 1;

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
}

int FFmpegExtractor::packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    /* duplicate the packet */
    if (pkt != &flush_pkt && av_dup_packet(pkt) < 0)
        return -1;

    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    pkt1->pkt = *pkt;
    pkt1->next = NULL;

    pthread_mutex_lock(&q->mutex);

    if (!q->last_pkt)

        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    //q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->size += pkt1->pkt.size;
    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
    return 0;
}

/* packet queue handling */
/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int FFmpegExtractor::packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            //q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->size -= pkt1->pkt.size;
            *pkt = pkt1->pkt;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }
    pthread_mutex_unlock(&q->mutex);
    return ret;
}

static void EncodeSize14(uint8_t **_ptr, size_t size) {
    CHECK_LE(size, 0x3fff);

    uint8_t *ptr = *_ptr;

    *ptr++ = 0x80 | (size >> 7);
    *ptr++ = size & 0x7f;

    *_ptr = ptr;
}

static sp<ABuffer> MakeMPEGVideoESDS(const sp<ABuffer> &csd) {
    sp<ABuffer> esds = new ABuffer(csd->size() + 25);

    uint8_t *ptr = esds->data();
    *ptr++ = 0x03;
    EncodeSize14(&ptr, 22 + csd->size());

    *ptr++ = 0x00;  // ES_ID
    *ptr++ = 0x00;

    *ptr++ = 0x00;  // streamDependenceFlag, URL_Flag, OCRstreamFlag

    *ptr++ = 0x04;
    EncodeSize14(&ptr, 16 + csd->size());

    *ptr++ = 0x40;  // Audio ISO/IEC 14496-3

    for (size_t i = 0; i < 12; ++i) {
        *ptr++ = 0x00;
    }

    *ptr++ = 0x05;
    EncodeSize14(&ptr, csd->size());

    memcpy(ptr, csd->data(), csd->size());

    return esds;
}

// Returns the sample rate based on the sampling frequency index
static uint32_t get_sample_rate(const uint8_t sf_index)
{
    static const uint32_t sample_rates[] =
    {
        96000, 88200, 64000, 48000, 44100, 32000,
        24000, 22050, 16000, 12000, 11025, 8000
    };

    if (sf_index < sizeof(sample_rates) / sizeof(sample_rates[0])) {
        return sample_rates[sf_index];
    }

    return 0;
}

int FFmpegExtractor::check_extradata(AVCodecContext *avctx)
{
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

    // ignore extradata
    if (avctx->codec_id == CODEC_ID_MP3 ||
            avctx->codec_id == CODEC_ID_MP1  ||
            avctx->codec_id == CODEC_ID_MP2  ||
            avctx->codec_id == CODEC_ID_AC3  ||
            avctx->codec_id == CODEC_ID_DTS  ||
            avctx->codec_id == CODEC_ID_H263  ||
            avctx->codec_id == CODEC_ID_H263P ||
            avctx->codec_id == CODEC_ID_H263I ||
            avctx->codec_id == CODEC_ID_WMV1)
        return 1;

    // is extradata compatible with android?
    if (avctx->codec_id != CODEC_ID_AAC) {
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

    if (avctx->codec_id == CODEC_ID_AAC) {
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

int FFmpegExtractor::stream_component_open(int stream_index)
{
    AVCodecContext *avctx = NULL;
    sp<MetaData> meta = NULL;
    bool isAVC = false;
    bool supported = false;
    uint32_t type = 0;
    const void *data = NULL;
    size_t size = 0;
    int ret = 0;

    ALOGI("stream_index: %d", stream_index);
    if (stream_index < 0 || stream_index >= (int)mFormatCtx->nb_streams)
        return -1;
    avctx = mFormatCtx->streams[stream_index]->codec;

    switch(avctx->codec_id) {
    case CODEC_ID_H264:
    case CODEC_ID_MPEG4:
    case CODEC_ID_H263:
    case CODEC_ID_H263P:
    case CODEC_ID_H263I:
    case CODEC_ID_AAC:
    case CODEC_ID_AC3:
    case CODEC_ID_MP1:
    case CODEC_ID_MP2:
    case CODEC_ID_MP3:
    case CODEC_ID_MPEG2VIDEO:
    case CODEC_ID_WMV1:
    case CODEC_ID_WMV2:
    case CODEC_ID_WMV3:
    case CODEC_ID_VC1:
    case CODEC_ID_WMAV1:
    case CODEC_ID_WMAV2:
    case CODEC_ID_WMAPRO:
    case CODEC_ID_WMALOSSLESS:
    case CODEC_ID_RV40:
    case CODEC_ID_COOK:
    case CODEC_ID_APE:
    case CODEC_ID_DTS:
    case CODEC_ID_FLAC:
        supported = true;
        break;
    default:
        supported = false;
        break;
    }

    if (!supported) {
        ALOGE("unsupport the codec, id: 0x%0x", avctx->codec_id);
        return -1;
    }
    ALOGV("support the codec");

    unsigned streamType;
    ssize_t index = mTracks.indexOfKey(stream_index);

    if (index >= 0) {
        ALOGE("this track already exists");
        return 0;
    }

    mFormatCtx->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    char tagbuf[32];
    av_get_codec_tag_string(tagbuf, sizeof(tagbuf), avctx->codec_tag);
    ALOGV("Tag %s/0x%08x with codec id '%d'\n", tagbuf, avctx->codec_tag, avctx->codec_id);

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        if (mVideoStreamIdx == -1)
            mVideoStreamIdx = stream_index;
        if (mVideoStream == NULL)
            mVideoStream = mFormatCtx->streams[stream_index];
        if (!mVideoQInited) {
	    packet_queue_init(&mVideoQ);
            mVideoQInited = true;
        }

        ret = check_extradata(avctx);
        if (ret != 1) {
            if (ret == -1) {
                // disable the stream
                mVideoStreamIdx = -1;
                mVideoStream = NULL;
                packet_queue_end(&mVideoQ);
                mVideoQInited =  false;
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

        meta = new MetaData;

        switch(avctx->codec_id) {
        case CODEC_ID_H264:
            /**
             * H.264 Video Types
             * http://msdn.microsoft.com/en-us/library/dd757808(v=vs.85).aspx
             */
            //if (avctx->codec_tag && avctx->codec_tag == AV_RL32("avc1")) {
            if (avctx->extradata[0] == 1 /* configurationVersion */) {
                // H.264 bitstream without start codes.
                isAVC = true;
                ALOGV("AVC");

                if (avctx->width == 0 || avctx->height == 0) {
                    int32_t width, height;
                    sp<ABuffer> seqParamSet = new ABuffer(avctx->extradata_size - 8);
                    memcpy(seqParamSet->data(), avctx->extradata + 8, avctx->extradata_size - 8);
                    FindAVCDimensions(seqParamSet, &width, &height);
                    avctx->width  = width;
                    avctx->height = height;
                }

                meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
                meta->setData(kKeyAVCC, kTypeAVCC, avctx->extradata, avctx->extradata_size);
            } else {
                // H.264 bitstream with start codes.
                isAVC = false;
                ALOGV("H264");

                /* set NULL to release meta as we will new a meta in MakeAVCCodecSpecificData() fxn */
                meta->clear();
                meta = NULL;

                sp<ABuffer> buffer = new ABuffer(avctx->extradata_size);
                memcpy(buffer->data(), avctx->extradata, avctx->extradata_size);
                meta = MakeAVCCodecSpecificData(buffer);
            }
            break;
        case CODEC_ID_MPEG4:
            ALOGV("MPEG4");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);
            {
                sp<ABuffer> csd = new ABuffer(avctx->extradata_size);
                memcpy(csd->data(), avctx->extradata, avctx->extradata_size);
                sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                meta->setData(kKeyESDS, kTypeESDS, esds->data(), esds->size());
            }
            break;
        case CODEC_ID_H263:
        case CODEC_ID_H263P:
        case CODEC_ID_H263I:
            ALOGV("H263");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
            break;
        case CODEC_ID_MPEG2VIDEO:
            ALOGV("MPEG2VIDEO");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);
            {
                sp<ABuffer> csd = new ABuffer(avctx->extradata_size);
                memcpy(csd->data(), avctx->extradata, avctx->extradata_size);
                sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                meta->setData(kKeyESDS, kTypeESDS, esds->data(), esds->size());
            }
            break;
        case CODEC_ID_VC1:
            ALOGV("VC1");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_VC1);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        case CODEC_ID_WMV1:
            ALOGV("WMV1");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setInt32(kKeyWMVVersion, kTypeWMVVer_7);
            break;
        case CODEC_ID_WMV2:
            ALOGV("WMV2");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMVVersion, kTypeWMVVer_8);
            break;
        case CODEC_ID_WMV3:
            ALOGV("WMV3");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMVVersion, kTypeWMVVer_9);
            break;
        case CODEC_ID_RV40:
            ALOGV("RV40");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RV);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        default:
            CHECK(!"Should not be here. Unsupported codec.");
            break;
        }

        ALOGI("width: %d, height: %d, bit_rate: %d",
             avctx->width, avctx->height, avctx->bit_rate);

        meta->setInt32(kKeyWidth, avctx->width);
        meta->setInt32(kKeyHeight, avctx->height);
        if (avctx->bit_rate > 0)
            meta->setInt32(kKeyBitRate, avctx->bit_rate);
        if (mVideoStream->duration != AV_NOPTS_VALUE) {
            int64_t duration = mVideoStream->duration * av_q2d(mVideoStream->time_base) * 1000000;
            printTime(duration);
            ALOGV("video startTime: %lld", mVideoStream->start_time);
            meta->setInt64(kKeyDuration, duration);
        } else {
            // default when no stream duration
            meta->setInt64(kKeyDuration, mFormatCtx->duration);
        }

        ALOGV("create a video track");
        index = mTracks.add(
            stream_index, new Track(this, meta, isAVC, mVideoStream, &mVideoQ));

        mDefersToCreateVideoTrack = false;

        break;
    case AVMEDIA_TYPE_AUDIO:
        if (mAudioStreamIdx == -1)
            mAudioStreamIdx = stream_index;
        if (mAudioStream == NULL)
            mAudioStream = mFormatCtx->streams[stream_index];
        if (!mAudioQInited) {
	    packet_queue_init(&mAudioQ);
            mAudioQInited = true;
        }

        ret = check_extradata(avctx);
        if (ret != 1) {
            if (ret == -1) {
                // disable the stream
                mAudioStreamIdx = -1;
                mAudioStream = NULL;
                packet_queue_end(&mAudioQ);
                mAudioQInited =  false;
                mFormatCtx->streams[stream_index]->discard = AVDISCARD_ALL;
            }
            return ret;
        }

        if (avctx->extradata) {
            ALOGV("audio stream extradata:");
            hexdump(avctx->extradata, avctx->extradata_size);
        } else {
            ALOGV("audio stream no extradata, but we can ignore it.");
        }

        switch(avctx->codec_id) {
        case CODEC_ID_MP1:
            ALOGV("MP1");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
            break;
        case CODEC_ID_MP2:
            ALOGV("MP2");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
            break;
        case CODEC_ID_MP3:
            ALOGV("MP3");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
            break;
        case CODEC_ID_AC3:
            ALOGV("AC3");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);
            break;
        case CODEC_ID_AAC:
            ALOGV("AAC");
            uint32_t sr;
            const uint8_t *header;
            uint8_t profile, sf_index, channel;

            header = avctx->extradata;
            CHECK(header != NULL);

            // AudioSpecificInfo follows
            // oooo offf fccc c000
            // o - audioObjectType
            // f - samplingFreqIndex
            // c - channelConfig
            profile = ((header[0] & 0xf8) >> 3) - 1;
            sf_index = (header[0] & 0x07) << 1 | (header[1] & 0x80) >> 7;
            sr = get_sample_rate(sf_index);
            if (sr == 0) {
                ALOGE("unsupport the sample rate");
                return -1;
            }
            channel = (header[1] >> 3) & 0xf;
            ALOGV("profile: %d, sf_index: %d, channel: %d", profile, sf_index, channel);

            meta = MakeAACCodecSpecificData(profile, sf_index, channel);
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
            break;
        case CODEC_ID_WMAV1:  // TODO, version?
            ALOGV("WMAV1");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        case CODEC_ID_WMAV2:
            ALOGV("WMAV2");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMAVersion, kTypeWMA);
            break;
        case CODEC_ID_WMAPRO:
            ALOGV("WMAPRO");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMAVersion, kTypeWMAPro);
            break;
        case CODEC_ID_WMALOSSLESS:
            ALOGV("WMALOSSLESS");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMAVersion, kTypeWMALossLess);
            break;
        case CODEC_ID_COOK: // audio codec in RMVB
            ALOGV("COOK");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        case CODEC_ID_APE:
            ALOGV("APE");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_APE);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        case CODEC_ID_DTS:
            ALOGV("DTS");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_DTS);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        case CODEC_ID_FLAC:
            ALOGV("FLAC");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_FLAC);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        default:
            CHECK(!"Should not be here. Unsupported codec.");
            break;
        }

        ALOGI("bit_rate: %d, sample_rate: %d, channels: %d, bits_per_coded_sample: %d",
             avctx->bit_rate, avctx->sample_rate, avctx->channels, avctx->bits_per_coded_sample);

        meta->setInt32(kKeySampleRate, avctx->sample_rate);
        meta->setInt32(kKeyChannelCount, avctx->channels);
        meta->setInt32(kKeyBitspersample, avctx->bits_per_coded_sample);
        meta->setInt32(kKeyBitRate, avctx->bit_rate);
        meta->setInt32(kKeyBlockAlign, avctx->block_align);
        if (mAudioStream->duration != AV_NOPTS_VALUE) {
            int64_t duration = mAudioStream->duration * av_q2d(mAudioStream->time_base) * 1000000;
            printTime(duration);
            ALOGV("audio startTime: %lld", mAudioStream->start_time);
            meta->setInt64(kKeyDuration, duration);
        } else {
            // default when no stream duration
            meta->setInt64(kKeyDuration, mFormatCtx->duration);
        }

        ALOGV("create a audio track");
        index = mTracks.add(
            stream_index, new Track(this, meta, false, mAudioStream, &mAudioQ));

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

    if (mVideoStreamIdx >= 0 &&
        mAudioStreamIdx >= 0 &&
        media_type == AVMEDIA_TYPE_AUDIO &&
        !mVideoEOSReceived) {
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

void FFmpegExtractor::print_error_ex(const char *filename, int err)
{
    char errbuf[128];
    const char *errbuf_ptr = errbuf;

    if (av_strerror(err, errbuf, sizeof(errbuf)) < 0)
        errbuf_ptr = strerror(AVUNERROR(err));
    ALOGI("%s: %s\n", filename, errbuf_ptr);
}

void FFmpegExtractor::buildFileName(const sp<DataSource> &source)
{
#if 1
    ALOGI("android-source:%p", &source);
    // pass the addr of smart pointer("source")
    snprintf(mFilename, sizeof(mFilename), "android-source:%p", &source);
    ALOGI("build mFilename: %s", mFilename);
#else
    const char *url = mDataSource->getNamURI();
    if (url == NULL) {
        ALOGI("url is error!");
        return;
    }
    // is it right?
    if (!strcmp(url, "-")) {
        av_strlcpy(mFilename, "pipe:", strlen("pipe:") + 1);
    } else {
        av_strlcpy(mFilename, url, strlen(url) + 1);
    }
    ALOGI("build url: %s, mFilename: %s", url, mFilename);
#endif
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
    mVideoQInited = false;
    mAudioQInited = false;
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
    int ret = 0, audio_ret = 0, video_ret = 0;
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

    setFFmpegDefaultOpts();

    status = initFFmpeg();
    if (status != OK) {
        ret = -1;
        goto fail;
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)"FLUSH";
    flush_pkt.size = 0;

    mFormatCtx = avformat_alloc_context();
	if (!mFormatCtx)
	{
        ALOGE("oom for alloc avformat context");
        ret -1;
		goto fail;
	}
    mFormatCtx->interrupt_callback.callback = decode_interrupt_cb;
    mFormatCtx->interrupt_callback.opaque = this;
    ALOGV("mFilename: %s", mFilename);
    err = avformat_open_input(&mFormatCtx, mFilename, NULL, &format_opts);
    if (err < 0) {
        print_error_ex(mFilename, err);
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
        ALOGE("%s: could not find codec parameters\n", mFilename);
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
    if (mFormatCtx) {
        avformat_close_input(&mFormatCtx);
    }

    deInitFFmpeg();
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

    ALOGV("FFmpegExtractor::readerEntry");

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
                    packet_queue_put(&mAudioQ, &flush_pkt);
                }
                if (mVideoStreamIdx >= 0) {
                    packet_queue_flush(&mVideoQ);
                    packet_queue_put(&mVideoQ, &flush_pkt);
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
            ALOGV("readerEntry, is full, fuck");
#endif
            /* wait 10 ms */
            usleep(10000);
            continue;
        }

        if (eof) {
            if (mVideoStreamIdx >= 0) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = mVideoStreamIdx;
                packet_queue_put(&mVideoQ, pkt);
            }
            if (mAudioStreamIdx >= 0) {
                av_init_packet(pkt);
                pkt->data = NULL;
                pkt->size = 0;
                pkt->stream_index = mAudioStreamIdx;
                packet_queue_put(&mAudioQ, pkt);
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
}

////////////////////////////////////////////////////////////////////////////////

FFmpegExtractor::Track::Track(
        const sp<FFmpegExtractor> &extractor, sp<MetaData> meta, bool isAVC,
          AVStream *stream, PacketQueue *queue)
    : mExtractor(extractor),
      mMeta(meta),
      mIsAVC(isAVC),
      mStream(stream),
      mQueue(queue) {
    const char *mime;

    /* H.264 Video Types */
    {
        mNal2AnnexB = false;

        if (mIsAVC) {
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

FFmpegExtractor::Track::~Track() {
}

status_t FFmpegExtractor::Track::start(MetaData *params) {
    Mutex::Autolock autoLock(mLock);
    //mExtractor->startReaderThread();
    return OK;
}

status_t FFmpegExtractor::Track::stop() {
    Mutex::Autolock autoLock(mLock);
    mExtractor->stopReaderThread();
    return OK;
}

sp<MetaData> FFmpegExtractor::Track::getFormat() {
    Mutex::Autolock autoLock(mLock);

    return mMeta;
}

status_t FFmpegExtractor::Track::read(
        MediaBuffer **buffer, const ReadOptions *options) {
    *buffer = NULL;

    Mutex::Autolock autoLock(mLock);

    AVPacket pkt;
    bool seeking = false;
    bool waitKeyPkt = false;
    ReadOptions::SeekMode mode;
    int64_t pktTS = AV_NOPTS_VALUE;
    int64_t seekTimeUs = AV_NOPTS_VALUE;
    int64_t timeUs;
    int key;
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
    if (mExtractor->packet_queue_get(mQueue, &pkt, 1) < 0) {
        mExtractor->reachedEOS(mMediaType);
        return ERROR_END_OF_STREAM;
    }

    if (seeking) {
        if (pkt.data != flush_pkt.data) {
            av_free_packet(&pkt);
            goto retry;
        } else {
            seeking = false;
#if WAIT_KEY_PACKET_AFTER_SEEK
            waitKeyPkt = true;
#endif
        }
    }

    if (pkt.data == flush_pkt.data) {
        ALOGV("read %s flush pkt", av_get_media_type_string(mMediaType));
        av_free_packet(&pkt);
        mFirstKeyPktTimestamp = AV_NOPTS_VALUE;
        goto retry;
    } else if (pkt.data == NULL && pkt.size == 0) {
        ALOGV("read %s eos pkt", av_get_media_type_string(mMediaType));
        av_free_packet(&pkt);
        mExtractor->reachedEOS(mMediaType);
	return ERROR_END_OF_STREAM;
    }

    key = pkt.flags & AV_PKT_FLAG_KEY ? 1 : 0;
    pktTS = pkt.pts;
    // use dts when AVI
    if (pkt.pts == AV_NOPTS_VALUE)
        pktTS = pkt.dts;

    if (waitKeyPkt) {
        if (!key) {
            ALOGV("drop the no key packet");
            av_free_packet(&pkt);
            goto retry;
        } else {
            ALOGV("~~~~~~ got the key packet");
            waitKeyPkt = false;
        }
    }

    if (mFirstKeyPktTimestamp == AV_NOPTS_VALUE) {
        // update the first key timestamp
        mFirstKeyPktTimestamp = pktTS;
    }
     
    if (pktTS < mFirstKeyPktTimestamp) {
            ALOGV("drop the packet with the backward timestamp, maybe they are B-frames after I-frame ^_^");
            av_free_packet(&pkt);
            goto retry;
    }

    MediaBuffer *mediaBuffer = new MediaBuffer(pkt.size + FF_INPUT_BUFFER_PADDING_SIZE);
    mediaBuffer->meta_data()->clear();
    mediaBuffer->set_range(0, pkt.size);
#if DISABLE_NAL_TO_ANNEXB
    mNal2AnnexB = false;
#endif
    if (mIsAVC && mNal2AnnexB) {
        /* Convert H.264 NAL format to annex b */
        if (mNALLengthSize >= 3 && mNALLengthSize <= 4 )
        {
            uint8_t *dst = (uint8_t *)mediaBuffer->data();

            /* This only works for NAL sizes 3-4 */
            size_t len = pkt.size, i;
            uint8_t *ptr = pkt.data;
            while (len >= mNALLengthSize) {
                uint32_t nal_len = 0;
                for( i = 0; i < mNALLengthSize; i++ ) {
                    nal_len = (nal_len << 8) | ptr[i];
                    dst[i] = 0;
                }
                dst[mNALLengthSize - 1] = 1;
                if (nal_len > INT_MAX || nal_len > (unsigned int)len) {
                    status = ERROR_MALFORMED;
                    break;
                }
				dst += mNALLengthSize;
				ptr += mNALLengthSize;
                len -= mNALLengthSize;

                memcpy(dst, ptr, nal_len);

                dst += nal_len;
                ptr += nal_len;
                len -= nal_len;
            }
        } else {
             status = ERROR_MALFORMED;
        }

        if (status != OK) {
            ALOGV("status != OK");
            mediaBuffer->release();
            mediaBuffer = NULL;
            av_free_packet(&pkt);
            return ERROR_MALFORMED;
        }
    } else {
        memcpy(mediaBuffer->data(), pkt.data, pkt.size);
    }

    int64_t start_time = mStream->start_time != AV_NOPTS_VALUE ? mStream->start_time : 0;
    timeUs = (int64_t)((pktTS - start_time) * av_q2d(mStream->time_base) * 1000000);

#if 0
    ALOGV("read %s pkt, size: %d, key: %d, pts: %lld, dts: %lld, timeUs[-startTime]: %llu us (%.2f secs)",
        av_get_media_type_string(mMediaType), pkt.size, key, pkt.pts, pkt.dts, timeUs, timeUs/1E6);
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
        {"avi",                     MEDIA_MIMETYPE_CONTAINER_AVI      },
        {"ape",                     MEDIA_MIMETYPE_CONTAINER_APE      },
        {"dts",                     MEDIA_MIMETYPE_CONTAINER_DTS      },
        {"flac",                    MEDIA_MIMETYPE_CONTAINER_FLAC     },
#if 0
        {"wav",                     MEDIA_MIMETYPE_CONTAINER_WAV      },
#endif
};

static void adjustMPEG4Confidence(AVFormatContext *ic, float *confidence)
{
	AVDictionary *tags = NULL;
	AVDictionaryEntry *tag = NULL;

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
		ALOGI("format is mov, confidence should be larger than mpeg4");
		//the MEDIA_MIMETYPE_CONTAINER_MPEG4 of confidence is 0.4f
		*confidence = 0.41f;
	}
}

static void adjustVideoCodecConfidence(AVFormatContext *ic,
		enum AVCodecID codec_id, float *confidence)
{
	//add to here
}

static void adjustAudioCodecConfidence(AVFormatContext *ic,
		enum AVCodecID codec_id, float *confidence)
{
	switch (codec_id) {
	case CODEC_ID_AC3:
		//TODO. if the other stream(e.g. mp3) is supported by stagefright
		ALOGI("ffmpeg can demux ac3 only");
		*confidence = 0.88f;
	default:
		break;
	}
}

static void adjustCodecConfidence(AVFormatContext *ic, float *confidence)
{
	unsigned int idx = 0;
	AVCodecContext *avctx = NULL;
	AVMediaType	codec_type = AVMEDIA_TYPE_UNKNOWN;
	enum AVCodecID codec_id = AV_CODEC_ID_NONE;

	for (idx = 0; idx < ic->nb_streams; idx++)
	{
		avctx = ic->streams[idx]->codec;
		codec_type = avctx->codec_type;
		codec_id = avctx->codec_id;

		if (codec_type == AVMEDIA_TYPE_VIDEO) {
			adjustVideoCodecConfidence(ic, codec_id, confidence);
		} else if (codec_type == AVMEDIA_TYPE_AUDIO) {
			adjustAudioCodecConfidence(ic, codec_id, confidence);
		}
	}
}

static void adjustConfidenceIfNeeded(const char *mime,
		AVFormatContext *ic, float *confidence)
{
	//check mime
	if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)) {
		adjustMPEG4Confidence(ic, confidence);
	} else {
		//add to here;
	}

	//check codec
	adjustCodecConfidence(ic, confidence);
}

static const char *SniffFFMPEGCommon(const char *url, float *confidence)
{
	size_t i = 0;
	int err = 0;
	const char *container = NULL;
	AVFormatContext *ic = NULL;

	status_t status = initFFmpeg();
	if (status != OK) {
		ALOGE("could not init ffmpeg");
		return NULL;
	}

	ic = avformat_alloc_context();
	if (!ic)
	{
		ALOGE("oom for alloc avformat context");
		return NULL;
	}

	err = avformat_open_input(&ic, url, NULL, NULL);
	if (err < 0) {
		ALOGE("avformat_open_input faild, url: %s err: %d", url, err);
		return NULL;
	}

	av_dump_format(ic, 0, url, 0);

	ALOGI("FFmpegExtrator, url: %s, format_name: %s, format_long_name: %s",
			url, ic->iformat->name, ic->iformat->long_name);

	ALOGI("list the formats suppoted by ffmpeg: ");
	ALOGI("========================================");
	for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
		ALOGV("format_names[%02d]: %s", i, FILE_FORMATS[i].format);
	}
	ALOGI("========================================");

	for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
		int len = strlen(FILE_FORMATS[i].format);
		if (!av_strncasecmp(ic->iformat->name, FILE_FORMATS[i].format, len)) {
			container = FILE_FORMATS[i].container;
			break;
		}
	}

	if (container) {
		adjustConfidenceIfNeeded(container, ic, confidence);
	}

	avformat_close_input(&ic);
	av_free(ic);

	return container;
}

static const char *LegacySniffFFMPEG(const sp<DataSource> &source, float *confidence)
{
	String8 uri = source->getUri();
	if (uri.empty()) {
		return NULL;
	}

	ALOGI("source url:%s", uri.string());

	return SniffFFMPEGCommon(uri.string(), confidence);
}

static const char *BetterSniffFFMPEG(const sp<DataSource> &source, float *confidence)
{
	char url[128] = {0};

	ALOGI("android-source:%p", &source);

	// pass the addr of smart pointer("source")
	snprintf(url, sizeof(url), "android-source:%p", &source);

	return SniffFFMPEGCommon(url, confidence);
}

bool SniffFFMPEG(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
	ALOGV("SniffFFMPEG");

	*confidence = 0.08f;  // be the last resort, by default

	const char *container = BetterSniffFFMPEG(source, confidence);
	if (!container) {
		ALOGW("sniff through BetterSniffFFMPEG failed, try LegacySniffFFMPEG");
		container = LegacySniffFFMPEG(source, confidence);
		if (container) {
			ALOGI("sniff through LegacySniffFFMPEG success");
		}
	} else {
		ALOGI("sniff through BetterSniffFFMPEG success");
	}

	if (container == NULL) {
		ALOGD("SniffFFMPEG failed to sniff this source");
		return false;
	}

	ALOGV("found container: %s", container);

	mimeType->setTo(container);

	/* use MPEG4Extractor(not extended extractor) for HTTP source only */
	if (!strcasecmp(container, MEDIA_MIMETYPE_CONTAINER_MPEG4)
			&& (source->flags() & DataSource::kIsCachingDataSource)) {
		ALOGI("support container: %s, but it is caching data source, "
				"Don't use ffmpegextractor", container);
		return false;
	}

	ALOGD("ffmpeg detected media content as '%s' with confidence %.2f",
			container, *confidence);

	*meta = new AMessage;
	(*meta)->setString("extended-extractor", "extended-extractor");
	(*meta)->setString("extended-extractor-subtype", "ffmpegextractor");

	//debug only
	char value[PROPERTY_VALUE_MAX];
	property_get("sys.media.parser.ffmpeg", value, "0");
	if (atoi(value)) {
		ALOGI("[debug] parser use ffmpeg");
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
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)     ||
            !strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_MPEG)          ||
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
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WMA))) {
        ret = new FFmpegExtractor(source);
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
