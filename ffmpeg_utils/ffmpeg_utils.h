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

#ifdef __cplusplus
extern "C" {
#endif

#include "config.h"
#include "libavcodec/avcodec.h"

#ifdef __cplusplus
}
#endif


namespace android {

void nam_av_log_callback(void* ptr, int level, const char* fmt, va_list vl);
void nam_av_log_set_flags(int arg);

int is_extradata_compatible_with_android(AVCodecContext *avctx);
int parser_split(AVCodecContext *avctx, const uint8_t *buf, int buf_size);

}  // namespace android

#endif  // FFMPEG_UTILS_H_
