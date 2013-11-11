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
#define LOG_TAG "codec_utils"
#include <utils/Log.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "libavcodec/xiph.h"

#ifdef __cplusplus
}
#endif

#include <utils/Errors.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>
#include "include/avc_utils.h"

#include "codec_utils.h"

namespace android {

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

//Returns the sample rate based on the sampling frequency index
static uint32_t getAACSampleRate(const uint8_t sf_index)
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

//video

//H.264 Video Types
//http://msdn.microsoft.com/en-us/library/dd757808(v=vs.85).aspx

// H.264 bitstream without start codes.
sp<MetaData> setAVCFormat(AVCodecContext *avctx)
{
    ALOGV("AVC");

	CHECK_EQ(avctx->codec_id, AV_CODEC_ID_H264);
	CHECK_GT(avctx->extradata_size, 0);
	CHECK_EQ(avctx->extradata[0], 1); //configurationVersion

    if (avctx->width == 0 || avctx->height == 0) {
         int32_t width, height;
         sp<ABuffer> seqParamSet = new ABuffer(avctx->extradata_size - 8);
         memcpy(seqParamSet->data(), avctx->extradata + 8, avctx->extradata_size - 8);
         FindAVCDimensions(seqParamSet, &width, &height);
         avctx->width  = width;
         avctx->height = height;
     }

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_AVC);
    meta->setData(kKeyAVCC, kTypeAVCC, avctx->extradata, avctx->extradata_size);

	return meta;
}

// H.264 bitstream with start codes.
sp<MetaData> setH264Format(AVCodecContext *avctx)
{
    ALOGV("H264");

	CHECK_EQ(avctx->codec_id, AV_CODEC_ID_H264);
	CHECK_NE(avctx->extradata[0], 1); //configurationVersion

    sp<ABuffer> buffer = new ABuffer(avctx->extradata_size);
    memcpy(buffer->data(), avctx->extradata, avctx->extradata_size);
    return MakeAVCCodecSpecificData(buffer);
}

sp<MetaData> setMPEG4Format(AVCodecContext *avctx)
{
    ALOGV("MPEG4");

    sp<ABuffer> csd = new ABuffer(avctx->extradata_size);
    memcpy(csd->data(), avctx->extradata, avctx->extradata_size);
    sp<ABuffer> esds = MakeMPEGVideoESDS(csd);

    sp<MetaData> meta = new MetaData;
    meta->setData(kKeyESDS, kTypeESDS, esds->data(), esds->size());
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG4);

    return meta;
}

sp<MetaData> setH263Format(AVCodecContext *avctx)
{
    ALOGV("H263");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_H263);

    return meta;
}

sp<MetaData> setMPEG2VIDEOFormat(AVCodecContext *avctx)
{
    ALOGV("MPEG%uVIDEO", avctx->codec_id == AV_CODEC_ID_MPEG2VIDEO ? 2 : 1);

    sp<ABuffer> csd = new ABuffer(avctx->extradata_size);
    memcpy(csd->data(), avctx->extradata, avctx->extradata_size);
    sp<ABuffer> esds = MakeMPEGVideoESDS(csd);

    sp<MetaData> meta = new MetaData;
    meta->setData(kKeyESDS, kTypeESDS, esds->data(), esds->size());
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_MPEG2);

    return meta;
}

sp<MetaData> setVC1Format(AVCodecContext *avctx)
{
    ALOGV("VC1");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_VC1);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

sp<MetaData> setWMV1Format(AVCodecContext *avctx)
{
    ALOGV("WMV1");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
    meta->setInt32(kKeyWMVVersion, kTypeWMVVer_7);

    return meta;
}

sp<MetaData> setWMV2Format(AVCodecContext *avctx)
{
    ALOGV("WMV2");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyWMVVersion, kTypeWMVVer_8);

    return meta;
}

sp<MetaData> setWMV3Format(AVCodecContext *avctx)
{
    ALOGV("WMV3");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_WMV);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyWMVVersion, kTypeWMVVer_9);

    return meta;
}

sp<MetaData> setRV20Format(AVCodecContext *avctx)
{
    ALOGV("RV20");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RV);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyRVVersion, kTypeRVVer_G2); //http://en.wikipedia.org/wiki/RealVide

    return meta;
}

sp<MetaData> setRV30Format(AVCodecContext *avctx)
{
    ALOGV("RV30");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RV);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyRVVersion, kTypeRVVer_8); //http://en.wikipedia.org/wiki/RealVide

    return meta;
}

sp<MetaData> setRV40Format(AVCodecContext *avctx)
{
    ALOGV("RV40");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_RV);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyRVVersion, kTypeRVVer_9); //http://en.wikipedia.org/wiki/RealVide

    return meta;
}

sp<MetaData> setFLV1Format(AVCodecContext *avctx)
{
    ALOGV("FLV1(Sorenson H263)");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_FLV1);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

sp<MetaData> setHEVCFormat(AVCodecContext *avctx)
{
    ALOGV("HEVC");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_VIDEO_HEVC);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

//audio

sp<MetaData> setMP2Format(AVCodecContext *avctx)
{
    ALOGV("MP2");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG_LAYER_II);

    return meta;
}

sp<MetaData> setMP3Format(AVCodecContext *avctx)
{
    ALOGV("MP3");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_MPEG);

    return meta;
}

sp<MetaData> setVORBISFormat(AVCodecContext *avctx)
{
    ALOGV("VORBIS");

    uint8_t *header_start[3];
    int header_len[3];
    if (avpriv_split_xiph_headers(avctx->extradata,
                avctx->extradata_size, 30,
                header_start, header_len) < 0) {
        ALOGE("vorbis extradata corrupt.");
        return NULL;
    }

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_VORBIS);
    //identification header
    meta->setData(kKeyVorbisInfo,  0, header_start[0], header_len[0]);
    //setup header
    meta->setData(kKeyVorbisBooks, 0, header_start[2], header_len[2]);

    return meta;
}

sp<MetaData> setAC3Format(AVCodecContext *avctx)
{
    ALOGV("AC3");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AC3);

    return meta;
}

sp<MetaData> setAACFormat(AVCodecContext *avctx)
{
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
    sr = getAACSampleRate(sf_index);
    if (sr == 0) {
        ALOGE("unsupport the aac sample rate");
        return NULL;
    }
    channel = (header[1] >> 3) & 0xf;
    ALOGV("aac profile: %d, sf_index: %d, channel: %d", profile, sf_index, channel);

    sp<MetaData> meta = MakeAACCodecSpecificData(profile, sf_index, channel);
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_AAC);

    return meta;
}

sp<MetaData> setWMAV1Format(AVCodecContext *avctx)
{
    ALOGV("WMAV1");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyWMAVersion, kTypeWMA); //FIXME version?

    return meta;
}

sp<MetaData> setWMAV2Format(AVCodecContext *avctx)
{
    ALOGV("WMAV2");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyWMAVersion, kTypeWMA);

    return meta;
}

sp<MetaData> setWMAProFormat(AVCodecContext *avctx)
{
    ALOGV("WMAPro");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyWMAVersion, kTypeWMAPro);

    return meta;
}

sp<MetaData> setWMALossLessFormat(AVCodecContext *avctx)
{
    ALOGV("WMALOSSLESS");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_WMA);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);
    meta->setInt32(kKeyWMAVersion, kTypeWMALossLess);

    return meta;
}

sp<MetaData> setRAFormat(AVCodecContext *avctx)
{
    ALOGV("COOK");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_RA);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

sp<MetaData> setAPEFormat(AVCodecContext *avctx)
{
    ALOGV("APE");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_APE);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

sp<MetaData> setDTSFormat(AVCodecContext *avctx)
{
    ALOGV("DTS");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_DTS);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

sp<MetaData> setFLACFormat(AVCodecContext *avctx)
{
    ALOGV("FLAC");

    sp<MetaData> meta = new MetaData;
    meta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_FLAC);
    meta->setData(kKeyRawCodecSpecificData, 0, avctx->extradata, avctx->extradata_size);

    return meta;
}

//Convert H.264 NAL format to annex b
status_t convertNal2AnnexB(uint8_t *dst, size_t dst_size,
        uint8_t *src, size_t src_size, size_t nal_len_size)
{
    size_t i = 0;
    size_t nal_len = 0;
    status_t status = OK;

    CHECK_EQ(dst_size, src_size);
    CHECK(nal_len_size == 3 || nal_len_size == 4);

    while (src_size >= nal_len_size) {
        nal_len = 0;
        for( i = 0; i < nal_len_size; i++ ) {
            nal_len = (nal_len << 8) | src[i];
            dst[i] = 0;
        }
        dst[nal_len_size - 1] = 1;
        if (nal_len > INT_MAX || nal_len > src_size) {
            status = ERROR_MALFORMED;
            break;
        }
        dst += nal_len_size;
        src += nal_len_size;
        src_size -= nal_len_size;

        memcpy(dst, src, nal_len);

        dst += nal_len;
        src += nal_len;
        src_size -= nal_len;
    }

    return status;
}

}  // namespace android

