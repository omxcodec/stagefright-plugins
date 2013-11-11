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

#ifndef CODEC_UTILS_H_

#define CODEC_UTILS_H_

#include <unistd.h>
#include <stdlib.h>

#include <utils/Errors.h>
#include <media/stagefright/foundation/ABuffer.h>

#include "ffmpeg_utils.h"

namespace android {

//video
sp<MetaData> setAVCFormat(AVCodecContext *avctx);
sp<MetaData> setH264Format(AVCodecContext *avctx);
sp<MetaData> setMPEG4Format(AVCodecContext *avctx);
sp<MetaData> setH263Format(AVCodecContext *avctx);
sp<MetaData> setMPEG2VIDEOFormat(AVCodecContext *avctx);
sp<MetaData> setVC1Format(AVCodecContext *avctx);
sp<MetaData> setWMV1Format(AVCodecContext *avctx);
sp<MetaData> setWMV2Format(AVCodecContext *avctx);
sp<MetaData> setWMV3Format(AVCodecContext *avctx);
sp<MetaData> setRV20Format(AVCodecContext *avctx);
sp<MetaData> setRV30Format(AVCodecContext *avctx);
sp<MetaData> setRV40Format(AVCodecContext *avctx);
sp<MetaData> setFLV1Format(AVCodecContext *avctx);
sp<MetaData> setHEVCFormat(AVCodecContext *avctx);
//audio
sp<MetaData> setMP2Format(AVCodecContext *avctx);
sp<MetaData> setMP3Format(AVCodecContext *avctx);
sp<MetaData> setVORBISFormat(AVCodecContext *avctx);
sp<MetaData> setAC3Format(AVCodecContext *avctx);
sp<MetaData> setAACFormat(AVCodecContext *avctx);
sp<MetaData> setWMAV1Format(AVCodecContext *avctx);
sp<MetaData> setWMAV2Format(AVCodecContext *avctx);
sp<MetaData> setWMAProFormat(AVCodecContext *avctx);
sp<MetaData> setWMALossLessFormat(AVCodecContext *avctx);
sp<MetaData> setRAFormat(AVCodecContext *avctx);
sp<MetaData> setAPEFormat(AVCodecContext *avctx);
sp<MetaData> setDTSFormat(AVCodecContext *avctx);
sp<MetaData> setFLACFormat(AVCodecContext *avctx);

//Convert H.264 NAL format to annex b
status_t convertNal2AnnexB(uint8_t *dst, size_t dst_size,
        uint8_t *src, size_t src_size, size_t nal_len_size);

}  // namespace android

#endif  // CODEC_UTILS_H_
