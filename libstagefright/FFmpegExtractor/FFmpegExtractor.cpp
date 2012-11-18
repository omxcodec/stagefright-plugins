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

#include <limits.h> /* INT_MAX */

#include <media/stagefright/foundation/ABitReader.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>
#include <utils/misc.h>

#include "include/avc_utils.h"
#include "utils/common_utils.h"
#include "utils/ffmpeg_utils.h"
#include "FFmpegExtractor.h"

#define DEBUG_READ_ENTRY 0
#define DIABLE_VIDEO     0
#define DIABLE_AUDIO     0
#define WAIT_KEY_PACKET_AFTER_SEEK 1
#define DISABLE_NAL_TO_ANNEXB 0

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

    DISALLOW_EVIL_CONSTRUCTORS(Track);
};

////////////////////////////////////////////////////////////////////////////////

FFmpegExtractor::FFmpegExtractor(const sp<DataSource> &source)
    : mDataSource(source),
      mReaderThreadStarted(false),
      mInitCheck(NO_INIT) {
    LOGV("FFmpegExtractor::FFmpegExtractor");

    int err;
    const char *url = mDataSource->getNamURI();
    if (url == NULL) {
        LOGI("url is error!");
        return;
    }
    // is it right?
    if (!strcmp(url, "-")) {
        av_strlcpy(mFilename, "pipe:", strlen("pipe:") + 1);
    } else {
        av_strlcpy(mFilename, url, strlen(url) + 1);
    }
    LOGI("url: %s, mFilename: %s", url, mFilename);

    err = initStreams();
    if (err < 0) {
        LOGE("failed to init ffmpeg");
        return;
    }

    // start reader here, as we want to extract extradata from bitstream if no extradata
    startReaderThread();

    while(mProbePkts <= EXTRACTOR_MAX_PROBE_PACKETS && !mEOF &&
        (mFormatCtx->pb ? !mFormatCtx->pb->error : 1) &&
        (mDefersToCreateVideoTrack || mDefersToCreateAudioTrack)) {
        // FIXME, i am so lazy! Should use pthread_cond_wait to wait conditions
        NamDelay(5);
    }

    LOGV("mProbePkts: %d, mEOF: %d, pb->error(if has): %d, mDefersToCreateVideoTrack: %d, mDefersToCreateAudioTrack: %d",
        mProbePkts, mEOF, mFormatCtx->pb ? mFormatCtx->pb->error : 0, mDefersToCreateVideoTrack, mDefersToCreateAudioTrack);

    mInitCheck = OK;
}

FFmpegExtractor::~FFmpegExtractor() {
    LOGV("FFmpegExtractor::~FFmpegExtractor");

    // stop reader here if no track!
    stopReaderThread();

    deInitStreams();
}

size_t FFmpegExtractor::countTracks() {
    return mInitCheck == OK ? mTracks.size() : 0;
}

sp<MediaSource> FFmpegExtractor::getTrack(size_t index) {
    LOGV("FFmpegExtractor::getTrack[%d]", index);

    if (mInitCheck != OK) {
        return NULL;
    }

    if (index >= mTracks.size()) {
        return NULL;
    }

    return mTracks.valueAt(index);
}

sp<MetaData> FFmpegExtractor::getTrackMetaData(size_t index, uint32_t flags) {
    LOGV("FFmpegExtractor::getTrackMetaData[%d]", index);

    if (mInitCheck != OK) {
        return NULL;
    }

    if (index >= mTracks.size()) {
        return NULL;
    }

    return mTracks.valueAt(index)->getFormat();
}

sp<MetaData> FFmpegExtractor::getMetaData() {
    LOGV("FFmpegExtractor::getMetaData");

    if (mInitCheck != OK) {
        return NULL;
    }

    sp<MetaData> meta = new MetaData;
    // TODO
    meta->setCString(kKeyMIMEType, "video/ffmpeg");

    return meta;
}

uint32_t FFmpegExtractor::flags() const {
    LOGV("FFmpegExtractor::flags");

    if (mInitCheck != OK) {
        return NULL;
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
    const char *name;
    bool *defersToCreateTrack;
    AVBitStreamFilterContext **bsfc;

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
            avctx->codec_id == CODEC_ID_H263  ||
            avctx->codec_id == CODEC_ID_H263P ||
            avctx->codec_id == CODEC_ID_H263I ||
            avctx->codec_id == CODEC_ID_WMV1)
        return 1;

    // is extradata compatible with android?
    if (avctx->codec_id != CODEC_ID_AAC) {
        int is_compatible = is_extradata_compatible_with_android(avctx);
        if (!is_compatible) {
            LOGI("%s extradata is not compatible with android, should to extract it from bitstream",
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
        LOGI("No %s extradata found, should to extract it from bitstream",
                av_get_media_type_string(avctx->codec_type));
        *defersToCreateTrack = true;
         //CHECK(name != NULL);
        if (!*bsfc && name) {
            *bsfc = av_bitstream_filter_init(name);
            if (!*bsfc) {
                LOGE("Cannot open the %s BSF!", name);
                *defersToCreateTrack = false;
                return -1;
            } else {
                LOGV("open the %s bsf", name);
                return 0;
            }
        } else {
            return 0;
        }
    }
    return 1;
}


int FFmpegExtractor::stream_component_open(int stream_index)
{
    AVCodecContext *avctx;
    sp<MetaData> meta;
    bool isAVC = false;
    bool supported = false;
    uint32_t type;
    const void *data;
    size_t size;
    int ret;

    LOGI("stream_index: %d", stream_index);
    if (stream_index < 0 || stream_index >= mFormatCtx->nb_streams)
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
        supported = true;
        break;
    default:
        supported = false;
        break;
    }

    if (!supported) {
        LOGE("unsupport the codec, id: 0x%0x", avctx->codec_id);
        return -1;
    }
    LOGV("support the codec");

    unsigned streamType;
    ssize_t index = mTracks.indexOfKey(stream_index);

    if (index >= 0) {
        LOGE("this track already exists");
        return 0;
    }

    mFormatCtx->streams[stream_index]->discard = AVDISCARD_DEFAULT;

    char tagbuf[32];
    av_get_codec_tag_string(tagbuf, sizeof(tagbuf), avctx->codec_tag);
    LOGV("Tag %s/0x%08x with codec id '%d'\n", tagbuf, avctx->codec_tag, avctx->codec_id);

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
            LOGV("video stream extradata:");
            hexdump(avctx->extradata, avctx->extradata_size);
        } else {
            LOGV("video stream no extradata, but we can ignore it.");
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
                LOGV("AVC");

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
                LOGV("H264");

                /* set NULL to release meta as we will new a meta in MakeAVCCodecSpecificData() fxn */
                meta->clear();
                meta = NULL;

                sp<ABuffer> buffer = new ABuffer(avctx->extradata_size);
                memcpy(buffer->data(), avctx->extradata, avctx->extradata_size);
                meta = MakeAVCCodecSpecificData(buffer);
            }
            break;
        case CODEC_ID_MPEG4:
            LOGV("MPEG4");
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
            LOGV("H263");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);
            break;
        case CODEC_ID_MPEG2VIDEO:
            LOGV("MPEG2VIDEO");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);
            {
                sp<ABuffer> csd = new ABuffer(avctx->extradata_size);
                memcpy(csd->data(), avctx->extradata, avctx->extradata_size);
                sp<ABuffer> esds = MakeMPEGVideoESDS(csd);
                meta->setData(kKeyESDS, kTypeESDS, esds->data(), esds->size());
            }
            break;
        case CODEC_ID_VC1:
            LOGV("VC1");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            //meta->setInt32(kKeyWMVVersion, kTypeVC1); // TODO
            break;
        case CODEC_ID_WMV1:
            LOGV("WMV1");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setInt32(kKeyWMVVersion, kTypeWMVVer_7);
            break;
        case CODEC_ID_WMV2:
            LOGV("WMV2");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMVVersion, kTypeWMVVer_8);
            break;
        case CODEC_ID_WMV3:
            LOGV("WMV2");
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMVVersion, kTypeWMVVer_9);
            break;
        default:
            CHECK(!"Should not be here. Unsupported codec.");
            break;
        }

        LOGI("width: %d, height: %d, bit_rate: %d", avctx->width, avctx->height, avctx->bit_rate);

        meta->setInt32(kKeyWidth, avctx->width);
        meta->setInt32(kKeyHeight, avctx->height);
        if (avctx->bit_rate > 0)
            meta->setInt32(kKeyBitRate, avctx->bit_rate);
        if (mFormatCtx->duration != AV_NOPTS_VALUE)
            meta->setInt64(kKeyDuration, mFormatCtx->duration);

        LOGV("create a video track");
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
            LOGV("audio stream extradata:");
            hexdump(avctx->extradata, avctx->extradata_size);
        } else {
            LOGV("audio stream no extradata, but we can ignore it.");
        }

        switch(avctx->codec_id) {
        case CODEC_ID_MP1:
            LOGV("MP1");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_I);
            break;
        case CODEC_ID_MP2:
            LOGV("MP2");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);
            break;
        case CODEC_ID_MP3:
            LOGV("MP3");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);
            break;
        case CODEC_ID_AC3:
            LOGV("AC3");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);
            break;
        case CODEC_ID_AAC:
            LOGV("AAC"); 
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
                LOGE("unsupport the sample rate");
                return -1;
            }
            channel = (header[1] >> 3) & 0xf;
            LOGV("profile: %d, sf_index: %d, channel: %d", profile, sf_index, channel);

            meta = MakeAACCodecSpecificData(profile, sf_index, channel);
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);
            break;
        case CODEC_ID_WMAV1:  // TODO, version?
            LOGV("WMAV1");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            break;
        case CODEC_ID_WMAV2:
            LOGV("WMAV2");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMAVersion, kTypeWMA);
            break;
        case CODEC_ID_WMAPRO:
            LOGV("WMAPRO");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMAVersion, kTypeWMAPro);
            break;
        case CODEC_ID_WMALOSSLESS:
            LOGV("WMALOSSLESS");
            meta = new MetaData;
            meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
            meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
            meta->setInt32(kKeyWMAVersion, kTypeWMALossLess);
            break;
        default:
            CHECK(!"Should not be here. Unsupported codec.");
            break;
        }

        LOGI("bit_rate: %d, sample_rate: %d, channels: %d", avctx->bit_rate, avctx->sample_rate, avctx->channels);

        meta->setInt32(kKeySampleRate, avctx->sample_rate);
        meta->setInt32(kKeyChannelCount, avctx->channels);
        meta->setInt32(kKeyBitRate, avctx->bit_rate);
        if (mFormatCtx->duration != AV_NOPTS_VALUE)
            meta->setInt64(kKeyDuration, mFormatCtx->duration);

        LOGV("create a audio track");
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

    if (stream_index < 0 || stream_index >= mFormatCtx->nb_streams)
        return;
    avctx = mFormatCtx->streams[stream_index]->codec;

    switch (avctx->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        LOGV("packet_queue_abort videoq");
        packet_queue_abort(&mVideoQ);
        /* wait until the end */
        while (!mAbortRequest && !mVideoEOSReceived) {
            LOGV("wait for video received");
            NamDelay(10);
        }
        LOGV("packet_queue_end videoq");
        packet_queue_end(&mVideoQ);
        break;
    case AVMEDIA_TYPE_AUDIO:
        LOGV("packet_queue_abort audioq");
        packet_queue_abort(&mAudioQ);
        while (!mAbortRequest && !mAudioEOSReceived) {
            LOGV("wait for audio received");
            NamDelay(10);
        }
        LOGV("packet_queue_end audioq");
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
    LOGI("%s: %s\n", filename, errbuf_ptr);
}

void FFmpegExtractor::setFFmpegDefaultOpts()
{
    mGenPTS       = 0;
#if DIABLE_VIDEO
    mVideoDisable = 1;
#else
    mVideoDisable = 0;
#endif
#if DIABLE_AUDIO
    mAudioDisable = 1;
#else
    mAudioDisable = 0;
#endif
    mShowStatus   = 1;
    mSeekByBytes  = 0; /* seek by bytes 0=off 1=on -1=auto" */
    mStartTime    = AV_NOPTS_VALUE;
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
    int err, i;
    status_t status;
    int eof = 0;
    int ret = 0, audio_ret = 0, video_ret = 0;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    AVDictionary **opts;
    int orig_nb_streams;
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
    mFormatCtx->interrupt_callback.callback = decode_interrupt_cb;
    mFormatCtx->interrupt_callback.opaque = this;
    LOGV("mFilename: %s", mFilename);
    err = avformat_open_input(&mFormatCtx, mFilename, NULL, &format_opts);
    if (err < 0) {
        print_error_ex(mFilename, err);
        ret = -1;
        goto fail;
    }
    if ((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))) {
        LOGE("Option %s not found.\n", t->key);
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
        LOGE("%s: could not find codec parameters\n", mFilename);
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

    /* if seeking requested, we execute it */
    if (mStartTime != AV_NOPTS_VALUE) {
        int64_t timestamp;

        timestamp = mStartTime;
        /* add the stream start time */
        if (mFormatCtx->start_time != AV_NOPTS_VALUE)
            timestamp += mFormatCtx->start_time;
        ret = avformat_seek_file(mFormatCtx, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            LOGE("%s: could not seek to position %0.3f",
                    mFilename, (double)timestamp / AV_TIME_BASE);
            goto fail;
        }
    }

    for (i = 0; i < mFormatCtx->nb_streams; i++)
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

    if (mFormatCtx->duration != AV_NOPTS_VALUE) {
        int hours, mins, secs, us;
        secs = mFormatCtx->duration / AV_TIME_BASE;
        us = mFormatCtx->duration % AV_TIME_BASE;
        mins = secs / 60;
        secs %= 60;
        hours = mins / 60;
        mins %= 60;
        LOGI("the duration is %02d:%02d:%02d.%02d", hours, mins, secs, (100 * us) / AV_TIME_BASE);
    }

    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        audio_ret = stream_component_open(st_index[AVMEDIA_TYPE_AUDIO]);
    }

    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        video_ret = stream_component_open(st_index[AVMEDIA_TYPE_VIDEO]);
    }

    if ( audio_ret < 0 && video_ret < 0) {
        LOGE("%s: could not open codecs\n", mFilename);
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
    LOGV("Starting reader thread");
    Mutex::Autolock autoLock(mLock);

    if (mReaderThreadStarted)
        return OK;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    pthread_create(&mReaderThread, &attr, ReaderWrapper, this);
    pthread_attr_destroy(&attr);
    mReaderThreadStarted = true;
    LOGD("Reader thread started");

    return OK;
}

void FFmpegExtractor::stopReaderThread() {
    LOGV("Stopping reader thread");
    Mutex::Autolock autoLock(mLock);

    if (!mReaderThreadStarted) {
        LOGD("Reader thread have been stopped");
        return;
    }

    mAbortRequest = 1;

    void *dummy;
    pthread_join(mReaderThread, &dummy);
    mReaderThreadStarted = false;
    LOGD("Reader thread stopped");
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

    LOGV("FFmpegExtractor::readerEntry");

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
            NamDelay(10);
            continue;
        }
#endif

        if (mSeekReq) {
            LOGV("readerEntry, mSeekReq: %d", mSeekReq);
            ret = avformat_seek_file(mFormatCtx, -1, INT64_MIN, mSeekPos, INT64_MAX, mSeekFlags);
            if (ret < 0) {
                LOGE("%s: error while seeking", mFormatCtx->filename);
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
            LOGV("readerEntry, is full, fuck");
#endif
            /* wait 10 ms */
            NamDelay(10);
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
            NamDelay(10);
#if DEBUG_READ_ENTRY
            LOGV("readerEntry, eof = 1, mVideoQ.size: %d, mVideoQ.nb_packets: %d, mAudioQ.size: %d, mAudioQ.nb_packets: %d",
                    mVideoQ.size, mVideoQ.nb_packets, mAudioQ.size, mAudioQ.nb_packets);
#endif
            if (mAudioQ.size + mVideoQ.size  == 0) {
                if (mLoop != 1 && (!mLoop || --mLoop)) {
                    if (mVideoStreamIdx >= 0) {
                        stream_seek(mStartTime != AV_NOPTS_VALUE ? mStartTime : 0, AVMEDIA_TYPE_VIDEO);
                    } else if (mAudioStreamIdx >= 0) {
                        stream_seek(mStartTime != AV_NOPTS_VALUE ? mStartTime : 0, AVMEDIA_TYPE_AUDIO);
                    }
                } else if (mAutoExit) {
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
                    //LOGV("ret == AVERROR_EOF");
		}
                if (url_feof(mFormatCtx->pb)) {
                    //LOGV("url_feof(mFormatCtx->pb)");
		}

                eof = 1;
                mEOF = true;
            if (mFormatCtx->pb && mFormatCtx->pb->error) {
                LOGE("mFormatCtx->pb->error: %d", mFormatCtx->pb->error);
                break;
            }
            NamDelay(100);
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
                    LOGI("probe packet counter: %d when create video track ok", mProbePkts);
                if (mProbePkts == EXTRACTOR_MAX_PROBE_PACKETS)
                    LOGI("probe packet counter to max: %d, create video track: %d",
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
                    LOGI("probe packet counter: %d when create audio track ok", mProbePkts);
                if (mProbePkts == EXTRACTOR_MAX_PROBE_PACKETS)
                    LOGI("probe packet counter to max: %d, create audio track: %d",
                        mProbePkts, !mDefersToCreateAudioTrack);
            }
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        pkt_in_play_range = mDuration == AV_NOPTS_VALUE ||
                (pkt->pts - mFormatCtx->streams[pkt->stream_index]->start_time) *
                av_q2d(mFormatCtx->streams[pkt->stream_index]->time_base) -
                (double)(mStartTime != AV_NOPTS_VALUE ? mStartTime : 0) / 1000000
                <= ((double)mDuration / 1000000);
        if (pkt->stream_index == mAudioStreamIdx && pkt_in_play_range) {
            packet_queue_put(&mAudioQ, pkt);
        } else if (pkt->stream_index == mVideoStreamIdx && pkt_in_play_range) {
            packet_queue_put(&mVideoQ, pkt);
        } else {
            av_free_packet(pkt);
        }
    }
    /* wait until the end */
    while (!mAbortRequest) {
        NamDelay(100);
    }

    ret = 0;
fail:
    LOGI("reader thread goto end...");

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

            LOGV("the stream is AVC, the length of a NAL unit: %d", mNALLengthSize);

            mNal2AnnexB = true;
        }
    }

    mMediaType = mStream->codec->codec_type;
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
        LOGV("~~~%s seekTimeUs: %lld, mode: %d", av_get_media_type_string(mMediaType), seekTimeUs, mode);
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
        LOGV("read %s flush pkt", av_get_media_type_string(mMediaType));
        av_free_packet(&pkt);
        goto retry;
    } else if (pkt.data == NULL && pkt.size == 0) {
        LOGV("read %s eos pkt", av_get_media_type_string(mMediaType));
        av_free_packet(&pkt);
        mExtractor->reachedEOS(mMediaType);
	return ERROR_END_OF_STREAM;
    }

    key = pkt.flags & AV_PKT_FLAG_KEY ? 1 : 0;

    if (waitKeyPkt) {
        if (!key) {
            LOGV("drop the no key packet");
            av_free_packet(&pkt);
            goto retry;
        } else {
            LOGV("~~~~~~ got the key packet");
            waitKeyPkt = false;
        }
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
            LOGV("status != OK");
            mediaBuffer->release();
            mediaBuffer = NULL;
            av_free_packet(&pkt);
            return ERROR_MALFORMED;
        }
    } else {
        memcpy(mediaBuffer->data(), pkt.data, pkt.size);
    }

    pktTS = pkt.pts;
    // use dts when AVI
    if (pkt.pts == AV_NOPTS_VALUE)
        pktTS = pkt.dts;

#if 0
    // TODO, Stagefright can't handle negative timestamps
    // if needed, work around this by offsetting them manually?
    if (pktTS < 0)
        pktTS = 0;
#endif

    timeUs = (int64_t)(pktTS * av_q2d(mStream->time_base) * 1000000);

#if 0
    LOGV("read %s pkt, size: %d, key: %d, pts: %lld, dts: %lld, timeUs: %llu us (%.2f secs)",
        av_get_media_type_string(mMediaType), pkt.size, key, pkt.pts, pkt.dts, timeUs, timeUs/1E6);
#endif

#if 0
    // TODO, Stagefright can't handle negative timestamps
    // if needed, work around this by offsetting them manually?
    if (timeUs < 0)
        timeUs = 0;
#endif

    mediaBuffer->meta_data()->setInt64(kKeyTime, timeUs);
    mediaBuffer->meta_data()->setInt32(kKeyIsSyncFrame, key);

    *buffer = mediaBuffer;

    av_free_packet(&pkt);

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

// LegacySniffFFMPEG
typedef struct {
    const char *extension;
    const char *container;
} extmap;

static extmap FILE_EXTS[] = {
        {".mp4", MEDIA_MIMETYPE_CONTAINER_MPEG4},
        {".3gp", MEDIA_MIMETYPE_CONTAINER_MPEG4},
        {".mp3", MEDIA_MIMETYPE_AUDIO_MPEG},
        {".mov", MEDIA_MIMETYPE_CONTAINER_MOV},
        {".mkv", MEDIA_MIMETYPE_CONTAINER_MATROSKA},
        {".ts",  MEDIA_MIMETYPE_CONTAINER_TS},
        {".avi", MEDIA_MIMETYPE_CONTAINER_AVI},
        {".asf", MEDIA_MIMETYPE_CONTAINER_ASF},
#if 0
        {".wmv", MEDIA_MIMETYPE_CONTAINER_WMV},
        {".wma", MEDIA_MIMETYPE_CONTAINER_WMA},
        {".mpg", MEDIA_MIMETYPE_CONTAINER_MPG},
        {".flv", MEDIA_MIMETYPE_CONTAINER_FLV},
        {".divx", MEDIA_MIMETYPE_CONTAINER_DIVX},
        {".mp2", MEDIA_MIMETYPE_CONTAINER_MP2},
        {".ape", MEDIA_MIMETYPE_CONTAINER_APE},
        {".rm ", MEDIA_MIMETYPE_CONTAINER_RM},
        {".ra",  MEDIA_MIMETYPE_CONTAINER_RA},
#endif
};

const char *LegacySniffFFMPEG(const char * uri)
{
    size_t i;
    const char *container = NULL;

    LOGI("list the file extensions suppoted by ffmpeg: ");
    LOGI("========================================");
    for (i = 0; i < NELEM(FILE_EXTS); ++i) {
            LOGV("file_exts[%02d]: %s", i, FILE_EXTS[i].extension);
    }
    LOGI("========================================");

    int lenURI = strlen(uri);
    for (i = 0; i < NELEM(FILE_EXTS); ++i) {
        int len = strlen(FILE_EXTS[i].extension);
        int start = lenURI - len;
        if (start > 0) {
            if (!av_strncasecmp(uri + start, FILE_EXTS[i].extension, len)) {
                container = FILE_EXTS[i].container;
                break;
            }
        }
    }

    return container;
}

// BetterSniffFFMPEG
typedef struct {
    const char *format;
    const char *container;
} formatmap;

static formatmap FILE_FORMATS[] = {
        {"mpegts",                  MEDIA_MIMETYPE_CONTAINER_TS},
        {"mov,mp4,m4a,3gp,3g2,mj2", MEDIA_MIMETYPE_CONTAINER_MOV},
        {"asf",                     MEDIA_MIMETYPE_CONTAINER_ASF},
};

const char *BetterSniffFFMPEG(const char * uri)
{
    size_t i;
    const char *container = NULL;
    AVFormatContext *ic = NULL;

    status_t status = initFFmpeg();
    if (status != OK) {
        LOGE("could not init ffmpeg");
        return false;
    }

    ic = avformat_alloc_context();
    avformat_open_input(&ic, uri, NULL, NULL);

    av_dump_format(ic, 0, uri, 0);

    LOGI("FFmpegExtrator, uri: %s, format_name: %s, format_long_name: %s", uri, ic->iformat->name, ic->iformat->long_name);

    LOGI("list the format suppoted by ffmpeg: ");
    LOGI("========================================");
    for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
            LOGV("format_names[%02d]: %s", i, FILE_FORMATS[i].format);
    }
    LOGI("========================================");

    for (i = 0; i < NELEM(FILE_FORMATS); ++i) {
        int len = strlen(FILE_FORMATS[i].format);
        if (!av_strncasecmp(ic->iformat->name, FILE_FORMATS[i].format, len)) {
            container = FILE_FORMATS[i].container;
            break;
        }
    }

    avformat_close_input(&ic);
    av_free(ic);

    return container;
}

bool SniffFFMPEG(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *meta) {
    LOGV("SniffFFMPEG");
    const char *uri, *container = NULL;

    uri = source->getNamURI();

    if (!uri)
        return false;

    LOGI("ffmpeg uri: %s", uri);

    container = BetterSniffFFMPEG(uri);
    if (!container) {
        LOGW("sniff through LegacySniffFFMPEG, only check the file extension");
        container = LegacySniffFFMPEG(uri);
    }

    if (container == NULL)
        return false;

    LOGV("found container: %s", container);

    *confidence = 0.88f;  // Slightly larger than other extractor's confidence
    mimeType->setTo(container);

    /* use MPEG4Extractor(not extended extractor) for HTTP source only */
    if (!av_strcasecmp(container, MEDIA_MIMETYPE_CONTAINER_MPEG4)
            && (source->flags() & DataSource::kIsCachingDataSource)) {
            return true;
    }

    *meta = new AMessage;
    (*meta)->setString("extended-extractor", "extended-extractor");
    (*meta)->setString("extended-extractor-subtype", "ffmpegextractor");

    return true;
}

}  // namespace android
