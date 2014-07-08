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
#define LOG_TAG "FFMPEG"
#include <utils/Log.h>

#include <stdlib.h>

#include <media/stagefright/DataSource.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "libavformat/url.h"

#ifdef __cplusplus
}
#endif

namespace android {

class FFSource
{
public:
    FFSource(DataSource *source);
    int init_check();
    int read(unsigned char *buf, size_t size);
    int64_t seek(int64_t pos);
    off64_t getSize();
    ~FFSource();
protected:
    sp<DataSource> mSource;
    int64_t mOffset;
};

FFSource::FFSource(DataSource *source)
    : mSource(source),
      mOffset(0)
{
}

FFSource::~FFSource()
{
	mSource = NULL;
}

int FFSource::init_check()
{
    if (mSource->initCheck() != OK) {
        ALOGE("FFSource initCheck failed");
        return -1;
    }

    return 0;
}

int FFSource::read(unsigned char *buf, size_t size)
{
    ssize_t n = 0;

    n = mSource->readAt(mOffset, buf, size);
    if (n == UNKNOWN_ERROR) {
        ALOGE("FFSource readAt failed");
        return AVERROR(errno);
    }
    assert(n >= 0);
    mOffset += n;

    return n;
}

int64_t FFSource::seek(int64_t pos)
{
    mOffset = pos;
    return 0;
}

off64_t FFSource::getSize()
{
    off64_t sz = -1;

    if (mSource->getSize(&sz) != OK) {
         ALOGE("FFSource getSize failed");
         return AVERROR(errno);
    }

    return sz;
}

/////////////////////////////////////////////////////////////////

static int android_open(URLContext *h, const char *url, int flags)
{
    // the url in form of "android-source:<DataSource Ptr>",
    // the DataSource Pointer passed by the ffmpeg extractor
    DataSource *source = NULL;

    ALOGV("android source begin open");

    if (!url) {
        ALOGE("android url is null!");
        return -1;
    }

    ALOGV("android open, url: %s", url);
    sscanf(url + strlen("android-source:"), "%p", &source);
    if(source == NULL){
        ALOGE("ffmpeg open data source error!");
        return -1;
    }
    ALOGV("ffmpeg open android data source success, source ptr: %p", source);

    FFSource *ffs = new FFSource(source);
    h->priv_data = (void *)ffs;

    ALOGV("android source open success");

    return 0;
}
static int android_read(URLContext *h, unsigned char *buf, int size)
{
    FFSource* ffs = (FFSource *)h->priv_data;
    return ffs->read(buf, size);
}

static int android_write(URLContext *h, const unsigned char *buf, int size)
{
    return -1;
}

static int64_t android_seek(URLContext *h, int64_t pos, int whence)
{
    FFSource* ffs = (FFSource*)h->priv_data;

    if (whence == AVSEEK_SIZE) {
        return ffs->getSize();
    }

    ffs->seek(pos);
    return 0;
}

static int android_close(URLContext *h)
{
    FFSource* ffs = (FFSource*)h->priv_data;
    ALOGV("android source close");
    delete ffs;
    return 0;
}

static int android_get_handle(URLContext *h)
{
    return (intptr_t)h->priv_data;
}

static int android_check(URLContext *h, int mask)
{
    FFSource* ffs = (FFSource*)h->priv_data;

    if (ffs->init_check() < 0)
        return AVERROR(EACCES); // FIXME

    return (mask & AVIO_FLAG_READ);
}

static URLProtocol ff_android_protocol;

void ffmpeg_register_android_source()
{
    memset(&ff_android_protocol, 0, sizeof(URLProtocol));
    ff_android_protocol.name                = "android-source";
    ff_android_protocol.url_open            = android_open;
    ff_android_protocol.url_read            = android_read;
    ff_android_protocol.url_write           = android_write;
    ff_android_protocol.url_seek            = android_seek;
    ff_android_protocol.url_close           = android_close;
    ff_android_protocol.url_get_file_handle = android_get_handle;
    ff_android_protocol.url_check           = android_check;
    
    ffurl_register_protocol(&ff_android_protocol, sizeof(URLProtocol));
}

}  // namespace android
