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

#ifndef FFMPEG_UTILS_H_

#define FFMPEG_UTILS_H_

#include <unistd.h>
#include <stdlib.h>

#include <utils/Errors.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"

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
#include "libavcodec/xiph.h"
#include "libswresample/swresample.h"

#ifdef __cplusplus
}
#endif

//XXX hack!!!
#define SF_NOPTS_VALUE ((uint64_t)AV_NOPTS_VALUE-1)

namespace android {

//////////////////////////////////////////////////////////////////////////////////
// log
//////////////////////////////////////////////////////////////////////////////////
void nam_av_log_callback(void* ptr, int level, const char* fmt, va_list vl);
void nam_av_log_set_flags(int arg);

//////////////////////////////////////////////////////////////////////////////////
// constructor and destructor
//////////////////////////////////////////////////////////////////////////////////
status_t initFFmpeg();
void deInitFFmpeg();

//////////////////////////////////////////////////////////////////////////////////
// parser
//////////////////////////////////////////////////////////////////////////////////
int is_extradata_compatible_with_android(AVCodecContext *avctx);
int parser_split(AVCodecContext *avctx, const uint8_t *buf, int buf_size);

//////////////////////////////////////////////////////////////////////////////////
// packet queue
//////////////////////////////////////////////////////////////////////////////////

typedef struct PacketQueue {
    AVPacket flush_pkt;
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    int abort_request;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

void packet_queue_init(PacketQueue *q);
void packet_queue_destroy(PacketQueue *q);
void packet_queue_flush(PacketQueue *q);
void packet_queue_end(PacketQueue *q);
void packet_queue_abort(PacketQueue *q);
int packet_queue_put(PacketQueue *q, AVPacket *pkt);
int packet_queue_put_nullpacket(PacketQueue *q, int stream_index);
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

//////////////////////////////////////////////////////////////////////////////////
// misc
//////////////////////////////////////////////////////////////////////////////////
bool setup_vorbis_extradata(uint8_t **extradata, int *extradata_size,
		const uint8_t *header_start[3], const int header_len[3]);

}  // namespace android

#endif  // FFMPEG_UTILS_H_
