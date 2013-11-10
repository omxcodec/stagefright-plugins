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

#include <utils/Errors.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MetaData.h>

#include "codec_utils.h"

namespace android {

static void EncodeSize14(uint8_t **_ptr, size_t size) {
    CHECK_LE(size, 0x3fff);

    uint8_t *ptr = *_ptr;

    *ptr++ = 0x80 | (size >> 7);
    *ptr++ = size & 0x7f;

    *_ptr = ptr;
}

sp<ABuffer> MakeMPEGVideoESDS(const sp<ABuffer> &csd) {
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
uint32_t getAACSampleRate(const uint8_t sf_index)
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

