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
#define LOG_TAG "FFMPEG"
#include <utils/Log.h>

#include <utils/Errors.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"

#include <unistd.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <limits.h> /* INT_MAX */

#undef strncpy
#include <string.h>

#ifdef __cplusplus
}
#endif

#include <cutils/properties.h>

#include "ffmpeg_utils.h"
#include "ffmpeg_source.h"

// log
static int flags;

// dummy
const char program_name[] = "dummy";
const int program_birth_year = 2012;

// init ffmpeg
static pthread_mutex_t s_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_ref_count = 0;

namespace android {

//////////////////////////////////////////////////////////////////////////////////
// dummy
//////////////////////////////////////////////////////////////////////////////////
#ifdef __cplusplus
extern "C" {
#endif

void exit_program(int ret)
{
    // do nothing
}

void show_help_default(const char *opt, const char *arg)
{
    // do nothing
}

#ifdef __cplusplus
}
#endif

//////////////////////////////////////////////////////////////////////////////////
// log
//////////////////////////////////////////////////////////////////////////////////
static void sanitize(uint8_t *line){
    while(*line){
        if(*line < 0x08 || (*line > 0x0D && *line < 0x20))
            *line='?';
        line++;
    }
}

// TODO, remove static variables to support multi-instances
void nam_av_log_callback(void* ptr, int level, const char* fmt, va_list vl)
{
    static int print_prefix = 1;
    static int count;
    static char prev[1024];
    char line[1024];
    static int is_atty;

    if (level > av_log_get_level())
        return;
    av_log_format_line(ptr, level, fmt, vl, line, sizeof(line), &print_prefix);

    if (print_prefix && (flags & AV_LOG_SKIP_REPEATED) && !strcmp(line, prev)){
        count++;
        return;
    }
    if (count > 0) {
        ALOGI("Last message repeated %d times\n", count);
        count = 0;
    }
    strcpy(prev, line);
    sanitize((uint8_t *)line);

#if 0
    ALOGI("%s", line);
#else
#define LOG_BUF_SIZE 1024
    static char g_msg[LOG_BUF_SIZE];
    static int g_msg_len = 0;

    int saw_lf, check_len;

    do {
        check_len = g_msg_len + strlen(line) + 1;
        if (check_len <= LOG_BUF_SIZE) {
            /* lf: Line feed ('\n') */
            saw_lf = (strchr(line, '\n') != NULL) ? 1 : 0;
            strncpy(g_msg + g_msg_len, line, strlen(line));
            g_msg_len += strlen(line);
            if (!saw_lf) {
               /* skip */
               return;
            } else {
               /* attach the line feed */
               g_msg_len += 1;
               g_msg[g_msg_len] = '\n';
            }
        } else {
            /* trace is fragmented */
            g_msg_len += 1;
            g_msg[g_msg_len] = '\n';
        }
        ALOGI("%s", g_msg);
        /* reset g_msg and g_msg_len */
        memset(g_msg, 0, LOG_BUF_SIZE);
        g_msg_len = 0;
     } while (check_len > LOG_BUF_SIZE);
#endif
}

void nam_av_log_set_flags(int arg)
{
    flags = arg;
}

#if 0
const struct { const char *name; int level; } log_levels[] = {
    { "quiet"  , AV_LOG_QUIET   },
    { "panic"  , AV_LOG_PANIC   },
    { "fatal"  , AV_LOG_FATAL   },
    { "error"  , AV_LOG_ERROR   },
    { "warning", AV_LOG_WARNING },
    { "info"   , AV_LOG_INFO    },
    { "verbose", AV_LOG_VERBOSE },
    { "debug"  , AV_LOG_DEBUG   },
};

#define AV_LOG_QUIET    -8
#define AV_LOG_PANIC     0
#define AV_LOG_FATAL     8
#define AV_LOG_ERROR    16
#define AV_LOG_WARNING  24
#define AV_LOG_INFO     32
#define AV_LOG_VERBOSE  40
#define AV_LOG_DEBUG    48
#endif

//////////////////////////////////////////////////////////////////////////////////
// constructor and destructor
//////////////////////////////////////////////////////////////////////////////////
/* Mutex manager callback. */
static int lockmgr(void **mtx, enum AVLockOp op)
{
    switch (op) {
    case AV_LOCK_CREATE:
        *mtx = (void *)av_malloc(sizeof(pthread_mutex_t));
        if (!*mtx)
            return 1;
        return !!pthread_mutex_init((pthread_mutex_t *)(*mtx), NULL);
    case AV_LOCK_OBTAIN:
        return !!pthread_mutex_lock((pthread_mutex_t *)(*mtx));
    case AV_LOCK_RELEASE:
        return !!pthread_mutex_unlock((pthread_mutex_t *)(*mtx));
    case AV_LOCK_DESTROY:
        pthread_mutex_destroy((pthread_mutex_t *)(*mtx));
        av_freep(mtx);
        return 0;
    }
    return 1;
}

/**
 * To debug ffmpeg", type this command on the console before starting playback:
 *     setprop debug.nam.ffmpeg 1
 * To disable the debug, type:
 *     setprop debug.nam.ffmpge 0
*/
status_t initFFmpeg() 
{
    status_t ret = OK;
    bool debug_enabled = false;
    char value[PROPERTY_VALUE_MAX];

    pthread_mutex_lock(&s_init_mutex);

    if (property_get("debug.nam.ffmpeg", value, NULL)
        && (!strcmp(value, "1") || !av_strcasecmp(value, "true"))) {
        ALOGI("set ffmpeg debug level to AV_LOG_DEBUG");
        debug_enabled = true;
    }
    if (debug_enabled)
        av_log_set_level(AV_LOG_DEBUG);
    else
        av_log_set_level(AV_LOG_INFO);

    if(s_ref_count == 0) {
        nam_av_log_set_flags(AV_LOG_SKIP_REPEATED);
        av_log_set_callback(nam_av_log_callback);

        /* register all codecs, demux and protocols */
        avcodec_register_all();
#if CONFIG_AVDEVICE
        avdevice_register_all();
#endif
        av_register_all();
        avformat_network_init();

        /* register android source */
        ffmpeg_register_android_source();

        if (av_lockmgr_register(lockmgr)) {
            ALOGE("could not initialize lock manager!");
            ret = NO_INIT;
        }
    }

    // update counter
    s_ref_count++;

    pthread_mutex_unlock(&s_init_mutex);

    return ret;
}

void deInitFFmpeg()
{
    pthread_mutex_lock(&s_init_mutex);

    // update counter
    s_ref_count--;

    if(s_ref_count == 0) {
        av_lockmgr_register(NULL);
        avformat_network_deinit();
    }

    pthread_mutex_unlock(&s_init_mutex);
}

//////////////////////////////////////////////////////////////////////////////////
// parser
//////////////////////////////////////////////////////////////////////////////////
/* H.264 bitstream with start codes, NOT AVC1! */
static int h264_split(AVCodecContext *avctx,
		const uint8_t *buf, int buf_size, int check_compatible_only)
{
    int i;
    uint32_t state = -1;
    int has_sps= 0;
    int has_pps= 0;

    //av_hex_dump(stderr, buf, 100);

    for(i=0; i<=buf_size; i++){
        if((state&0xFFFFFF1F) == 0x107) {
            ALOGI("found NAL_SPS");
            has_sps=1;
        }
        if((state&0xFFFFFF1F) == 0x108) {
            ALOGI("found NAL_PPS");
            has_pps=1;
            if (check_compatible_only)
                return (has_sps & has_pps);
        }
        if((state&0xFFFFFF00) == 0x100
				&& ((state&0xFFFFFF1F) == 0x101
					|| (state&0xFFFFFF1F) == 0x102
					|| (state&0xFFFFFF1F) == 0x105)){
            if(has_pps){
                while(i>4 && buf[i-5]==0) i--;
                return i-4;
            }
        }
        if (i<buf_size)
            state= (state<<8) | buf[i];
    }
    return 0;
}

static int mpegvideo_split(AVCodecContext *avctx,
		const uint8_t *buf, int buf_size, int check_compatible_only)
{
    int i;
    uint32_t state= -1;
    int found=0;

    for(i=0; i<buf_size; i++){
        state= (state<<8) | buf[i];
        if(state == 0x1B3){
            found=1;
        }else if(found && state != 0x1B5 && state < 0x200 && state >= 0x100)
            return i-3;
    }
    return 0;
}

/* split extradata from buf for Android OMXCodec */
int parser_split(AVCodecContext *avctx,
		const uint8_t *buf, int buf_size)
{
    if (!avctx || !buf || buf_size <= 0) {
        ALOGE("parser split, valid params");
        return 0;
    }

    if (avctx->codec_id == AV_CODEC_ID_H264) {
        return h264_split(avctx, buf, buf_size, 0);
    } else if (avctx->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
            avctx->codec_id == AV_CODEC_ID_MPEG4) {
        return mpegvideo_split(avctx, buf, buf_size, 0);
    } else {
        ALOGE("parser split, unsupport the codec, id: 0x%0x", avctx->codec_id);
    }

    return 0;
}

int is_extradata_compatible_with_android(AVCodecContext *avctx)
{
    if (avctx->extradata_size <= 0) {
        ALOGI("extradata_size <= 0, extradata is not compatible with "
				"android decoder, the codec id: 0x%0x", avctx->codec_id);
        return 0;
    }

    if (avctx->codec_id == AV_CODEC_ID_H264
			&& avctx->extradata[0] != 1 /* configurationVersion */) {
        // SPS + PPS
        return !!(h264_split(avctx, avctx->extradata,
					avctx->extradata_size, 1) > 0);
    } else {
        // default, FIXME
        return !!(avctx->extradata_size > 0);
    }
}

//////////////////////////////////////////////////////////////////////////////////
// packet queue
//////////////////////////////////////////////////////////////////////////////////
void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);

    av_init_packet(&q->flush_pkt);
    q->flush_pkt.data = (uint8_t *)&q->flush_pkt;
    q->flush_pkt.size = 0;

    packet_queue_put(q, &q->flush_pkt);
}

void packet_queue_destroy(PacketQueue *q)
{
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

void packet_queue_flush(PacketQueue *q)
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

void packet_queue_end(PacketQueue *q)
{
    packet_queue_flush(q);
}

void packet_queue_abort(PacketQueue *q)
{
    pthread_mutex_lock(&q->mutex);

    q->abort_request = 1;

    pthread_cond_signal(&q->cond);

    pthread_mutex_unlock(&q->mutex);
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;

    /* duplicate the packet */
    if (pkt != &q->flush_pkt && av_dup_packet(pkt) < 0)
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

int packet_queue_put_nullpacket(PacketQueue *q, int stream_index)
{
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
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

//////////////////////////////////////////////////////////////////////////////////
// misc
//////////////////////////////////////////////////////////////////////////////////
bool setup_vorbis_extradata(uint8_t **extradata, int *extradata_size,
        const uint8_t *header_start[3], const int header_len[3])
{
	uint8_t *p = NULL;
    int len = 0;
    int i = 0;

    len = header_len[0] + header_len[1] + header_len[2];
    p = *extradata = (uint8_t *)av_mallocz(64 + len + len/255);
    if (!p) {
        ALOGE("oom for vorbis extradata");
        return false;
	}

    *p++ = 2;
    p += av_xiphlacing(p, header_len[0]);
    p += av_xiphlacing(p, header_len[1]);
    for (i = 0; i < 3; i++) {
        if (header_len[i] > 0) {
            memcpy(p, header_start[i], header_len[i]);
            p += header_len[i];
        }
    }
    *extradata_size = p - *extradata;

    return true;
}

}  // namespace android

