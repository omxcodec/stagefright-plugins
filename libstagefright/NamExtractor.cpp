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
#define LOG_TAG "NamExtractor"
#include <utils/Log.h>

#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MetaData.h>
#include <utils/String8.h>

#include "FFmpegExtractor/FFmpegExtractor.h"
#undef strcasecmp
#include <strings.h>

namespace android {

#ifdef __cplusplus
extern "C" {
#endif

// TODO, SniffFFMPEG, SniffVLC, SniffMPLAYER
static const DataSource::SnifferFunc gNamSniffers[] = {
    SniffFFMPEG,
};

// static
void snifferArray(const DataSource::SnifferFunc *snifferArray[], int *count)
{
    *count = sizeof(gNamSniffers) / sizeof(gNamSniffers[0]);
    *snifferArray = gNamSniffers;
}

// static
MediaExtractor *createExtractor(const sp<DataSource> &source, const char *mime) {
    MediaExtractor *ret = NULL;

    if (!strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MPEG4)         ||
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
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_FLAC)      ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_APE)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_DTS)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_MP2)       ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_RA)        ||
            !strcasecmp(mime, MEDIA_MIMETYPE_CONTAINER_WMA)) {
        ret = new FFmpegExtractor(source, NULL);
    }

    if (ret)
        ALOGI("NamExtractor support the mime: %s", mime);
    else
        ALOGI("NamExtractor don't support the mime: %s", mime);

    return ret;
}

#ifdef __cplusplus
}
#endif

}   //namespace android
